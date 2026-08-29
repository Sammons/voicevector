import SwiftUI
import AppKit

/// Collapsible folder sections with paginated entries.
struct LibraryListView: View {
    @ObservedObject var dictation: DictationController
    private static let pageSize = 25

    @State private var collapsed: Set<String> = []
    @State private var shownCounts: [String: Int] = [:]
    @State private var expandedEntry: String?

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 10) {
                ForEach(dictation.currentLibrary.folderNames(), id: \.self) { folder in
                    folderSection(folder)
                }
            }
            .padding(14)
        }
        .id(dictation.libraryGeneration) // hard refresh when entries change
    }

    @ViewBuilder
    private func folderSection(_ folder: String) -> some View {
        let library = dictation.currentLibrary
        let total = library.entryCount(folder: folder)
        let shown = shownCounts[folder] ?? Self.pageSize
        let isCollapsed = collapsed.contains(folder)

        VStack(alignment: .leading, spacing: 6) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) {
                    if isCollapsed { collapsed.remove(folder) } else { collapsed.insert(folder) }
                }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: isCollapsed ? "chevron.right" : "chevron.down")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(.tertiary)
                        .frame(width: 12)
                    Text(folder).vvSectionTitle()
                    Pill(text: "\(total)", color: .secondary)
                    Spacer()
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            if !isCollapsed {
                if total == 0 {
                    Text("No dictations yet")
                        .font(.callout)
                        .foregroundStyle(.tertiary)
                        .padding(.leading, 20)
                } else {
                    ForEach(library.entries(folder: folder, offset: 0, limit: shown), id: \.id) { entry in
                        EntryRow(entry: entry,
                                 isExpanded: expandedEntry == entry.id,
                                 dictation: dictation) {
                            withAnimation(.easeInOut(duration: 0.15)) {
                                expandedEntry = expandedEntry == entry.id ? nil : entry.id
                            }
                        }
                    }
                    if shown < total {
                        Button("Show more (\(total - shown) older)") {
                            shownCounts[folder] = shown + Self.pageSize
                        }
                        .buttonStyle(.borderless)
                        .font(.callout)
                        .padding(.leading, 20)
                    }
                }
            }
        }
    }
}

struct EntryRow: View {
    let entry: Entry
    let isExpanded: Bool
    @ObservedObject var dictation: DictationController
    let onToggle: () -> Void

    private var isError: Bool { entry.status.hasPrefix("error") }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: onToggle) {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Text(Self.timeFormatter.string(from: entry.date))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                    if isError {
                        Pill(text: "failed", color: Theme.danger)
                    } else if entry.cleanupLabel.contains("failed") {
                        Pill(text: "cleanup failed", color: .orange)
                    } else if entry.cleanupLabel.hasPrefix("not run") {
                        Pill(text: "raw", color: .secondary)
                    }
                    Text(preview)
                        .font(.callout)
                        .lineLimit(isExpanded ? nil : 1)
                        .foregroundStyle(isError ? .secondary : .primary)
                    Spacer(minLength: 4)
                    Text(durationLabel)
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.vertical, 6)

            if isExpanded {
                expandedBody
                    .padding(.top, 4)
                    .padding(.bottom, 8)
            }
        }
        .padding(.horizontal, 10)
        .background(isExpanded ? Theme.accentSoft.opacity(0.5) : Color.clear,
                    in: RoundedRectangle(cornerRadius: Theme.corner))
        .padding(.leading, 12)
    }

    private var preview: String {
        let text = entry.cleaned.isEmpty ? entry.status : entry.cleaned
        return text.replacingOccurrences(of: "\n", with: " ")
    }

    private var durationLabel: String {
        entry.duration >= 60
            ? String(format: "%d:%02d", Int(entry.duration) / 60, Int(entry.duration) % 60)
            : String(format: "%.0fs", entry.duration)
    }

    private var expandedBody: some View {
        VStack(alignment: .leading, spacing: 10) {
            if isError {
                Text(entry.status)
                    .font(.callout)
                    .foregroundStyle(Theme.danger)
                    .textSelection(.enabled)
            }
            if !entry.cleaned.isEmpty {
                transcriptBlock(title: "Cleaned", text: entry.cleaned)
            }
            if !entry.raw.isEmpty, entry.raw != entry.cleaned {
                transcriptBlock(title: "Raw", text: entry.raw)
            }
            if !entry.sttLabel.isEmpty || !entry.cleanupLabel.isEmpty {
                Text([entry.sttLabel.isEmpty ? nil : "STT: \(entry.sttLabel)",
                      entry.cleanupLabel.isEmpty ? nil : "Cleanup: \(entry.cleanupLabel)"]
                    .compactMap { $0 }.joined(separator: "   ·   "))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .textSelection(.enabled)
            }
            HStack(spacing: 12) {
                if isError || entry.cleanupLabel.contains("failed") {
                    Button {
                        dictation.retry(entry: entry)
                    } label: {
                        Label("Retry", systemImage: "arrow.clockwise")
                    }
                }
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting([dictation.currentLibrary.audioURL(entry)])
                } label: {
                    Label("Show Files", systemImage: "folder")
                }
                Spacer()
                Button(role: .destructive) {
                    dictation.currentLibrary.delete(entry)
                    dictation.reloadLibraryRoot()
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            }
            .buttonStyle(.borderless)
            .font(.callout)
            .controlSize(.small)
        }
    }

    private func transcriptBlock(title: String, text: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title).vvSectionTitle()
                Spacer()
                CopyButton(text: text)
            }
            Text(text)
                .font(.callout)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .vvCard()
    }

    private static let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "MMM d  HH:mm"
        return formatter
    }()
}

struct CopyButton: View {
    let text: String
    @State private var copied = false

    var body: some View {
        Button {
            let pasteboard = NSPasteboard.general
            pasteboard.clearContents()
            pasteboard.setString(text, forType: .string)
            copied = true
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 1_200_000_000)
                copied = false
            }
        } label: {
            Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc")
                .font(.caption)
        }
        .buttonStyle(.borderless)
        .foregroundStyle(copied ? .green : Theme.accent)
    }
}
