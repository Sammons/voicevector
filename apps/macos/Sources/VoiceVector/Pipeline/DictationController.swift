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
        /// A draft is staged in the HUD: the hotkey records a spoken change,
        /// ⏎ pastes, Esc discards.
        case reviewing
        case failed(String)

        var isBusy: Bool {
            switch self {
            case .recording, .processing: return true
            default: return false
            }
        }
    }

    /// A staged dictation awaiting spoken revisions / ⏎ / Esc.
    /// Where the router decided the draft should go.
    struct RouteTarget {
        var machine: String
        var window: UInt32          // 0 = whatever is focused there
        var peer: PeerRef?          // nil = this machine
        var label: String           // "Slack — #general on crankshaft"
    }

    struct ReviewSession {
        var entry: Entry
        var slot: (id: String, audioURL: URL, folder: String)
        var config: AppConfig
        var profile: DictationProfile?
        var policy: CleanupEngine.EffectiveCleanup
        var screenshots: ScreenshotSet?
        var windows: [WindowInfo] = []
        var routeTarget: RouteTarget?
        var revisions = 0
    }

    @Published private(set) var state: State = .idle
    /// Text shown in the HUD staging area while a review session is active
    /// (kept through command recordings and revisions).
    @Published private(set) var reviewDraft: String?
    private var review: ReviewSession?
    private var commandSlot: URL?
    /// Screenshots (one per display) taken when the hotkey fired, for
    /// cleanup/review context; saved beside the entry so a retry reuses them.
    private var pendingScreenshots: ScreenshotSet?
    /// Window list captured when the hotkey fired (router hotkeys only).
    private var pendingWindows: [WindowInfo] = []
    /// Router verdict shown in the staging card ("→ Slack on crankshaft").
    @Published var reviewRoute: String?
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

    /// Pushes the mic warm policy from config into the recorder. Only opens
    /// the input when the microphone permission is already granted, so it
    /// never triggers the system prompt on its own.
    func applyWarmPolicy() {
        let config = configStore.config
        recorder.warmAfterRecording = config.keepMicWarmAfterRecording
        recorder.alwaysWarm = config.keepMicAlwaysWarm && Recorder.permissionGranted
        recorder.applyWarmPolicy()
    }

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
        if state == .reviewing { startCommandRecording(); return }
        activeProfileID = profileID ?? configStore.config.dictationProfiles.first?.id
        guard Recorder.permissionGranted else {
            state = .failed("Microphone permission is not granted — open Settings to fix.")
            Chime.shared.playError()
            return
        }
        let folder = configStore.config.activeFolder
        let slot = library.newEntrySlot(folder: folder)
        currentSlot = (slot.id, slot.audioURL, folder)

        // Screenshots of what the user is looking at, before anything moves.
        pendingScreenshots = nil
        if let profile = configStore.config.dictationProfiles.first(where: { $0.id == activeProfileID }),
           profile.screenshotContext {
            pendingScreenshots = ScreenCapture.allScreens()
            library.saveScreenshots(id: slot.id, folder: folder, pendingScreenshots)
        }
        pendingWindows = []
        if let profile = configStore.config.dictationProfiles.first(where: { $0.id == activeProfileID }),
           profile.routerEnabled, profile.reviewBeforePaste {
            pendingWindows = WindowInventory.list()
        }

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

        // Show the HUD right away; the device opens in the background (an
        // external USB interface can take a beat) and reports back here.
        state = .recording
        let slotID = slot.id
        recorder.start(to: slot.audioURL) { [weak self] error in
            guard let self else { return }
            if let error {
                if self.currentSlot?.id == slotID {
                    self.currentSlot = nil
                    self.state = .failed("Could not start recording: \(error.localizedDescription)")
                } else {
                    self.state = .failed("Could not start recording: \(error.localizedDescription)")
                }
                Chime.shared.playError()
            } else if self.state == .recording, self.currentSlot?.id == slotID,
                      self.configStore.config.playSounds {
                Chime.shared.playStart()
            }
        }
    }

    func finishRecording() {
        if review != nil, commandSlot != nil { finishCommandRecording(); return }
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
        if let slot = currentSlot, review == nil || commandSlot == nil {
            library.deleteScreenshots(id: slot.id, folder: slot.folder)
            pendingScreenshots = nil
        }
        currentSlot = nil
        if review != nil, commandSlot != nil {
            commandSlot = nil
            state = .reviewing
        } else {
            state = .idle
        }
    }

    // MARK: Review session (staged draft, spoken revisions)

    private func beginReview(entry: Entry, slot: (id: String, audioURL: URL, folder: String),
                             config: AppConfig, profile: DictationProfile?,
                             policy: CleanupEngine.EffectiveCleanup) {
        review = ReviewSession(entry: entry, slot: slot, config: config, profile: profile,
                               policy: policy, screenshots: pendingScreenshots, windows: pendingWindows)
        reviewDraft = entry.cleaned
        reviewRoute = nil
        state = .reviewing
        if profile?.routerEnabled == true, let session = review {
            reviewRoute = "Routing…"
            Task { await self.runRouter(session) }
        }
    }

    // MARK: AI routing (docs/multi-machine.md)

    /// The router model: the profile's choice, else the review provider.
    private func routerProvider(for session: ReviewSession) -> ProviderProfile? {
        if let id = session.profile?.routerProviderID,
           let p = session.config.providers.first(where: { $0.id == id }) { return p }
        return reviewProvider(for: session)
    }

    /// Gathers local + peer contexts, asks the router, stores the verdict on
    /// the SAME review session (guarded by slot id across every await, so a
    /// verdict for dictation A can never land on a later dictation B).
    private func runRouter(_ session: ReviewSession) async {
        let id = session.slot.id
        guard let provider = routerProvider(for: session) else { clearRoute(id); return }
        let mm = session.config.multiMachine
        let machineName = mm.resolvedMachineName
        let local = PeerService.localContext(machineName: machineName, windows: session.windows,
                                             screens: session.screenshots, captureScreens: false)
        var contexts = [local]
        let peers = mm.peers.filter { !$0.address.isEmpty }
        if !peers.isEmpty {
            // Fetch each peer's screens/windows when it shares them; otherwise
            // it still appears as a routable machine (window 0 = its focus), so
            // "send this to <machine>" works without screen-sharing permission.
            let fetched = await withTaskGroup(of: (PeerRef, MachineContext?).self) { group in
                for peer in peers {
                    group.addTask {
                        let context = await withCheckedContinuation { done in
                            PeerService.shared.fetchContext(peer: peer) { done.resume(returning: $0) }
                        }
                        return (peer, context)
                    }
                }
                var found: [(PeerRef, MachineContext?)] = []
                for await pair in group { found.append(pair) }
                return found
            }
            for (peer, context) in fetched {
                contexts.append(context ?? MachineContext(
                    machine: peer.name, isLocal: false, fingerprint: peer.fingerprint,
                    windows: [], windowLines: "", screens: []))
            }
        }
        guard review?.slot.id == id else { return }   // this review ended/replaced while gathering
        let message = CleanupEngine.routerMessage(
            draft: session.entry.cleaned, spoken: session.entry.raw,
            machines: contexts.map { ($0.machine, $0.isLocal, $0.windowLines) })
        let images = contexts.flatMap(\.screens)
        let catalog = contexts.map {
            CleanupEngine.RouterCatalog(machine: $0.machine, windowIDs: Set($0.windows.map(\.id)))
        }
        do {
            let client = ProviderClient(profile: provider)
            var correction: String?
            // The model must pick a machine + window that were actually offered;
            // an invalid or hallucinated choice is bounced back with the reason,
            // up to a few tries, before we give up and paste locally.
            for _ in 0..<3 {
                let user = correction.map { message + "\n\n" + $0 } ?? message
                let reply: String
                do { reply = try await client.chat(system: CleanupEngine.routerPrompt, user: user, images: images) }
                catch { reply = try await client.chat(system: CleanupEngine.routerPrompt, user: user) }
                guard review?.slot.id == id else { return }
                guard let verdict = CleanupEngine.parseRouterVerdict(reply),
                      CleanupEngine.routerVerdictValid(verdict, catalog: catalog) else {
                    correction = CleanupEngine.routerCorrection(reply)
                    continue
                }
                apply(verdict: verdict, contexts: contexts, sessionID: id, machineName: machineName)
                return
            }
            Log.error("Router gave no valid destination after retries; using the local paste.")
            clearRoute(id)
        } catch {
            Log.error("Router failed: \(error.localizedDescription)")
            clearRoute(id)
        }
    }

    /// Clears the route banner only if `id` is still the active review.
    private func clearRoute(_ id: String) { if review?.slot.id == id { reviewRoute = nil } }

    private func apply(verdict: CleanupEngine.RouterVerdict, contexts: [MachineContext],
                       sessionID: String, machineName: String) {
        guard review?.slot.id == sessionID else { return }
        guard let context = contexts.first(where: { $0.machine == verdict.machine }) else {
            reviewRoute = nil; return
        }
        let window = context.windows.first(where: { $0.id == verdict.window })
        let windowLabel = window.map { $0.title.isEmpty ? $0.app : "\($0.app) — \($0.title)" }
        if context.isLocal {
            if let window, let windowLabel {
                review?.routeTarget = RouteTarget(machine: machineName, window: window.id,
                                                  peer: nil, label: windowLabel)
                reviewRoute = "→ " + windowLabel
            } else {
                review?.routeTarget = nil
                reviewRoute = nil     // focused window — the normal paste
            }
        } else if let peer = review?.config.multiMachine.peers.first(where: { $0.fingerprint == context.fingerprint }) {
            let label = (windowLabel.map { "\($0) on " } ?? "") + context.machine
            review?.routeTarget = RouteTarget(machine: context.machine,
                                              window: window?.id ?? 0, peer: peer, label: label)
            reviewRoute = "→ " + label
        } else {
            reviewRoute = nil
        }
    }

    /// The reviewer model: the profile's choice, else the cleanup provider.
    private func reviewProvider(for session: ReviewSession) -> ProviderProfile? {
        if let id = session.profile?.reviewProviderID,
           let p = session.config.providers.first(where: { $0.id == id }) { return p }
        if let p = session.policy.provider, p.kind.supportsChat, !p.chatModel.isEmpty { return p }
        return session.config.providers.first { $0.kind.supportsChat && !$0.chatModel.isEmpty }
    }

    private func startCommandRecording() {
        guard review != nil, state == .reviewing else { return }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("vv-command-\(UUID().uuidString).wav")
        commandSlot = url
        recorder.chunking = false
        recorder.onSegment = nil
        state = .recording
        recorder.start(to: url) { [weak self] error in
            guard let self else { return }
            if let error {
                self.commandSlot = nil
                self.state = .reviewing
                self.notify(title: "Could not record the change", body: error.localizedDescription)
                Chime.shared.playError()
            } else if self.state == .recording, self.configStore.config.playSounds {
                Chime.shared.playStart()
            }
        }
    }

    private func finishCommandRecording() {
        guard let session = review, let url = commandSlot else { return }
        let duration = recorder.stop()
        commandSlot = nil
        if configStore.config.playSounds { Chime.shared.playStop() }
        guard duration >= 0.5 else {
            try? FileManager.default.removeItem(at: url)
            state = .reviewing
            return
        }
        state = .processing("Hearing the change…")
        Task { await self.applyCommand(audioURL: url, session: session) }
    }

    private func applyCommand(audioURL: URL, session: ReviewSession) async {
        defer { try? FileManager.default.removeItem(at: audioURL) }
        guard let stt = session.policy.stt else {
            state = .reviewing
            notify(title: "No transcription provider", body: "Pick one in Settings → Dictation.")
            return
        }
        guard let reviewer = reviewProvider(for: session) else {
            state = .reviewing
            notify(title: "No review model", body: "Pick a cleanup or review model for this hotkey.")
            return
        }
        do {
            let vocabulary = CleanupEngine.parseVocabulary(session.policy.config.vocabulary)
            let instruction = try await ProviderClient(profile: stt)
                .transcribe(audioURL: audioURL, vocabulary: vocabulary).text
                .trimmingCharacters(in: .whitespacesAndNewlines)
            guard !instruction.isEmpty, let draft = reviewDraft else {
                state = .reviewing
                notify(title: "Didn't catch that", body: "No change was heard — press the hotkey and say it again.")
                Chime.shared.playError()
                return
            }
            state = .processing("Revising…")
            let revised = try await CleanupEngine.revise(draft: draft, instruction: instruction,
                                                         vocabulary: session.policy.config.vocabulary,
                                                         profile: reviewer, images: session.screenshots?.attachments ?? [])
            reviewDraft = revised
            review?.revisions += 1
            review?.entry.cleanupLabel = session.entry.cleanupLabel
                + " · review \(reviewer.name)/\(reviewer.chatModel) ×\(session.revisions + 1)"
        } catch {
            notify(title: "Revision failed", body: error.localizedDescription)
            Chime.shared.playError()
        }
        state = .reviewing
    }

    /// ⏎ while reviewing: save the draft and paste it (where the router said).
    func acceptReview() {
        guard state == .reviewing, var session = review, let draft = reviewDraft else { return }
        review = nil
        reviewDraft = nil
        reviewRoute = nil
        session.entry.cleaned = draft
        if let target = session.routeTarget {
            session.entry.cleanupLabel += " · routed to \(target.label)"
        }
        library.save(session.entry)
        libraryGeneration += 1
        let routed = session
        Task { await self.deliverRouted(session: routed) }
    }

    private func deliverRouted(session: ReviewSession) async {
        let submit = session.profile?.autoSubmit == true
        guard let target = session.routeTarget else {
            await deliver(entry: session.entry, slot: session.slot, config: session.config, submit: submit)
            return
        }
        if let peer = target.peer {
            state = .processing("Sending to \(target.machine)…")
            let text = session.entry.cleaned
            let error: String? = await withCheckedContinuation { done in
                PeerService.shared.deliver(text: text, window: target.window, submit: submit, peer: peer) { done.resume(returning: $0) }
            }
            if let error {
                notify(title: "Could not deliver to \(target.machine)", body: error + " — pasting here instead.")
                await deliver(entry: session.entry, slot: session.slot, config: session.config, submit: submit)
            } else {
                finish(with: .idle)
                if let webhook = session.config.folderWebhooks[session.slot.folder], webhook.enabled {
                    let finished = session.entry
                    let audioURL = session.slot.audioURL
                    Task.detached { await WebhookSender.send(entry: finished, audioURL: audioURL, config: webhook) }
                }
            }
        } else {
            state = .processing("Pasting…")
            // Only paste once the target window is genuinely frontmost. If the
            // system refused the focus change, pasting now would dump the text
            // into whatever the user had focused (e.g. the terminal), so copy
            // it and tell them instead.
            if target.window != 0, !WindowInventory.activate(windowID: target.window) {
                paste.copyToClipboard(session.entry.cleaned)
                notify(title: "Couldn't focus \(target.label)",
                       body: "The text is on the clipboard — click \(target.label) and press ⌘V.")
                finish(with: .idle)
                return
            }
            try? await Task.sleep(nanoseconds: 350_000_000)   // let focus settle
            await deliver(entry: session.entry, slot: session.slot, config: session.config, submit: submit)
        }
    }

    /// Inbound routed text from a paired machine: activate the window (when
    /// we know it), paste, and save a routed entry to the library.
    func receiveRoutedText(_ text: String, window: UInt32, submit: Bool, from machine: String,
                           completion: @escaping (Bool, String) -> Void) {
        // Not while the local user is recording OR mid-review — pasting would
        // yank focus and the clipboard out from under them.
        guard !state.isBusy, state != .reviewing else { completion(false, "busy dictating"); return }
        Task {
            if window != 0, !WindowInventory.activate(windowID: window) {
                Log.error("Routed window \(window) not found; pasting into the focused window.")
            }
            try? await Task.sleep(nanoseconds: 350_000_000)
            let config = self.configStore.config
            let outcome = await self.paste.insert(text, autoPaste: config.autoPaste,
                                                  preferAppleScript: config.appleScriptPaste)
            let folder = config.activeFolder
            let slot = self.library.newEntrySlot(folder: folder)
            var entry = Entry(id: slot.id, folder: folder, date: Date(), duration: 0,
                              sttLabel: "routed from \(machine)", cleanupLabel: "",
                              status: "complete", cleaned: text, raw: text)
            switch outcome {
            case .pasted:
                if submit { await self.paste.pressReturn(preferAppleScript: config.appleScriptPaste) }
                completion(true, "")
            case .copiedOnly(let reason):
                entry.status = "complete (copied only)"
                self.notify(title: "Text from \(machine) copied", body: "\(reason). Press ⌘V to insert it.")
                // Tell the sender the truth: it landed on the clipboard, not in the app.
                completion(false, "the receiving machine could not paste (\(reason)); it copied the text instead")
            }
            self.library.save(entry)
            self.libraryGeneration += 1
        }
    }

    /// Esc while reviewing: keep the entry (with the draft) but don't paste.
    func discardReview() {
        guard state == .reviewing, var session = review else { return }
        review = nil
        reviewRoute = nil
        let draft = reviewDraft
        reviewDraft = nil
        if let draft { session.entry.cleaned = draft }
        session.entry.status = "complete (not pasted)"
        library.save(session.entry)
        libraryGeneration += 1
        state = .idle
    }

    // MARK: Pipeline

    private func process(slot: (id: String, audioURL: URL, folder: String),
                         duration: Double, config: AppConfig, tailStartByte: UInt32 = 0,
                         profileID: UUID? = nil) async {
        var entry = Entry(id: slot.id, folder: slot.folder, date: Date(), duration: duration,
                          sttLabel: "", cleanupLabel: "", status: "complete", cleaned: "", raw: "")
        entry.attach(pendingScreenshots)

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
                                                                    profile: cleanupProfile,
                                                                    images: pendingScreenshots?.attachments ?? [],
                                                                    stripRouting: dictationProfile?.routerEnabled == true)
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

        // Staged review: hold the draft in the HUD until ⏎ / Esc.
        if let dictationProfile, dictationProfile.reviewBeforePaste, profileID != nil {
            beginReview(entry: entry, slot: slot, config: config, profile: dictationProfile, policy: policy)
            return
        }
        await deliver(entry: entry, slot: slot, config: config,
                      submit: dictationProfile?.autoSubmit == true)
    }

    /// 3. Paste into the frontmost app, then 4. webhook (fire and forget).
    private func deliver(entry: Entry, slot: (id: String, audioURL: URL, folder: String),
                         config: AppConfig, submit: Bool = false) async {
        state = .processing("Pasting…")
        let outcome = await paste.insert(entry.cleaned, autoPaste: config.autoPaste,
                                         preferAppleScript: config.appleScriptPaste)
        switch outcome {
        case .pasted:
            if submit { await paste.pressReturn(preferAppleScript: config.appleScriptPaste) }
            finish(with: .idle)
        case .copiedOnly(let reason):
            notify(title: "Transcript copied", body: "\(reason). Press ⌘V to insert it.")
            finish(with: .idle)
        }

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
        // Reuse the screenshots saved with the entry; the screen has moved on.
        pendingScreenshots = library.loadScreenshots(entry)
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
