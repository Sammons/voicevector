import SwiftUI

/// First-run walkthrough: welcome → permissions → provider → hotkey → done.
struct WizardView: View {
    @EnvironmentObject var app: AppState
    @State private var step = 0
    private let steps = ["Welcome", "Permissions", "Provider", "Hotkey", "Ready"]

    var body: some View {
        VStack(spacing: 0) {
            // Progress dots
            HStack(spacing: 8) {
                ForEach(steps.indices, id: \.self) { index in
                    Circle()
                        .fill(index <= step ? Theme.accent : Color.secondary.opacity(0.25))
                        .frame(width: 8, height: 8)
                }
            }
            .padding(.top, 24)

            Group {
                switch step {
                case 0: welcome
                case 1: permissions
                case 2: provider
                case 3: hotkeyStep
                default: done
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .padding(28)

            Divider()
            HStack {
                if step > 0, step < steps.count - 1 {
                    Button("Back") { step -= 1 }
                }
                Spacer()
                if step < steps.count - 1 {
                    Button(step == 0 ? "Get Started" : "Continue") { step += 1 }
                        .keyboardShortcut(.defaultAction)
                        .buttonStyle(.borderedProminent)
                        .tint(Theme.accent)
                        .disabled(step == 1 && !(app.microphoneGranted && app.accessibilityGranted))
                } else {
                    Button("Start Dictating") {
                        app.config.wizardCompleted = true
                    }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .tint(Theme.accent)
                }
            }
            .padding(16)
        }
        .frame(minWidth: 560, minHeight: 480)
        .background(.background)
        .onAppear { app.refreshPermissions() }
    }

    private var welcome: some View {
        VStack(spacing: 14) {
            Image(systemName: "waveform.circle.fill")
                .font(.system(size: 64))
                .foregroundStyle(Theme.accent)
            Text("Welcome to VoiceVector")
                .font(.system(.largeTitle, design: .rounded).weight(.bold))
            Text("Press a hotkey anywhere, speak, and the cleaned-up text is typed right where your cursor is. Recordings and transcripts stay on your Mac as plain files.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 400)
        }
    }

    private var permissions: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Two permissions")
                .font(.system(.title2, design: .rounded).weight(.bold))
            Text("VoiceVector needs the microphone to record, and Accessibility to listen for the hotkey and paste text for you.")
                .foregroundStyle(.secondary)
            VStack(spacing: 12) {
                wizardPermission(icon: "mic.fill", name: "Microphone",
                                 granted: app.microphoneGranted) {
                    Task { _ = await Recorder.requestPermission(); app.refreshPermissions() }
                }
                wizardPermission(icon: "accessibility", name: "Accessibility",
                                 granted: app.accessibilityGranted) {
                    app.requestAccessibility()
                }
            }
            Text("If a grant dialog doesn't appear, enable VoiceVector manually in System Settings → Privacy & Security, then click Re-check.")
                .font(.caption)
                .foregroundStyle(.tertiary)
            Button("Re-check") { app.refreshPermissions() }
        }
        .frame(maxWidth: 440)
    }

    private func wizardPermission(icon: String, name: String, granted: Bool,
                                  onRequest: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 20))
                .frame(width: 30)
                .foregroundStyle(Theme.accent)
            Text(name).font(.headline)
            Spacer()
            if granted {
                Label("Granted", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
            } else {
                Button("Grant…", action: onRequest)
                    .buttonStyle(.borderedProminent)
                    .tint(Theme.accent)
            }
        }
        .vvCard()
    }

    private var provider: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Connect a transcription service")
                .font(.system(.title2, design: .rounded).weight(.bold))
            Text("Pick at least one. You can add more and choose cleanup models later in Settings.")
                .foregroundStyle(.secondary)
            if app.config.providers.isEmpty {
                HStack(spacing: 10) {
                    ForEach(ProviderKind.allCases) { kind in
                        Button {
                            _ = app.addProvider(kind)
                        } label: {
                            VStack(spacing: 6) {
                                Image(systemName: kind.wizardIcon)
                                    .font(.system(size: 22))
                                Text(kind.displayName).font(.callout.weight(.medium))
                                Text(kind.wizardSubtitle)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 16)
                            .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: Theme.corner))
                        }
                        .buttonStyle(.plain)
                    }
                }
            } else {
                WizardProviderEditors()
            }
        }
    }

    private var hotkeyStep: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Choose your hotkey")
                .font(.system(.title2, design: .rounded).weight(.bold))
            HotkeyRecorderView()
            VStack(alignment: .leading, spacing: 6) {
                Label("Hold it down to talk, release to finish", systemImage: "hand.tap")
                Label(app.config.tapStartMode == .doubleTap
                      ? "Or double-tap to start, tap once to stop"
                      : "Or tap to start, tap again to stop", systemImage: "hand.tap.fill")
                Label("Esc cancels a recording", systemImage: "escape")
            }
            .font(.callout)
            .foregroundStyle(.secondary)
            Picker("Tap behavior", selection: $app.config.tapStartMode) {
                ForEach(TapStartMode.allCases, id: \.self) { mode in
                    Text(mode.label).tag(mode)
                }
            }
            .pickerStyle(.radioGroup)
        }
        .frame(maxWidth: 440)
    }

    private var done: some View {
        VStack(spacing: 14) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 56))
                .foregroundStyle(.green)
            Text("You're set")
                .font(.system(.largeTitle, design: .rounded).weight(.bold))
            Text("Click into any text field and press \(app.hotkeyDescription). Your dictations are saved in \(app.config.libraryPath).")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 400)
        }
    }
}

extension ProviderKind {
    var wizardIcon: String {
        switch self {
        case .elevenLabs: return "waveform"
        case .fireworks: return "flame"
        case .cerebras: return "bolt.fill"
        case .vercelGateway: return "triangle"
        case .openAICompatible: return "server.rack"
        }
    }

    var wizardSubtitle: String {
        switch self {
        case .elevenLabs: return "Transcription"
        case .fireworks: return "Cleanup LLM"
        case .cerebras: return "Fast cleanup LLM"
        case .vercelGateway: return "STT + LLM"
        case .openAICompatible: return "STT + LLM"
        }
    }
}

/// Compact editors for the providers added during the wizard.
struct WizardProviderEditors: View {
    @EnvironmentObject var app: AppState

    var body: some View {
        ScrollView {
            VStack(spacing: 10) {
                ForEach($app.config.providers) { $profile in
                    WizardProviderCard(profile: $profile)
                }
                Menu("Add another…") {
                    ForEach(ProviderKind.allCases) { kind in
                        Button(kind.displayName) { _ = app.addProvider(kind) }
                    }
                }
                .fixedSize()
            }
        }
    }
}

struct WizardProviderCard: View {
    @Binding var profile: ProviderProfile
    @State private var apiKey = ""
    @State private var testResult: String?
    @State private var testFailed = false
    @State private var busy = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(profile.name).font(.headline)
            if profile.kind == .openAICompatible {
                TextField("Base URL", text: $profile.baseURL)
                    .font(.system(.callout, design: .monospaced))
            }
            SecureField(profile.kind == .openAICompatible ? "API key (optional for local servers)" : "API key",
                        text: $apiKey)
                .onChange(of: apiKey) { _, newValue in
                    Keychain.setAPIKey(newValue, for: profile.id)
                }
            HStack {
                Button {
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
                } label: {
                    if busy { ProgressView().controlSize(.small) } else { Text("Test") }
                }
                if let testResult {
                    Label(testResult, systemImage: testFailed ? "xmark.circle.fill" : "checkmark.circle.fill")
                        .font(.caption)
                        .foregroundStyle(testFailed ? Theme.danger : .green)
                        .lineLimit(2)
                }
                Spacer()
            }
        }
        .textFieldStyle(.roundedBorder)
        .vvCard()
        .onAppear { apiKey = Keychain.apiKey(for: profile.id) }
    }
}
