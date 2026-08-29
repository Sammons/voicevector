import Foundation
import AppKit

/// Orchestrates one dictation at a time:
/// hotkey → record → transcribe → clean → paste → save → webhook.
/// Published state drives the HUD, status item, and main window refreshes.
@MainActor
final class DictationController: ObservableObject {
    enum State: Equatable {
        case idle
        case recording
        case processing(String)     // "Transcribing…" / "Cleaning up…"
        case failed(String)

        var isBusy: Bool {
            switch self {
            case .recording, .processing: return true
            default: return false
            }
        }
    }

    @Published private(set) var state: State = .idle
    /// Bumped whenever an entry is added/updated so list views reload.
    @Published private(set) var libraryGeneration = 0
    /// Mic level passthrough for the HUD.
    var level: Float { recorder.level }
    var elapsed: TimeInterval { recorder.elapsed }

    private let configStore: ConfigStore
    private let recorder = Recorder()
    private let paste = PasteService()
    private var library: Library
    private var currentSlot: (id: String, audioURL: URL, folder: String)?

    // Streamed segment transcription (silence-gap chunking).
    private let segmentLock = NSLock()
    private var segmentTasks: [Task<String, Error>] = []

    init(configStore: ConfigStore) {
        self.configStore = configStore
        self.library = Library(root: configStore.config.expandedLibraryURL)
    }

    var currentLibrary: Library { library }

    func reloadLibraryRoot() {
        library = Library(root: configStore.config.expandedLibraryURL)
        libraryGeneration += 1
    }

    // MARK: Hotkey actions

    func handle(_ action: TapStateMachine.Action, profileID: UUID? = nil) {
        switch action {
        case .startRecording: startRecording(profileID: profileID)
        case .commit: finishRecording()
        case .discard: discardRecording()
        }
    }

    var isRecording: Bool { state == .recording }

    private var activeProfileID: UUID?

    func startRecording(profileID: UUID? = nil) {
        guard !state.isBusy else { return }
        activeProfileID = profileID ?? configStore.config.dictationProfiles.first?.id
        guard Recorder.permissionGranted else {
            state = .failed("Microphone permission is not granted — open Settings to fix.")
            Chime.shared.playError()
            return
        }
        let folder = configStore.config.activeFolder
        let slot = library.newEntrySlot(folder: folder)
        currentSlot = (slot.id, slot.audioURL, folder)

        // Arm silence-gap streaming: completed phrases transcribe in the
        // background during pauses; cleanup still runs once at the end.
        segmentLock.lock(); segmentTasks = []; segmentLock.unlock()
        let cfg = configStore.config
        let dictationProfile = cfg.dictationProfiles.first(where: { $0.id == activeProfileID })
        let policy = CleanupEngine.effective(profile: dictationProfile, config: cfg)
        let sttProfile = policy.stt
        if cfg.chunkedTranscription,
           let sttProfile,
           !policy.enabled || !CleanupEngine.singlePassEligible(
               stt: sttProfile, cleanupProfile: policy.provider,
               mode: policy.config.mode) {
            let client = ProviderClient(profile: sttProfile)
            let vocabulary = CleanupEngine.parseVocabulary(policy.config.vocabulary)
            recorder.chunking = true
            recorder.onSegment = { [weak self] data, index in
                guard let self else { return }
                let task = Task {
                    try await client.transcribe(audioData: data, filename: "segment\(index).wav",
                                                vocabulary: vocabulary).text
                }
                self.segmentLock.lock()
                self.segmentTasks.append(task)
                self.segmentLock.unlock()
            }
        } else {
            recorder.chunking = false
            recorder.onSegment = nil
        }

        do {
            try recorder.start(to: slot.audioURL)
            state = .recording
            if configStore.config.playSounds { Chime.shared.playStart() }
        } catch {
            currentSlot = nil
            state = .failed("Could not start recording: \(error.localizedDescription)")
            Chime.shared.playError()
        }
    }

    func finishRecording() {
        guard state == .recording, let slot = currentSlot else { return }
        let duration = recorder.stop()
        let tailStartByte = recorder.tailStartByte
        currentSlot = nil
        if configStore.config.playSounds { Chime.shared.playStop() }

        // Sub-half-second recordings are almost certainly accidental.
        guard duration >= 0.5 else {
            try? FileManager.default.removeItem(at: slot.audioURL)
            state = .idle
            return
        }

        state = .processing("Transcribing…")
        let config = configStore.config
        let profileID = activeProfileID
        Task { await self.process(slot: slot, duration: duration, config: config,
                                  tailStartByte: tailStartByte, profileID: profileID) }
    }

    func discardRecording() {
        guard state == .recording else { return }
        recorder.discard()
        currentSlot = nil
        state = .idle
    }

    // MARK: Pipeline

    private func process(slot: (id: String, audioURL: URL, folder: String),
                         duration: Double, config: AppConfig, tailStartByte: UInt32 = 0,
                         profileID: UUID? = nil) async {
        var entry = Entry(id: slot.id, folder: slot.folder, date: Date(), duration: duration,
                          sttLabel: "", cleanupLabel: "", status: "complete", cleaned: "", raw: "")

        let dictationProfile = config.dictationProfiles.first(where: { $0.id == profileID })
        let policy = CleanupEngine.effective(profile: dictationProfile, config: config)
        guard let sttProfile = policy.stt else {
            entry.status = "error: no transcription provider configured"
            library.save(entry)
            finish(with: .failed("No transcription provider configured — open Settings."), bumpLibrary: true)
            return
        }
        entry.sttLabel = "\(sttProfile.name)/\(sttProfile.sttModel)"

        // 0. Single-pass: same provider + same model for both stages fuses
        //    transcription and cleanup into one audio chat call. Falls back to
        //    the two-pass pipeline on any failure.
        var singlePassed = false
        if policy.enabled, CleanupEngine.singlePassEligible(stt: sttProfile,
                                                            cleanupProfile: policy.provider,
                                                            mode: policy.config.mode) {
            do {
                let cleaned = try await CleanupEngine.cleanupSinglePass(
                    audioURL: slot.audioURL, config: policy.config, profile: sttProfile)
                guard !cleaned.isEmpty else {
                    throw HTTPError.badResponse("empty single-pass result")
                }
                entry.raw = cleaned // no separate raw transcript in single-pass
                entry.cleaned = cleaned
                entry.sttLabel = "\(sttProfile.name)/\(sttProfile.chatModel) (single-pass)"
                entry.cleanupLabel = "single-pass"
                singlePassed = true
            } catch {
                Log.error("Single-pass failed, falling back to two-pass: \(error.localizedDescription)")
            }
        }

        if !singlePassed {
        // 1. Transcribe — preferring segments streamed during silence pauses;
        //    any failure falls back to transcribing the full recording.
        let vocabulary = CleanupEngine.parseVocabulary(policy.config.vocabulary)
        let pending: [Task<String, Error>] = {
            segmentLock.lock(); defer { segmentLock.unlock() }
            let tasks = segmentTasks; segmentTasks = []; return tasks
        }()
        if !pending.isEmpty {
            do {
                let client = ProviderClient(profile: sttProfile)
                var parts: [String] = []
                for task in pending { parts.append(try await task.value) }
                let totalBytes = UInt32(max(0, (try? FileManager.default
                    .attributesOfItem(atPath: slot.audioURL.path)[.size] as? Int ?? 44) ?? 44) - 44)
                if totalBytes > tailStartByte {
                    let tail = try WavWriter.sliceWav(fileURL: slot.audioURL,
                                                      fromByte: tailStartByte, toByte: totalBytes)
                    parts.append(try await client.transcribe(audioData: tail, filename: "tail.wav",
                                                             vocabulary: vocabulary).text)
                }
                let joined = parts.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
                    .filter { !$0.isEmpty }
                    .joined(separator: " ")
                if !joined.isEmpty {
                    entry.raw = joined
                    entry.sttLabel += " (streamed \(pending.count + 1) parts)"
                }
            } catch {
                Log.error("Streamed transcription failed; falling back to full file: \(error.localizedDescription)")
            }
        }
        if entry.raw.isEmpty {
        do {
            let client = ProviderClient(profile: sttProfile)
            let result = try await client.transcribe(audioURL: slot.audioURL, vocabulary: vocabulary)
            entry.raw = result.text.trimmingCharacters(in: .whitespacesAndNewlines)
        } catch {
            entry.status = "error: transcription failed — \(error.localizedDescription)"
            library.save(entry)
            finish(with: .failed("Transcription failed: \(error.localizedDescription)"), bumpLibrary: true)
            return
        }
        } // end full-file fallback

        guard !entry.raw.isEmpty else {
            entry.status = "error: empty transcript"
            library.save(entry)
            finish(with: .failed("The recording produced an empty transcript."), bumpLibrary: true)
            return
        }

        // 2. Clean up (optional; falls back to raw on failure — loudly).
        entry.cleaned = entry.raw
        if !policy.enabled {
            if let dictationProfile {
                entry.cleanupLabel = "skipped — \(dictationProfile.name) hotkey"
            }
        } else {
            if let cleanupProfile = policy.provider,
               cleanupProfile.kind.supportsChat, !cleanupProfile.chatModel.isEmpty {
                state = .processing("Cleaning up…")
                entry.cleanupLabel = "\(cleanupProfile.name)/\(cleanupProfile.chatModel)"
                do {
                    entry.cleaned = try await CleanupEngine.cleanup(raw: entry.raw, config: policy.config,
                                                                    profile: cleanupProfile)
                } catch {
                    entry.cleanupLabel += " (failed — raw used)"
                    Log.error("Cleanup failed, using raw transcript: \(error.localizedDescription)")
                    notify(title: "Cleanup failed — raw transcript used",
                           body: error.localizedDescription)
                }
            } else {
                entry.cleanupLabel = "not run — no cleanup provider selected"
                notify(title: "Cleanup skipped",
                       body: "No cleanup provider is selected. Pick one in Settings → Dictation.")
            }
        }
        } // end two-pass pipeline

        library.save(entry)
        libraryGeneration += 1

        // 3. Paste into the frontmost app.
        state = .processing("Pasting…")
        let outcome = await paste.insert(entry.cleaned, autoPaste: config.autoPaste,
                                         preferAppleScript: config.appleScriptPaste)
        switch outcome {
        case .pasted:
            finish(with: .idle)
        case .copiedOnly(let reason):
            notify(title: "Transcript copied", body: "\(reason). Press ⌘V to insert it.")
            finish(with: .idle)
        }

        // 4. Webhook (fire and forget).
        if let webhook = config.folderWebhooks[slot.folder], webhook.enabled {
            let finished = entry
            let audioURL = slot.audioURL
            Task.detached { await WebhookSender.send(entry: finished, audioURL: audioURL, config: webhook) }
        }
    }

    private func finish(with newState: State, bumpLibrary: Bool = false) {
        state = newState
        if bumpLibrary { libraryGeneration += 1 }
        if case .failed(let message) = newState {
            Chime.shared.playError()
            notify(title: "Dictation failed", body: message)
            // Clear the failure banner after a few seconds.
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 5_000_000_000)
                if case .failed = self.state { self.state = .idle }
            }
        }
    }

    // MARK: Retry from the library list

    func retry(entry: Entry) {
        guard !state.isBusy else { return }
        let audioURL = library.audioURL(entry)
        guard FileManager.default.fileExists(atPath: audioURL.path) else { return }
        state = .processing("Transcribing…")
        let config = configStore.config
        Task {
            await self.process(slot: (entry.id, audioURL, entry.folder),
                               duration: entry.duration, config: config)
        }
    }

    private func notify(title: String, body: String) {
        // NSUserNotification is deprecated but UserNotifications requires a
        // bundle with an Info.plist — which we have; still, keep it simple and
        // visible: floating alert via the status item would be overkill, so use
        // the notification center through the modern API.
        Notifier.show(title: title, body: body)
    }
}
