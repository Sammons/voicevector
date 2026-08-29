import SwiftUI
import AppKit

/// The one main screen: header with record/settings, collapsible folder
/// sections of past dictations, paginated.
struct MainView: View {
    @EnvironmentObject var app: AppState
    @ObservedObject var dictation: DictationController

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if case .failed(let message) = dictation.state {
                errorBanner(message)
            }
            LibraryListView(dictation: dictation)
            Divider()
            footer
        }
        .frame(minWidth: 560, minHeight: 480)
        .background(.background)
        .sheet(isPresented: $app.showSettings) {
            SettingsView()
                .environmentObject(app)
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            Image(systemName: "waveform.circle.fill")
                .font(.system(size: 26))
                .foregroundStyle(Theme.accent)
            VStack(alignment: .leading, spacing: 1) {
                Text("VoiceVector")
                    .font(.system(.title3, design: .rounded).weight(.bold))
                Text(statusLine)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            RecordButton(dictation: dictation)
            Button {
                app.showSettings = true
            } label: {
                Image(systemName: "gearshape")
                    .font(.system(size: 15, weight: .medium))
            }
            .buttonStyle(.borderless)
            .help("Settings")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private var statusLine: String {
        switch dictation.state {
        case .idle:
            return "Press \(app.hotkeyDescription) anywhere to dictate"
        case .recording:
            return "Recording…"
        case .processing(let step):
            return step
        case .reviewing:
            return "Reviewing — press the hotkey to say a change, ⏎ to paste, Esc to discard"
        case .failed:
            return "Something went wrong"
        }
    }

    private func errorBanner(_ message: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
            Text(message).lineLimit(2)
            Spacer()
        }
        .font(.callout)
        .foregroundStyle(Theme.danger)
        .padding(10)
        .background(Theme.danger.opacity(0.08))
    }

    private var footer: some View {
        HStack(spacing: 8) {
            Text("New dictations go to")
                .foregroundStyle(.secondary)
            Picker("", selection: $app.config.activeFolder) {
                ForEach(dictation.currentLibrary.folderNames(), id: \.self) { name in
                    Text(name).tag(name)
                }
            }
            .labelsHidden()
            .fixedSize()
            Spacer()
            NewFolderButton(dictation: dictation)
        }
        .font(.callout)
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}

struct RecordButton: View {
    @ObservedObject var dictation: DictationController

    var body: some View {
        Button {
            if dictation.isRecording {
                dictation.finishRecording()
            } else {
                dictation.startRecording()
            }
        } label: {
            HStack(spacing: 6) {
                Image(systemName: dictation.isRecording ? "stop.fill" : "mic.fill")
                Text(dictation.isRecording ? "Stop" : "Record")
            }
            .font(.system(.callout, design: .rounded).weight(.semibold))
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(dictation.isRecording ? Theme.danger : Theme.accent, in: Capsule())
            .foregroundStyle(.white)
        }
        .buttonStyle(.plain)
        .disabled({ if case .processing = dictation.state { return true } else { return false } }())
    }
}

struct NewFolderButton: View {
    @ObservedObject var dictation: DictationController
    @State private var showingPrompt = false
    @State private var name = ""

    var body: some View {
        Button {
            showingPrompt = true
        } label: {
            Label("New Folder", systemImage: "folder.badge.plus")
                .font(.callout)
        }
        .buttonStyle(.borderless)
        .popover(isPresented: $showingPrompt) {
            VStack(alignment: .leading, spacing: 10) {
                Text("New folder").font(.headline)
                TextField("Name", text: $name)
                    .frame(width: 180)
                    .onSubmit(create)
                HStack {
                    Spacer()
                    Button("Create", action: create)
                        .keyboardShortcut(.defaultAction)
                        .disabled(Library.sanitize(name).isEmpty)
                }
            }
            .padding(14)
        }
    }

    private func create() {
        try? dictation.currentLibrary.createFolder(name)
        name = ""
        showingPrompt = false
        dictation.reloadLibraryRoot()
    }
}
