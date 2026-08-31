import Foundation
import CoreGraphics
import CryptoKit

/// Built-in test suite (`VoiceVector --self-test`) covering the pure logic.
/// Command Line Tools ship no XCTest, so the app carries its own tiny harness
/// rather than pulling in a dependency.
enum SelfTest {
    private static var failures: [String] = []
    private static var count = 0

    private static func expect(_ condition: Bool, _ label: String,
                               file: String = #file, line: Int = #line) {
        count += 1
        if !condition {
            failures.append("\((file as NSString).lastPathComponent):\(line)  \(label)")
        }
    }

    static func run() -> Never {
        testMultipart()
        testTapStateMachine()
        testLibraryMarkdown()
        testLibraryFiles()
        testConfig()
        testWavWriter()

        if failures.isEmpty {
            print("SELF-TEST PASSED (\(count) assertions)")
            exit(0)
        }
        print("SELF-TEST FAILED — \(failures.count)/\(count) assertions failed:")
        for failure in failures { print("  ✗ \(failure)") }
        exit(1)
    }

    // MARK: Multipart

    private static func testMultipart() {
        var form = Multipart(boundary: "BOUND")
        form.addField(name: "model", value: "whisper-1")
        form.addFile(name: "file", filename: "a.wav", contentType: "audio/wav",
                     data: Data([0x01, 0x02]))
        let body = String(decoding: form.encoded(), as: UTF8.self)
        expect(body.contains("--BOUND\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"),
               "multipart field encoding")
        expect(body.contains("name=\"file\"; filename=\"a.wav\""), "multipart file disposition")
        expect(body.contains("Content-Type: audio/wav"), "multipart file content type")
        expect(body.hasSuffix("--BOUND--\r\n"), "multipart terminator")
    }

    // MARK: Tap state machine

    private static func testTapStateMachine() {
        // Hold-to-talk
        var machine = TapStateMachine(startMode: .doubleTap)
        expect(machine.keyDown(at: 0) == [.startRecording], "hold: down starts recording")
        expect(machine.keyUp(at: 1.0) == [.commit], "hold: long release commits")
        expect(machine.phase == .idle, "hold: back to idle")

        // Double-tap toggle
        machine = TapStateMachine(startMode: .doubleTap)
        expect(machine.keyDown(at: 0) == [.startRecording], "double: first down records")
        expect(machine.keyUp(at: 0.1).isEmpty, "double: first short tap waits")
        expect(machine.keyDown(at: 0.2).isEmpty, "double: second tap continues")
        expect(machine.keyUp(at: 0.3).isEmpty, "double: latched after second tap")
        expect(machine.phase == .latched, "double: latched phase")
        expect(machine.keyDown(at: 5.0) == [.commit], "double: stop tap commits")
        expect(machine.keyUp(at: 5.1).isEmpty, "double: drain up")
        expect(machine.phase == .idle, "double: idle after stop")

        // Stray tap discards after window
        machine = TapStateMachine(startMode: .doubleTap)
        _ = machine.keyDown(at: 0)
        _ = machine.keyUp(at: 0.1)
        expect(machine.pendingDeadline != nil, "stray: deadline pending")
        expect(machine.expire(at: 0.6) == [.discard], "stray: expiry discards")
        expect(machine.phase == .idle, "stray: idle after discard")

        // Single-tap mode
        machine = TapStateMachine(startMode: .singleTap)
        expect(machine.keyDown(at: 0) == [.startRecording], "single: down records")
        expect(machine.keyUp(at: 0.1).isEmpty, "single: quick release latches")
        expect(machine.phase == .latched, "single: latched")
        expect(machine.keyDown(at: 2.0) == [.commit], "single: next tap commits")
        _ = machine.keyUp(at: 2.1)
        expect(machine.phase == .idle, "single: idle at end")

        // Second press held long = hold commit
        machine = TapStateMachine(startMode: .doubleTap)
        _ = machine.keyDown(at: 0)
        _ = machine.keyUp(at: 0.1)
        _ = machine.keyDown(at: 0.2)
        expect(machine.keyUp(at: 1.5) == [.commit], "double+hold: commits on release")

        // Esc cancel
        machine = TapStateMachine(startMode: .doubleTap)
        _ = machine.keyDown(at: 0)
        expect(machine.cancel() == [.discard], "cancel discards")
        expect(machine.phase == .idle, "cancel returns to idle")
    }

    // MARK: Markdown round trip

    private static func testLibraryMarkdown() {
        let entry = Entry(id: "20260825-120000", folder: "Inbox",
                          date: Library.dateFormatter.date(from: "2026-08-25T12:00:00Z")!,
                          duration: 12.4, sttLabel: "ElevenLabs/scribe_v2",
                          cleanupLabel: "Fireworks/gpt-oss-20b", status: "complete",
                          cleaned: "Hello world.\n\n- bullet one\n- bullet two",
                          raw: "um hello world uh bullet one bullet two")
        let parsed = Library.parse(markdown: Library.render(entry), id: entry.id, folder: entry.folder)
        expect(parsed.cleaned == entry.cleaned, "markdown: cleaned round trip")
        expect(parsed.raw == entry.raw, "markdown: raw round trip")
        expect(abs(parsed.duration - 12.4) < 0.01, "markdown: duration")
        expect(parsed.sttLabel == entry.sttLabel, "markdown: stt label")
        expect(parsed.status == "complete", "markdown: status")
        expect(parsed.date == entry.date, "markdown: date")

        var shot = entry
        shot.attach(ScreenshotSet(images: [Data([1]), Data([2])], activeIndex: 0, outlined: true))
        let shotRendered = Library.render(shot)
        expect(shotRendered.contains("status: complete\nscreenshots: 2\nactiveScreenshot: 1\nscreenshotOutline: true\n---\n"),
               "markdown: screenshot keys rendered after status")
        let shotParsed = Library.parse(markdown: shotRendered, id: entry.id, folder: entry.folder)
        expect(shotParsed.screenshots == 2 && shotParsed.activeScreenshot == 1 && shotParsed.screenshotOutline,
               "markdown: screenshot keys round trip")
        expect(!Library.render(entry).contains("screenshots:"), "markdown: no screenshot keys without screenshots")

        let bare = Library.parse(markdown: "---\ndate: 2026-08-25T12:00:00Z\nstatus: complete\n---\n\nJust text\n",
                                 id: "x", folder: "Inbox")
        expect(bare.cleaned == "Just text", "markdown: bare cleaned")
        expect(bare.raw == "Just text", "markdown: bare raw mirrors cleaned")
    }

    // MARK: Library file operations

    private static func testLibraryFiles() {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("vv-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let library = Library(root: root)
        expect(library.folderNames() == ["Inbox"], "library: Inbox auto-created")

        try? library.createFolder("Work Notes")
        expect(library.folderNames() == ["Inbox", "Work Notes"], "library: folder created")

        let slot = library.newEntrySlot(folder: "Inbox")
        var entry = Entry(id: slot.id, folder: "Inbox", date: Date(), duration: 1,
                          sttLabel: "t", cleanupLabel: "", status: "complete",
                          cleaned: "hi", raw: "hi")
        library.save(entry)
        expect(library.entryCount(folder: "Inbox") == 1, "library: entry saved")
        expect(library.entries(folder: "Inbox", offset: 0, limit: 10).first?.cleaned == "hi",
               "library: entry listed")

        entry.cleaned = "updated"
        library.save(entry)
        expect(library.entry(folder: "Inbox", id: entry.id)?.cleaned == "updated", "library: entry updated")

        let set = ScreenshotSet(images: [Data([0xFF, 0xD8, 1]), Data([0xFF, 0xD8, 2])], activeIndex: 1, outlined: false)
        library.saveScreenshots(id: entry.id, folder: "Inbox", set)
        entry.attach(set)
        library.save(entry)
        let loaded = library.loadScreenshots(library.entry(folder: "Inbox", id: entry.id)!)
        expect(loaded?.images == set.images && loaded?.activeIndex == 1 && loaded?.outlined == false,
               "library: screenshots saved beside the entry and reloaded")
        expect(loaded?.attachments.map(\.caption) == ["Display 1 of 2.", "Display 2 of 2 — ACTIVE: the dictated text will be inserted here."],
               "library: reloaded captions")
        expect(library.entryCount(folder: "Inbox") == 1, "library: screenshot files are not entries")

        library.delete(entry)
        expect(library.entryCount(folder: "Inbox") == 0, "library: entry deleted")
        expect(!FileManager.default.fileExists(atPath: library.folderURL("Inbox").appendingPathComponent(entry.screenshotFilename(1)).path),
               "library: screenshots deleted with the entry")
    }

    // MARK: Config

    private static func testConfig() {
        var config = AppConfig()
        config.providers = [.preset(.elevenLabs), .preset(.fireworks)]
        config.sttProviderID = config.providers[0].id
        config.folderWebhooks["Inbox"] = WebhookConfig(url: "https://x.test/h", includeAudio: true,
                                                       enabled: true)
        do {
            let data = try JSONEncoder().encode(config)
            let decoded = try JSONDecoder().decode(AppConfig.self, from: data)
            expect(decoded == config, "config: JSON round trip")
        } catch {
            expect(false, "config: encode/decode threw \(error)")
        }

        expect(CleanupEngine.parseVocabulary("Luna, VoiceVector\n OrbStack ,,\n")
               == ["Luna", "VoiceVector", "OrbStack"], "config: vocabulary parsing")
        expect(ProviderKind.fireworks.supportsTranscription == false, "config: fireworks is chat-only")
        expect(ProviderKind.vercelGateway.supportsTranscription
               && ProviderKind.vercelGateway.supportsChat, "config: vercel gateway does STT + chat")
        expect(ProviderProfile.preset(.vercelGateway).baseURL == "https://ai-gateway.vercel.sh/v1",
               "config: vercel gateway base URL")

        // Custom cleanup prompt handling
        var cleanup = CleanupConfig()
        expect(CleanupEngine.systemPrompt(config: cleanup)
               == CleanupEngine.defaultPrompt(mode: .rich), "cleanup: default prompt used when no custom")
        cleanup.customPrompt = "My own prompt."
        cleanup.vocabulary = "Luna"
        let custom = CleanupEngine.systemPrompt(config: cleanup)
        expect(custom.hasPrefix("My own prompt."), "cleanup: custom prompt replaces built-in")
        expect(custom.contains("Luna"), "cleanup: vocabulary appended to custom prompt")

        // Review (staged revisions): prompt parity with shared/prompts, message shape, sanitizing
        let cwd = FileManager.default.currentDirectoryPath
        let candidates = [cwd, cwd + "/..", cwd + "/../..", cwd + "/../../.."]
            .map { $0 + "/shared/prompts" }
        if let dir = candidates.first(where: { FileManager.default.fileExists(atPath: $0 + "/review.txt") }) {
            let file = (try? String(contentsOfFile: dir + "/review.txt", encoding: .utf8)) ?? ""
            expect(file.trimmingCharacters(in: .whitespacesAndNewlines)
                   == CleanupEngine.reviewPrompt.trimmingCharacters(in: .whitespacesAndNewlines),
                   "review: prompt matches shared/prompts/review.txt")
            let router = (try? String(contentsOfFile: dir + "/router.txt", encoding: .utf8)) ?? ""
            expect(router.trimmingCharacters(in: .whitespacesAndNewlines)
                   == CleanupEngine.routerPrompt.trimmingCharacters(in: .whitespacesAndNewlines),
                   "router: prompt matches shared/prompts/router.txt")
            let rich = (try? String(contentsOfFile: dir + "/cleanup-rich.txt", encoding: .utf8)) ?? ""
            expect(rich.trimmingCharacters(in: .whitespacesAndNewlines)
                   == CleanupEngine.defaultPrompt(mode: .rich).trimmingCharacters(in: .whitespacesAndNewlines),
                   "cleanup: rich prompt matches shared/prompts/cleanup-rich.txt")
        }
        expect(CleanupEngine.reviewMessage(draft: "Hi", instruction: "shorter")
               == "<draft>\nHi\n</draft>\n<instruction>\nshorter\n</instruction>",
               "review: message wraps draft and instruction")
        let captions = ScreenshotSet(images: [Data([1]), Data([2])], activeIndex: 0, outlined: true).attachments.map(\.caption)
        expect(captions == ["Display 1 of 2 — ACTIVE: the dictated text will be inserted here; the target window is outlined in red.",
                            "Display 2 of 2."],
               "screenshot: per-display captions")
        expect(ScreenshotSet(images: [Data([1])], activeIndex: nil, outlined: false).attachments.first?.caption
               == "Display 1 of 1 (which display is active is not known on this desktop).",
               "screenshot: unknown-active caption")

        // Every key code must produce a name without trapping (F-key virtual
        // codes are unordered; a range over them crashed the settings UI).
        let names = (UInt16(0)...UInt16(127)).map { HotkeyEngine.keyName($0) }
        expect(names.count == 128 && names.contains("F20") && !names.contains(""),
               "hotkey: keyName total over all key codes")
        // Multi-machine: pairing code vector (same in all three apps), frames,
        // identity certificate, router verdict parsing.
        let fpC = Data(SHA256.hash(data: Data("client-cert".utf8)))
        let fpS = Data(SHA256.hash(data: Data("server-cert".utf8)))
        expect(PeerCrypto.pairingCode(fpClient: fpC, fpServer: fpS,
                                      nonceClient: Data(repeating: 1, count: 32),
                                      nonceServer: Data(repeating: 2, count: 32)) == "636241",
               "peer: pairing code test vector")
        if let framed = PeerCrypto.frame(["t": "hello", "name": "mac"]),
           case .frame(let object, let consumed) = PeerCrypto.parseFrame(framed + Data([9, 9])) {
            expect(object["t"] as? String == "hello" && consumed == framed.count,
                   "peer: frame round trip leaves trailing bytes")
        } else {
            expect(false, "peer: frame round trip")
        }
        if case .incomplete = PeerCrypto.parseFrame(Data([0, 0])) { expect(true, "peer: incomplete frame") }
        else { expect(false, "peer: incomplete frame") }
        // An oversized length must be rejected (invalid), not treated as "need more".
        if case .invalid = PeerCrypto.parseFrame(Data([0xFF, 0xFF, 0xFF, 0xFF])) { expect(true, "peer: oversized frame rejected") }
        else { expect(false, "peer: oversized frame rejected") }
        expect(Data(hexString: Data([0xAB, 0x01]).hexString) == Data([0xAB, 0x01]), "peer: hex round trip")
        let keyAttributes: [String: Any] = [kSecAttrKeyType as String: kSecAttrKeyTypeECSECPrimeRandom,
                                            kSecAttrKeySizeInBits as String: 256]
        if let key = SecKeyCreateRandomKey(keyAttributes as CFDictionary, nil),
           let der = PeerCrypto.makeSelfSignedCertificate(key: key, commonName: "VoiceVector-test"),
           let cert = SecCertificateCreateWithData(nil, der as CFData) {
            let summary = SecCertificateCopySubjectSummary(cert) as String? ?? ""
            expect(summary == "VoiceVector-test", "peer: self-signed certificate parses with CN")
            expect(PeerCrypto.fingerprint(of: der).count == 32, "peer: certificate fingerprint")
        } else {
            expect(false, "peer: self-signed certificate creation")
        }
        expect(CleanupEngine.parseRouterVerdict("Sure: {\"machine\": \"mac\", \"window\": 42}")
               == CleanupEngine.RouterVerdict(machine: "mac", window: 42),
               "router: verdict parsed out of prose")
        expect(CleanupEngine.parseRouterVerdict("{\"machine\":\"m\"}")
               == CleanupEngine.RouterVerdict(machine: "m", window: 0), "router: missing window is 0")
        expect(CleanupEngine.parseRouterVerdict("no json here") == nil, "router: garbage is nil")
        let routerMsg = CleanupEngine.routerMessage(draft: "hi", spoken: "Hey Slack, hi",
                                                   machines: [("mac", true, "1: A — B")])
        expect(routerMsg.contains("<text>\nhi\n</text>")
               && routerMsg.contains("<spoken-request>\nHey Slack, hi\n</spoken-request>")
               && routerMsg.contains("(current: the user dictated here)"),
               "router: message carries spoken request and text")
        let catalog = [CleanupEngine.RouterCatalog(machine: "mac", windowIDs: [1, 2])]
        expect(CleanupEngine.routerVerdictValid(.init(machine: "mac", window: 1), catalog: catalog),
               "router: listed window is valid")
        expect(CleanupEngine.routerVerdictValid(.init(machine: "mac", window: 0), catalog: catalog),
               "router: window 0 (focus) is valid")
        expect(!CleanupEngine.routerVerdictValid(.init(machine: "mac", window: 99), catalog: catalog),
               "router: unlisted window id is rejected")
        expect(!CleanupEngine.routerVerdictValid(.init(machine: "ghost", window: 1), catalog: catalog),
               "router: unknown machine is rejected")
        if let mmData = "{\"peers\":[{\"name\":\"x\",\"fingerprint\":\"ab\"}]}".data(using: .utf8),
           let mm = try? JSONDecoder().decode(MultiMachineConfig.self, from: mmData) {
            expect(mm.enabled == false && mm.port == 47800 && mm.peers.first?.allowDeliver == false
                   && mm.peers.first?.name == "x", "peer: tolerant config decoding")
        } else {
            expect(false, "peer: tolerant config decoding")
        }

        if let asData = "{\"autoSubmit\":true,\"routerEnabled\":true}".data(using: .utf8),
           let ap = try? JSONDecoder().decode(DictationProfile.self, from: asData) {
            expect(ap.autoSubmit && ap.routerEnabled, "profile: autoSubmit decodes")
            if let round = try? JSONEncoder().encode(ap),
               let ap2 = try? JSONDecoder().decode(DictationProfile.self, from: round) {
                expect(ap2.autoSubmit, "profile: autoSubmit round trips")
            } else { expect(false, "profile: autoSubmit round trips") }
        } else { expect(false, "profile: autoSubmit decodes") }

        expect(!HotkeySpec.unset.isSet && HotkeySpec.default.isSet
               && HotkeySpec(keyCode: 0, modifiers: CGEventFlags.maskCommand.rawValue, isModifierOnly: false).isSet,
               "hotkey: unset placeholder is never a binding (key code 0 is the letter A)")
        expect(CleanupEngine.sanitize("<draft>\nHello\n</draft>", fallback: "x") == "Hello",
               "review: echoed draft delimiters stripped")
        var reviewProfile = DictationProfile(name: "Email", hotkey: .default, cleanupMode: .rich)
        reviewProfile.reviewBeforePaste = true
        reviewProfile.screenshotContext = true
        reviewProfile.reviewProviderID = UUID()
        let reviewRound = try? JSONDecoder().decode(DictationProfile.self,
                                                    from: JSONEncoder().encode(reviewProfile))
        expect(reviewRound?.reviewBeforePaste == true && reviewRound?.screenshotContext == true
               && reviewRound?.reviewProviderID == reviewProfile.reviewProviderID,
               "review: profile options round trip")
        let plainProfile = try? JSONDecoder().decode(DictationProfile.self, from: Data("{}".utf8))
        expect(plainProfile?.reviewBeforePaste == false && plainProfile?.screenshotContext == false,
               "review: options default off")

        // Dictation profiles: legacy migration + policy resolution
        do {
            let legacy = Data(#"{"hotkey":{"keyCode":63,"modifiers":0,"isModifierOnly":true}}"#.utf8)
            let migrated = try JSONDecoder().decode(AppConfig.self, from: legacy)
            expect(migrated.dictationProfiles.count == 1
                   && migrated.dictationProfiles[0].hotkey.keyCode == 63,
                   "profiles: legacy hotkey migrates into default profile")
        } catch {
            expect(false, "profiles: legacy decode threw \(error)")
        }
        var profCfg = AppConfig()
        let chatProv = ProviderProfile.preset(.cerebras)
        let defaultProv = ProviderProfile.preset(.fireworks)
        profCfg.providers = [chatProv, defaultProv]
        profCfg.cleanup.providerID = defaultProv.id
        var rawProfile = DictationProfile(name: "Raw", hotkey: .default, cleanupMode: .off)
        var altProfile = DictationProfile(name: "Email", hotkey: .default)
        altProfile.cleanupProviderID = chatProv.id
        altProfile.customPrompt = "Email tone."
        profCfg.dictationProfiles = [DictationProfile(), rawProfile, altProfile]
        let rawPolicy = CleanupEngine.effective(profile: rawProfile, config: profCfg)
        expect(!rawPolicy.enabled, "profiles: cleanup-off profile disables cleanup")
        let altPolicy = CleanupEngine.effective(profile: altProfile, config: profCfg)
        expect(altPolicy.enabled && altPolicy.provider?.id == chatProv.id,
               "profiles: provider override resolves")
        expect(altPolicy.config.customPrompt == "Email tone.", "profiles: prompt override resolves")
        let defPolicy = CleanupEngine.effective(profile: profCfg.dictationProfiles[0], config: profCfg)
        expect(defPolicy.provider?.id == defaultProv.id
               && defPolicy.config.customPrompt.isEmpty,
               "profiles: default profile inherits globals")
        let roundTrip = try? JSONDecoder().decode(AppConfig.self,
                                                  from: JSONEncoder().encode(profCfg))
        expect(roundTrip?.dictationProfiles.count == 3
               && roundTrip?.dictationProfiles[2].customPrompt == "Email tone.",
               "profiles: JSON round trip")
        expect(roundTrip?.dictationProfiles[1].cleanupMode == .off
               && roundTrip?.dictationProfiles[1].cleanupEnabled == false,
               "profiles: off mode round-trips and mirrors legacy cleanupEnabled")
        profCfg.cleanup.mode = .off
        var lightProfile = DictationProfile(name: "Light", hotkey: .default, cleanupMode: .light)
        expect(CleanupEngine.effective(profile: lightProfile, config: profCfg).config.mode == .light,
               "profiles: explicit mode wins over global off")
        lightProfile.cleanupMode = nil
        expect(!CleanupEngine.effective(profile: lightProfile, config: profCfg).enabled,
               "profiles: legacy profile inherits global mode")
        profCfg.cleanup.mode = .rich
        lightProfile.cleanupEnabled = false
        expect(!CleanupEngine.effective(profile: lightProfile, config: profCfg).enabled,
               "profiles: legacy cleanupEnabled=false forces raw")
        expect(ProviderKind.elevenLabs.supportsVocabulary && ProviderKind.openAICompatible.supportsVocabulary
               && !ProviderKind.vercelGateway.supportsVocabulary,
               "providers: vocabulary support matches the STT calls that send it")
        let sttA = ProviderProfile.preset(.elevenLabs)
        let sttB = ProviderProfile.preset(.vercelGateway)
        profCfg.providers += [sttA, sttB]
        profCfg.sttProviderID = sttA.id
        profCfg.cleanup.vocabulary = "Luna"
        var codeProfile = DictationProfile(name: "Code", hotkey: .default, cleanupMode: .light)
        codeProfile.sttProviderID = sttB.id
        codeProfile.vocabulary = "OrbStack, SwiftPM"
        let codePolicy = CleanupEngine.effective(profile: codeProfile, config: profCfg)
        expect(codePolicy.stt?.id == sttB.id, "profiles: transcriber override resolves")
        expect(codePolicy.config.vocabulary == "Luna, OrbStack, SwiftPM",
               "profiles: vocabulary merges global + hotkey")
        expect(CleanupEngine.effective(profile: DictationProfile(), config: profCfg).stt?.id == sttA.id,
               "profiles: default transcriber inherited")
        expect(CleanupEngine.mergeVocabulary("", " a, b ") == "a, b"
               && CleanupEngine.mergeVocabulary("x", "") == "x",
               "profiles: vocabulary merge edge cases")
        let codeRound = try? JSONDecoder().decode(DictationProfile.self,
                                                  from: JSONEncoder().encode(codeProfile))
        expect(codeRound?.sttProviderID == sttB.id && codeRound?.vocabulary == "OrbStack, SwiftPM",
               "profiles: transcriber + vocabulary round trip")

        // Single-pass eligibility (same provider + same model fuses stages)
        var spProfile = ProviderProfile.preset(.vercelGateway)
        spProfile.sttModel = "google/gemini-2.5-flash"
        spProfile.chatModel = "google/gemini-2.5-flash"
        expect(CleanupEngine.singlePassEligible(stt: spProfile, cleanupProfile: spProfile, mode: .rich),
               "single-pass: same provider+model")
        expect(!CleanupEngine.singlePassEligible(stt: spProfile, cleanupProfile: spProfile, mode: .off),
               "single-pass: off mode blocks")
        let spOther = ProviderProfile.preset(.vercelGateway)
        expect(!CleanupEngine.singlePassEligible(stt: spProfile, cleanupProfile: spOther, mode: .rich),
               "single-pass: different provider blocks")
        spProfile.sttModel = "openai/whisper-1"
        expect(!CleanupEngine.singlePassEligible(stt: spProfile, cleanupProfile: spProfile, mode: .rich),
               "single-pass: different models block")

        // Update version comparison
        expect(UpdateService.isNewer("0.2.0", than: "0.1.0"), "update: newer minor")
        expect(UpdateService.isNewer("1.0.0", than: "0.9.9"), "update: newer major")
        expect(!UpdateService.isNewer("0.1.0", than: "0.1.0"), "update: equal is not newer")
        expect(!UpdateService.isNewer("0.1.0", than: "0.1.1"), "update: older is not newer")
        expect(UpdateService.isNewer("0.1.0", than: "0.0.0-dev"), "update: dev builds always eligible")

        // Legacy config.json (no customPrompt key) must still decode
        do {
            let legacy = Data(#"{"mode":"light","vocabulary":"x"}"#.utf8)
            let decoded = try JSONDecoder().decode(CleanupConfig.self, from: legacy)
            expect(decoded.mode == .light && decoded.customPrompt.isEmpty,
                   "cleanup: legacy config decodes with defaults")
        } catch {
            expect(false, "cleanup: legacy config decode threw \(error)")
        }
    }

    // MARK: WAV writer

    private static func testWavWriter() {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("vv-wav-\(UUID().uuidString).wav")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            let writer = try WavWriter(url: url, sampleRate: 16_000)
            let samples: [Int16] = Array(repeating: 0, count: 16_000) // 1 second
            try samples.withUnsafeBufferPointer { buffer in
                try writer.append(samples: buffer.baseAddress!, count: buffer.count)
            }
            let duration = writer.finalize()
            expect(abs(duration - 1.0) < 0.001, "wav: duration")

            let data = try Data(contentsOf: url)
            expect(data.count == 44 + 32_000, "wav: file size")
            expect(String(decoding: data.prefix(4), as: UTF8.self) == "RIFF", "wav: RIFF magic")
            expect(String(decoding: data[8..<12], as: UTF8.self) == "WAVE", "wav: WAVE magic")
            let size = data[40..<44].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
            expect(size == 32_000, "wav: data chunk size")

            // Slice extraction for streamed segments (second half of a ramp).
            let sliceURL = FileManager.default.temporaryDirectory
                .appendingPathComponent("vv-slice-\(UUID().uuidString).wav")
            let sliceWriter = try WavWriter(url: sliceURL, sampleRate: 16_000)
            var ramp = [Int16](repeating: 0, count: 16_000)
            for i in 0..<ramp.count { ramp[i] = Int16(i) }
            try ramp.withUnsafeBufferPointer { try sliceWriter.append(samples: $0.baseAddress!, count: $0.count) }
            expect(sliceWriter.dataBytes == 32_000, "wav: dataBytes tracks writes")
            let slice = try WavWriter.sliceWav(fileURL: sliceURL, fromByte: 16_000, toByte: 32_000)
            expect(slice.count == 44 + 16_000, "wav: slice size")
            let sliceLen = slice[40..<44].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
            expect(sliceLen == 16_000, "wav: slice header size")
            let firstSample = slice[44..<46].withUnsafeBytes { $0.loadUnaligned(as: Int16.self) }
            expect(firstSample == 8000, "wav: slice starts mid-stream")
            sliceWriter.finalize()
            try? FileManager.default.removeItem(at: sliceURL)

            let silent = WavWriter.silentWav(seconds: 0.3)
            expect(silent.count == 44 + 9600, "wav: silent sample size")
            expect(String(decoding: silent.prefix(4), as: UTF8.self) == "RIFF", "wav: silent sample magic")
        } catch {
            expect(false, "wav: threw \(error)")
        }
    }
}
