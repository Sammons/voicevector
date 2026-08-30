import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var app: AppState
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Settings")
                    .font(.system(.title3, design: .rounded).weight(.bold))
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            .padding(16)
            Divider()
            TabView {
                ProvidersSettings()
                    .tabItem { Label("Providers", systemImage: "server.rack") }
                DictationSettings()
                    .tabItem { Label("Dictation", systemImage: "mic") }
                FoldersSettings()
                    .tabItem { Label("Folders & Webhooks", systemImage: "folder") }
                GeneralSettings()
                    .tabItem { Label("General", systemImage: "gearshape") }
                MultiMachineSettings()
                    .tabItem { Label("Multi-machine", systemImage: "laptopcomputer.and.iphone") }
                AboutSettings()
                    .tabItem { Label("About", systemImage: "info.circle") }
            }
            .padding(12)
        }
        .frame(width: 620, height: 560)
    }
}

// MARK: - Providers

struct ProvidersSettings: View {
    @EnvironmentObject var app: AppState
    @State private var selection: UUID?

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 8) {
                List(selection: $selection) {
                    ForEach(app.config.providers) { profile in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(profile.name).font(.callout.weight(.medium))
                            Text(profile.kind.displayName)
                                .font(.caption2).foregroundStyle(.secondary)
                        }
                        .tag(profile.id)
                    }
                }
                .listStyle(.bordered)
                Menu {
                    ForEach(ProviderKind.allCases) { kind in
                        Button(kind.displayName) {
                            selection = app.addProvider(kind).id
                        }
                    }
                } label: {
                    Label("Add Provider", systemImage: "plus")
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
            }
            .frame(width: 170)
            .padding(.trailing, 12)

            Divider()

            Group {
                if let id = selection,
                   let index = app.config.providers.firstIndex(where: { $0.id == id }) {
                    ProviderEditor(profile: $app.config.providers[index]) {
                        let profile = app.config.providers[index]
                        selection = nil
                        app.removeProvider(profile)
                    }
                } else {
                    VStack(spacing: 8) {
                        Image(systemName: "server.rack")
                            .font(.system(size: 28))
                            .foregroundStyle(.tertiary)
                        Text("Add or select a provider")
                            .foregroundStyle(.secondary)
                        Text("ElevenLabs for transcription, Fireworks for cleanup,\nor any OpenAI-compatible server for either.")
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .padding(.leading, 12)
        }
        .padding(8)
        .onAppear { selection = app.config.providers.first?.id }
    }
}

struct ProviderEditor: View {
    @EnvironmentObject var app: AppState
    @Binding var profile: ProviderProfile
    var onDelete: () -> Void

    @State private var apiKey = ""
    @State private var models: [String] = []
    @State private var testResult: String?
    @State private var testFailed = false
    @State private var busy = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Grid(alignment: .leading, verticalSpacing: 10) {
                    GridRow {
                        Text("Name").gridColumnAlignment(.trailing)
                        TextField("", text: $profile.name)
                    }
                    GridRow {
                        Text("Base URL")
                        TextField("", text: $profile.baseURL)
                            .font(.system(.body, design: .monospaced))
                            .disabled(profile.kind != .openAICompatible)
                            .foregroundStyle(profile.kind == .openAICompatible ? .primary : .secondary)
                    }
                    GridRow {
                        Text("API key")
                        SecureField(profile.kind == .openAICompatible ? "optional for local servers" : "",
                                    text: $apiKey)
                            .onChange(of: apiKey) { _, newValue in
                                Keychain.setAPIKey(newValue, for: profile.id)
                            }
                    }
                    if profile.kind.supportsTranscription {
                        GridRow {
                            Text("STT model")
                            modelField(text: $profile.sttModel)
                        }
                    }
                    if profile.kind.supportsChat {
                        GridRow {
                            Text("Chat model")
                            modelField(text: $profile.chatModel)
                        }
                    }
                }
                .textFieldStyle(.roundedBorder)

                HStack(spacing: 10) {
                    Button {
                        runTest()
                    } label: {
                        if busy { ProgressView().controlSize(.small) } else { Text("Test Connection") }
                    }
                    .disabled(busy)
                    if profile.kind.supportsModelListing {
                        Button("List Models") { loadModels() }
                            .disabled(busy)
                    }
                    Spacer()
                    Button(role: .destructive, action: onDelete) {
                        Image(systemName: "trash")
                    }
                    .help("Remove this provider")
                }

                if let testResult {
                    Label(testResult, systemImage: testFailed ? "xmark.circle.fill" : "checkmark.circle.fill")
                        .font(.callout)
                        .foregroundStyle(testFailed ? Theme.danger : .green)
                        .textSelection(.enabled)
                }

                if !models.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Available models").vvSectionTitle()
                        ScrollView {
                            VStack(alignment: .leading, spacing: 2) {
                                ForEach(models, id: \.self) { model in
                                    HStack {
                                        Text(model).font(.system(.caption, design: .monospaced))
                                        Spacer()
                                        if profile.kind.supportsTranscription {
                                            Button("STT") { profile.sttModel = model }
                                                .font(.caption2)
                                        }
                                        if profile.kind.supportsChat {
                                            Button("Chat") { profile.chatModel = model }
                                                .font(.caption2)
                                        }
                                    }
                                    .buttonStyle(.borderless)
                                }
                            }
                        }
                        .frame(maxHeight: 140)
                    }
                    .vvCard()
                }

                roleAssignments
            }
            .padding(4)
        }
        .onAppear { apiKey = Keychain.apiKey(for: profile.id) }
        .id(profile.id)
    }

    private func modelField(text: Binding<String>) -> some View {
        TextField("", text: text)
            .font(.system(.body, design: .monospaced))
    }

    /// Which roles this profile currently plays.
    private var roleAssignments: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Use this provider for").vvSectionTitle()
            HStack(spacing: 14) {
                if profile.kind.supportsTranscription {
                    Toggle("Transcription", isOn: Binding(
                        get: { app.config.sttProviderID == profile.id },
                        set: { app.config.sttProviderID = $0 ? profile.id : nil }
                    ))
                }
                if profile.kind.supportsChat {
                    Toggle("Cleanup", isOn: Binding(
                        get: { app.config.cleanup.providerID == profile.id },
                        set: { app.config.cleanup.providerID = $0 ? profile.id : nil }
                    ))
                }
            }
            .toggleStyle(.checkbox)
        }
    }

    private func runTest() {
        busy = true
        testResult = nil
        let client = ProviderClient(profile: profile, apiKey: apiKey)
        Task { @MainActor in
            do {
                testResult = try await client.test()
                testFailed = false
            } catch {
                testResult = error.localizedDescription
                testFailed = true
            }
            busy = false
        }
    }

    private func loadModels() {
        busy = true
        let client = ProviderClient(profile: profile, apiKey: apiKey)
        Task { @MainActor in
            do {
                models = try await client.listModels()
                if models.isEmpty { testResult = "No models reported."; testFailed = false }
            } catch {
                testResult = error.localizedDescription
                testFailed = true
            }
            busy = false
        }
    }
}

// MARK: - Dictation (hotkey + cleanup)

struct DictationSettings: View {
    @EnvironmentObject var app: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Transcription").vvSectionTitle()
                    Picker("Provider", selection: $app.config.sttProviderID) {
                        Text("None").tag(UUID?.none)
                        ForEach(app.config.providers.filter { $0.kind.supportsTranscription && !$0.sttModel.isEmpty }) { profile in
                            Text("\(profile.name) — \(profile.sttModel)").tag(Optional(profile.id))
                        }
                    }
                    if sttProviderMissing {
                        Label("No transcription provider selected — dictation can't run.",
                              systemImage: "exclamationmark.triangle.fill")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
                .vvCard()

                VStack(alignment: .leading, spacing: 10) {
                    Text("Hotkeys").vvSectionTitle()
                    Text("Each hotkey has its own cleanup: raw for LLM chats, polished for email — with its own model and prompt if you like.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    ForEach(Array(app.config.dictationProfiles.enumerated()), id: \.element.id) { index, _ in
                        if index > 0 { Divider() }
                        ProfileRow(index: index)
                    }
                    Button {
                        var profile = DictationProfile()
                        profile.name = "Hotkey \(app.config.dictationProfiles.count + 1)"
                        profile.hotkey = HotkeySpec.unset
                        profile.cleanupMode = CleanupEngine.effectiveMode(profile: nil, config: app.config)
                        app.config.dictationProfiles.append(profile)
                    } label: {
                        Label("Add Hotkey", systemImage: "plus")
                    }
                    .buttonStyle(.borderless)
                    Divider().padding(.vertical, 2)
                    Picker("Tap behavior", selection: $app.config.tapStartMode) {
                        ForEach(TapStartMode.allCases, id: \.self) { mode in
                            Text(mode.label).tag(mode)
                        }
                    }
                    .pickerStyle(.radioGroup)
                    Text("Hold-to-talk always works: press and hold, speak, release.\nEsc cancels a recording in progress.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Toggle("Transcribe during pauses (long dictations finish faster)",
                           isOn: $app.config.chunkedTranscription)
                        .toggleStyle(.checkbox)
                }
                .vvCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Shared vocabulary").vvSectionTitle()
                    Text("Names and jargon every hotkey should get right — comma separated. Sent to the transcriber when it supports it (ElevenLabs key terms, Whisper-style prompt) and always added to the cleanup prompt.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    TextEditor(text: $app.config.cleanup.vocabulary)
                        .font(.system(.callout, design: .monospaced))
                        .frame(height: 48)
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.quaternary))
                    if let unsupported = transcribersWithoutVocabulary {
                        Label("\(unsupported) doesn't accept vocabulary hints — these terms will only guide cleanup for hotkeys using it.",
                              systemImage: "info.circle")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .vvCard()
            }
            .padding(8)
        }
    }

    /// Names of transcribers in use (global default + per-hotkey overrides)
    /// whose STT call has no vocabulary field.
    private var transcribersWithoutVocabulary: String? {
        var ids: [UUID] = []
        if let id = app.config.sttProviderID { ids.append(id) }
        ids += app.config.dictationProfiles.compactMap(\.sttProviderID)
        var names: [String] = []
        for id in ids {
            guard let p = app.config.providers.first(where: { $0.id == id }),
                  !p.kind.supportsVocabulary, !names.contains(p.name) else { continue }
            names.append(p.name)
        }
        return names.isEmpty ? nil : names.joined(separator: ", ")
    }

    private var sttProviderMissing: Bool {
        guard let id = app.config.sttProviderID,
              let profile = app.config.providers.first(where: { $0.id == id }),
              profile.kind.supportsTranscription, !profile.sttModel.isEmpty else { return true }
        return false
    }
}

/// One dictation profile: name, hotkey, and the cleanup it runs (mode,
/// provider, prompt). Globals are only inherited defaults here.
struct ProfileRow: View {
    @EnvironmentObject var app: AppState
    let index: Int
    @State private var capturing = false
    @State private var showPrompt = false
    @State private var showVocabulary = false

    private var chatProviders: [ProviderProfile] {
        app.config.providers.filter { $0.kind.supportsChat && !$0.chatModel.isEmpty }
    }

    private var sttProviders: [ProviderProfile] {
        app.config.providers.filter { $0.kind.supportsTranscription && !$0.sttModel.isEmpty }
    }

    var body: some View {
        if index < app.config.dictationProfiles.count {
            let profile = app.config.dictationProfiles[index]
            let policy = CleanupEngine.effective(profile: profile, config: app.config)
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    TextField("Name", text: binding(\.name))
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 120)
                    Button {
                        toggleCapture()
                    } label: {
                        Text(capturing ? "Press a key…" :
                             !profile.hotkey.isSet
                                ? "Set hotkey…" : HotkeyEngine.describe(profile.hotkey))
                            .font(.system(.callout, design: .rounded).weight(.semibold))
                            .frame(minWidth: 90)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 5)
                            .background(capturing ? Theme.accent.opacity(0.25) : Theme.accentSoft,
                                        in: RoundedRectangle(cornerRadius: 8))
                            .foregroundStyle(Theme.accent)
                    }
                    .buttonStyle(.plain)
                    Picker("", selection: modeBinding) {
                        Text("Raw transcript").tag(CleanupMode.off)
                        Text("Light cleanup").tag(CleanupMode.light)
                        Text("Rich cleanup").tag(CleanupMode.rich)
                    }
                    .labelsHidden()
                    .frame(width: 150)
                    Spacer()
                    if app.config.dictationProfiles.count > 1 {
                        Button(role: .destructive) {
                            app.config.dictationProfiles.remove(at: index)
                        } label: {
                            Image(systemName: "trash")
                        }
                        .buttonStyle(.borderless)
                    }
                }
                HStack(spacing: 8) {
                    Picker("Transcriber", selection: binding(\.sttProviderID)) {
                        Text(defaultSttLabel).tag(UUID?.none)
                        ForEach(sttProviders) { p in
                            Text("\(p.name) — \(p.sttModel)").tag(Optional(p.id))
                        }
                    }
                    .frame(maxWidth: 330)
                    Button(showVocabulary ? "Hide vocabulary"
                           : (profile.vocabulary.isEmpty ? "Vocabulary…" : "Vocabulary (custom)…")) {
                        showVocabulary.toggle()
                    }
                    .buttonStyle(.borderless)
                    .font(.caption)
                    Spacer()
                }
                HStack(spacing: 8) {
                    Toggle("Review before pasting", isOn: binding(\.reviewBeforePaste))
                        .toggleStyle(.checkbox)
                    Toggle("Screenshot context", isOn: binding(\.screenshotContext))
                        .toggleStyle(.checkbox)
                    Spacer()
                }
                if profile.reviewBeforePaste {
                    HStack(spacing: 8) {
                        Picker("Review model", selection: binding(\.reviewProviderID)) {
                            Text("Same as cleanup").tag(UUID?.none)
                            ForEach(chatProviders) { p in
                                Text("\(p.name) — \(p.chatModel)").tag(Optional(p.id))
                            }
                        }
                        .frame(maxWidth: 330)
                        Toggle("Route with AI", isOn: binding(\.routerEnabled))
                            .toggleStyle(.checkbox)
                        Spacer()
                    }
                    if profile.routerEnabled {
                        HStack(spacing: 8) {
                            Picker("Router model", selection: binding(\.routerProviderID)) {
                                Text("Same as review").tag(UUID?.none)
                                ForEach(chatProviders) { p in
                                    Text("\(p.name) — \(p.chatModel)").tag(Optional(p.id))
                                }
                            }
                            .frame(maxWidth: 330)
                            Spacer()
                        }
                        Text("A router model looks at your windows (and paired machines' windows) and picks where the draft should go; the staging card shows its choice and ⏎ sends it there. Set up machines in Settings → Multi-machine.")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Text("The cleaned text is staged above the recording pill instead of pasted. Press the hotkey and say a change (\"make it shorter\", \"turn that into a list\") as many times as you like; ⏎ pastes, Esc discards.")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if profile.screenshotContext {
                    HStack(spacing: 8) {
                        Text("A screenshot of every display is attached to cleanup and review calls so the model knows what you're looking at; the window receiving the text is outlined. Needs Screen Recording; models without vision ignore it.")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .fixedSize(horizontal: false, vertical: true)
                        if !app.screenRecordingGranted {
                            Button("Grant…") { ScreenCapture.requestPermission() }
                                .controlSize(.small)
                        }
                    }
                }
                if showVocabulary {
                    TextEditor(text: binding(\.vocabulary))
                        .font(.system(.callout, design: .monospaced))
                        .frame(height: 40)
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.quaternary))
                    Text("Extra terms for this hotkey, added to the shared vocabulary (comma separated; the transcriber uses them when it supports vocabulary hints).")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                if policy.enabled {
                    HStack(spacing: 8) {
                        Picker("Cleanup model", selection: binding(\.cleanupProviderID)) {
                            Text(defaultProviderLabel).tag(UUID?.none)
                            ForEach(chatProviders) { p in
                                Text("\(p.name) — \(p.chatModel)").tag(Optional(p.id))
                            }
                        }
                        .frame(maxWidth: 330)
                        Button(showPrompt ? "Hide prompt" : (profile.customPrompt.isEmpty ? "Prompt…" : "Prompt (custom)…")) {
                            showPrompt.toggle()
                        }
                        .buttonStyle(.borderless)
                        .font(.caption)
                        Spacer()
                    }
                    if policy.provider == nil || !(policy.provider?.kind.supportsChat ?? false)
                        || (policy.provider?.chatModel.isEmpty ?? true) {
                        Label("No cleanup model available — this hotkey will paste the raw transcript.",
                              systemImage: "exclamationmark.triangle.fill")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                    if showPrompt {
                        TextEditor(text: promptBinding(policy: policy))
                            .font(.system(.caption, design: .monospaced))
                            .frame(height: 110)
                            .overlay(RoundedRectangle(cornerRadius: 6).stroke(.quaternary))
                        HStack {
                            Text(profile.customPrompt.isEmpty
                                 ? "Built-in prompt for this mode — any edit saves a custom prompt for this hotkey. Vocabulary is appended automatically."
                                 : "Custom prompt for this hotkey. Vocabulary is appended automatically.")
                                .font(.caption2)
                                .foregroundStyle(.tertiary)
                                .fixedSize(horizontal: false, vertical: true)
                            Spacer()
                            if !profile.customPrompt.isEmpty {
                                Button("Reset to Built-in") {
                                    app.config.dictationProfiles[index].customPrompt = ""
                                }
                                .controlSize(.small)
                            }
                        }
                    }
                }
            }
            .onDisappear { stopCapture() }
        }
    }

    private var defaultSttLabel: String {
        if let id = app.config.sttProviderID,
           let p = app.config.providers.first(where: { $0.id == id }) {
            return "Default (\(p.name) — \(p.sttModel))"
        }
        return "Default (none)"
    }

    private var defaultProviderLabel: String {
        if let id = app.config.cleanup.providerID,
           let p = app.config.providers.first(where: { $0.id == id }) {
            return "Default (\(p.name) — \(p.chatModel))"
        }
        return "Default (none)"
    }

    /// Reads the effective mode; writing pins an explicit mode on the profile.
    private var modeBinding: Binding<CleanupMode> {
        Binding(
            get: { CleanupEngine.effectiveMode(profile: app.config.dictationProfiles[index],
                                               config: app.config) },
            set: { mode in
                app.config.dictationProfiles[index].cleanupMode = mode
                app.config.dictationProfiles[index].cleanupEnabled = mode != .off
            }
        )
    }

    /// Shows the profile's custom prompt, or the effective (global custom or
    /// built-in) prompt until the user edits it.
    private func promptBinding(policy: CleanupEngine.EffectiveCleanup) -> Binding<String> {
        Binding(
            get: {
                let custom = app.config.dictationProfiles[index].customPrompt
                if !custom.isEmpty { return custom }
                let global = app.config.cleanup.customPrompt
                return global.isEmpty ? CleanupEngine.defaultPrompt(mode: policy.config.mode) : global
            },
            set: { app.config.dictationProfiles[index].customPrompt = $0 }
        )
    }

    private func binding<T>(_ keyPath: WritableKeyPath<DictationProfile, T>) -> Binding<T> {
        Binding(
            get: { app.config.dictationProfiles[index][keyPath: keyPath] },
            set: { app.config.dictationProfiles[index][keyPath: keyPath] = $0 }
        )
    }

    private func toggleCapture() {
        capturing ? stopCapture() : startCapture()
    }

    private func startCapture() {
        guard app.accessibilityGranted else {
            app.requestAccessibility()
            return
        }
        capturing = true
        let captureIndex = index
        app.hotkey.onCaptureKey = { spec in
            if captureIndex < app.config.dictationProfiles.count {
                app.config.dictationProfiles[captureIndex].hotkey = spec
            }
            stopCapture()
        }
        app.hotkey.captureMode = true
    }

    private func stopCapture() {
        capturing = false
        app.hotkey.captureMode = false
        app.hotkey.onCaptureKey = nil
    }
}

/// Shows the exact system prompt used for cleanup; editing it saves a custom
/// prompt, Reset returns to the built-in one for the selected mode.
// MARK: - Folders & webhooks

struct FoldersSettings: View {
    @EnvironmentObject var app: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Each folder can forward finished dictations to a webhook.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                ForEach(app.dictation.currentLibrary.folderNames(), id: \.self) { folder in
                    FolderWebhookRow(folder: folder)
                }
            }
            .padding(8)
        }
    }
}

struct FolderWebhookRow: View {
    @EnvironmentObject var app: AppState
    let folder: String

    private var binding: Binding<WebhookConfig> {
        Binding(
            get: { app.config.folderWebhooks[folder] ?? WebhookConfig() },
            set: { app.config.folderWebhooks[folder] = $0 }
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label(folder, systemImage: "folder.fill")
                    .font(.callout.weight(.semibold))
                Spacer()
                Toggle("Webhook", isOn: binding.enabled)
                    .toggleStyle(.switch)
                    .controlSize(.small)
            }
            if binding.wrappedValue.enabled {
                TextField("https://example.com/hook", text: binding.url)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.callout, design: .monospaced))
                Toggle("Attach audio file (multipart)", isOn: binding.includeAudio)
                    .toggleStyle(.checkbox)
                    .font(.callout)
            }
        }
        .vvCard()
    }
}

// MARK: - General

// MARK: - Multi-machine

/// Pairing and peers (docs/multi-machine.md).
struct MultiMachineSettings: View {
    @EnvironmentObject var app: AppState
    @State private var pairAddress = ""
    @State private var pairingCode: String?
    @State private var pairingAnswer: ((Bool) -> Void)?
    @State private var pairingStatus: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("This machine").vvSectionTitle()
                    Toggle("Allow paired machines to connect", isOn: $app.config.multiMachine.enabled)
                    HStack(spacing: 8) {
                        TextField("Machine name", text: $app.config.multiMachine.machineName,
                                  prompt: Text(MultiMachineConfig().resolvedMachineName))
                            .frame(maxWidth: 220)
                        TextField("Port", value: $app.config.multiMachine.port, format: .number.grouping(.never))
                            .frame(width: 70)
                        Spacer()
                    }
                    if app.config.multiMachine.enabled {
                        let fp = PeerService.shared.fingerprintHex
                        Text("Identity: \(fp.isEmpty ? "created when the listener starts" : String(fp.prefix(16)) + "…")")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                    Text("Machines pair once with a 6-digit code confirmed on both screens, then talk over TLS with pinned identities. Use tailnet or LAN addresses only.")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .vvCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Paired machines").vvSectionTitle()
                    if app.config.multiMachine.peers.isEmpty {
                        Text("No machines paired yet.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    ForEach($app.config.multiMachine.peers) { $peer in
                        VStack(alignment: .leading, spacing: 6) {
                            HStack(spacing: 8) {
                                Text(peer.name).font(.callout.weight(.semibold))
                                Text(String(peer.fingerprint.prefix(12)) + "…")
                                    .font(.caption.monospaced())
                                    .foregroundStyle(.secondary)
                                Spacer()
                                Button {
                                    app.config.multiMachine.peers.removeAll { $0.fingerprint == peer.fingerprint }
                                } label: {
                                    Image(systemName: "trash")
                                }
                                .buttonStyle(.borderless)
                            }
                            HStack(spacing: 8) {
                                TextField("Address (host or host:port)", text: $peer.address)
                                    .frame(maxWidth: 240)
                                Toggle("May see my screens", isOn: $peer.allowScreens)
                                    .toggleStyle(.checkbox)
                                Toggle("May paste into me", isOn: $peer.allowDeliver)
                                    .toggleStyle(.checkbox)
                                Spacer()
                            }
                            .font(.caption)
                        }
                        .padding(.vertical, 2)
                        if peer.fingerprint != app.config.multiMachine.peers.last?.fingerprint { Divider() }
                    }
                }
                .vvCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Pair a new machine").vvSectionTitle()
                    HStack(spacing: 8) {
                        TextField("Other machine's address (host or host:port)", text: $pairAddress)
                            .frame(maxWidth: 280)
                        Button("Pair…") { startPairing() }
                            .disabled(pairAddress.trimmingCharacters(in: .whitespaces).isEmpty || pairingCode != nil)
                        Spacer()
                    }
                    if let code = pairingCode {
                        HStack(spacing: 12) {
                            Text("\(code.prefix(3)) \(code.suffix(3))")
                                .font(.system(size: 28, weight: .bold, design: .monospaced))
                            VStack(alignment: .leading, spacing: 4) {
                                Text("Does the other machine show the same code?")
                                    .font(.caption)
                                HStack {
                                    Button("They match — pair") { answerPairing(true) }
                                        .controlSize(.small)
                                    Button("Cancel") { answerPairing(false) }
                                        .controlSize(.small)
                                }
                            }
                            Spacer()
                        }
                        .padding(10)
                        .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 8))
                    }
                    if let status = pairingStatus {
                        Text(status)
                            .font(.caption)
                            .foregroundStyle(status.hasPrefix("Paired") ? Color.green : Color.orange)
                    }
                    Text("VoiceVector must be running (and enabled above) on the other machine. Start pairing from either side.")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                .vvCard()
            }
            .padding(16)
        }
    }

    private func startPairing() {
        pairingStatus = nil
        let address = pairAddress.trimmingCharacters(in: .whitespaces)
        PeerService.shared.pair(address: address) { code, answer in
            pairingCode = code
            pairingAnswer = answer
        } completion: { result in
            pairingCode = nil
            pairingAnswer = nil
            switch result {
            case .success(let peer):
                if !app.config.multiMachine.peers.contains(where: { $0.fingerprint == peer.fingerprint }) {
                    app.config.multiMachine.peers.append(peer)
                }
                pairingStatus = "Paired with \(peer.name)."
                pairAddress = ""
            case .failure(let error):
                pairingStatus = "Pairing failed: \(error.localizedDescription)"
            }
        }
    }

    private func answerPairing(_ accepted: Bool) {
        pairingAnswer?(accepted)
        pairingCode = nil
        pairingAnswer = nil
        if !accepted { pairingStatus = "Cancelled." }
    }
}

struct GeneralSettings: View {
    @EnvironmentObject var app: AppState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Behavior").vvSectionTitle()
                    Toggle("Play chime when recording starts/stops", isOn: $app.config.playSounds)
                    Toggle("Paste transcript into the active app", isOn: $app.config.autoPaste)
                    Toggle("Use AppleScript for pasting (if normal paste doesn't work)",
                           isOn: $app.config.appleScriptPaste)
                }
                .vvCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Microphone").vvSectionTitle()
                    Text("Opening an external audio interface can take half a second before recording starts. Keeping the microphone open avoids that, but macOS shows its mic-in-use indicator while it's open.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Toggle("Keep the microphone open for 15 seconds after a recording",
                           isOn: $app.config.keepMicWarmAfterRecording)
                    Toggle("Always keep the microphone open while VoiceVector is running",
                           isOn: $app.config.keepMicAlwaysWarm)
                        .disabled(!app.config.autoPaste)
                }
                .toggleStyle(.checkbox)
                .vvCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Permissions").vvSectionTitle()
                    PermissionRow(name: "Microphone", granted: app.microphoneGranted) {
                        Task { _ = await Recorder.requestPermission(); app.refreshPermissions() }
                    }
                    PermissionRow(name: "Accessibility (hotkey & paste)", granted: app.accessibilityGranted) {
                        app.requestAccessibility()
                    }
                    PermissionRow(name: "Screen Recording (optional — screenshot context)",
                                  granted: app.screenRecordingGranted) {
                        ScreenCapture.requestPermission()
                    }
                    Button("Re-check") { app.refreshPermissions() }
                        .font(.caption)
                }
                .vvCard()

                UpdatesCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Storage").vvSectionTitle()
                    HStack {
                        Text(app.config.libraryPath)
                            .font(.system(.callout, design: .monospaced))
                            .foregroundStyle(.secondary)
                        Spacer()
                        Button("Show in Finder") {
                            NSWorkspace.shared.open(app.config.expandedLibraryURL)
                        }
                    }
                    Button("Run setup wizard again") {
                        app.config.wizardCompleted = false
                        app.showSettings = false
                    }
                    .font(.caption)
                }
                .vvCard()

                if !Log.recentErrors.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Recent errors").vvSectionTitle()
                        ForEach(Log.recentErrors.suffix(8).reversed(), id: \.self) { line in
                            Text(line)
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }
                    .vvCard()
                }
            }
            .padding(8)
        }
    }
}

// MARK: - About

struct AboutSettings: View {
    private static let website = URL(string: "https://voicevector.sammons.io")!
    private static let source = URL(string: "https://github.com/Sammons/voicevector")!
    private static let changelog = URL(string: "https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md")!
    private static let license = URL(string: "https://github.com/Sammons/voicevector/blob/main/LICENSE")!
    private static let commercial = URL(string: "https://github.com/Sammons/voicevector/blob/main/COMMERCIAL.md")!
    private static let email = URL(string: "mailto:sales@sammons.io?subject=VoiceVector%20commercial%20license")!

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(alignment: .firstTextBaseline, spacing: 10) {
                        Text("VoiceVector")
                            .font(.system(.title2, design: .rounded).weight(.bold))
                        Text("Version \(UpdateService.currentVersion)")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                    Text("A Sammons Software LLC product.")
                        .font(.callout)
                    Text("© 2026 Sammons Software LLC. All rights reserved.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    HStack(spacing: 14) {
                        Link("Website", destination: Self.website)
                        Link("What's new", destination: Self.changelog)
                        Link("Source on GitHub", destination: Self.source)
                    }
                    .font(.callout)
                    .padding(.top, 2)
                }
                .vvCard()

                VStack(alignment: .leading, spacing: 8) {
                    Text("Licensing").vvSectionTitle()
                    Text("VoiceVector is source-available under the VoiceVector Community License.")
                        .fixedSize(horizontal: false, vertical: true)
                    licenseRow(icon: "person.2", title: "Free",
                               detail: "For individuals, and for any company or organization with fewer than 1,000 employees and contractors (counted together with affiliates). Commercial use included.")
                    licenseRow(icon: "building.2", title: "Commercial license — US $50 per seat per year",
                               detail: "Required for organizations with 1,000 or more employees and contractors. A seat is one person who uses VoiceVector for their work. Organizations that cross the threshold get a 90-day grace period; site licenses are available.")
                    licenseRow(icon: "checkmark.shield", title: "The same software for everyone",
                               detail: "No feature differences, no license keys, no telemetry. Compliance rests with the organization, like any other license in its software inventory.")
                    HStack(spacing: 14) {
                        Link("License text", destination: Self.license)
                        Link("Commercial terms", destination: Self.commercial)
                        Link("Buy a commercial license", destination: Self.email)
                    }
                    .font(.callout)
                    .padding(.top, 4)
                }
                .vvCard()
            }
            .padding(8)
        }
    }

    private func licenseRow(icon: String, title: String, detail: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .foregroundStyle(Theme.accent)
                .frame(width: 18)
                .padding(.top, 2)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.callout.weight(.semibold))
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.top, 2)
    }
}

/// One-click updates from GitHub Releases.
struct UpdatesCard: View {
    @State private var available: UpdateInfo?
    @State private var status = ""
    @State private var busy = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Updates").vvSectionTitle()
            HStack(spacing: 10) {
                Text("Version \(UpdateService.currentVersion)")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Spacer()
                if let available {
                    Button {
                        install(available)
                    } label: {
                        if busy { ProgressView().controlSize(.small) }
                        else { Text("Update to \(available.version) & Relaunch") }
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(Theme.accent)
                    .disabled(busy)
                } else {
                    Button {
                        check()
                    } label: {
                        if busy { ProgressView().controlSize(.small) }
                        else { Text("Check for Updates") }
                    }
                    .disabled(busy)
                }
            }
            if !status.isEmpty {
                Text(status).font(.caption).foregroundStyle(.secondary)
            }
            Text("Updates swap the app in place and relaunch. macOS may ask you to re-enable Accessibility afterwards (unsigned builds).")
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
        .vvCard()
    }

    private func check() {
        busy = true
        status = ""
        Task { @MainActor in
            do {
                if let info = try await UpdateService.fetchLatest() {
                    available = info
                    status = "Version \(info.version) is available."
                } else {
                    status = "You're up to date."
                }
            } catch {
                status = "Update check failed: \(error.localizedDescription)"
            }
            busy = false
        }
    }

    private func install(_ info: UpdateInfo) {
        busy = true
        status = "Downloading \(info.version)…"
        Task { @MainActor in
            do {
                try await UpdateService.downloadAndInstall(info)
            } catch {
                status = "Update failed: \(error.localizedDescription)"
                busy = false
            }
        }
    }
}

struct PermissionRow: View {
    let name: String
    let granted: Bool
    let onRequest: () -> Void

    var body: some View {
        HStack {
            Image(systemName: granted ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(granted ? .green : Theme.danger)
            Text(name).font(.callout)
            Spacer()
            if !granted {
                Button("Grant…", action: onRequest)
                    .controlSize(.small)
            }
        }
    }
}
