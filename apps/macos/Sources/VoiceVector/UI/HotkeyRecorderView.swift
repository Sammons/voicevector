import SwiftUI

/// Click, then press the desired key (or tap a lone modifier like Right ⌥).
struct HotkeyRecorderView: View {
    @EnvironmentObject var app: AppState
    @State private var capturing = false

    var body: some View {
        HStack(spacing: 10) {
            Button {
                toggleCapture()
            } label: {
                Text(capturing ? "Press a key…" : app.hotkeyDescription)
                    .font(.system(.body, design: .rounded).weight(.semibold))
                    .frame(minWidth: 120)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 7)
                    .background(capturing ? Theme.accent.opacity(0.25) : Theme.accentSoft,
                                in: RoundedRectangle(cornerRadius: 8))
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke(capturing ? Theme.accent : .clear, lineWidth: 1.5))
                    .foregroundStyle(Theme.accent)
            }
            .buttonStyle(.plain)
            if capturing {
                Button("Cancel") { stopCapture() }
                    .controlSize(.small)
            } else {
                Text("Click, then press a key or tap a modifier (e.g. Right ⌥, fn)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .onDisappear { stopCapture() }
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
        app.hotkey.onCaptureKey = { spec in
            app.config.primaryHotkey = spec
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
