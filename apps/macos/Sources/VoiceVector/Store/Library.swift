import Foundation

/// One dictation on disk: `<stamp>.wav` + `<stamp>.md` in a folder directory.
struct Entry: Identifiable, Equatable {
    var id: String          // basename without extension, e.g. "20260825-153600"
    var folder: String
    var date: Date
    var duration: Double
    var sttLabel: String
    var cleanupLabel: String
    var status: String      // "complete" | "error: ..."
    var cleaned: String
    var raw: String

    var audioFilename: String { id + ".wav" }
    var markdownFilename: String { id + ".md" }
}

/// Files-first store: folders are directories under the library root, entries
/// are WAV + Markdown pairs. The markdown is the source of truth and is written
/// to be pleasant to read outside the app.
final class Library {
    let root: URL

    init(root: URL) {
        self.root = root
        try? FileManager.default.createDirectory(at: root.appendingPathComponent("Inbox"),
                                                 withIntermediateDirectories: true)
    }

    // MARK: Folders

    func folderNames() -> [String] {
        let fm = FileManager.default
        let items = (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                                 options: .skipsHiddenFiles)) ?? []
        var names = items.filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
            .map(\.lastPathComponent)
            .sorted { $0.localizedCaseInsensitiveCompare($1) == .orderedAscending }
        // Inbox always exists and sorts first.
        if let index = names.firstIndex(of: "Inbox") {
            names.remove(at: index)
        }
        return ["Inbox"] + names
    }

    func createFolder(_ name: String) throws {
        let sanitized = Library.sanitize(name)
        guard !sanitized.isEmpty else { return }
        try FileManager.default.createDirectory(at: root.appendingPathComponent(sanitized),
                                                withIntermediateDirectories: true)
    }

    func folderURL(_ name: String) -> URL {
        root.appendingPathComponent(name, isDirectory: true)
    }

    static func sanitize(_ name: String) -> String {
        name.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
    }

    // MARK: Entries

    /// Newest-first ids (cheap: directory listing only, no parsing).
    func entryIDs(folder: String) -> [String] {
        let url = folderURL(folder)
        let files = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil,
                                                                  options: .skipsHiddenFiles)) ?? []
        return files.filter { $0.pathExtension == "md" }
            .map { $0.deletingPathExtension().lastPathComponent }
            .sorted(by: >)
    }

    func entry(folder: String, id: String) -> Entry? {
        let url = folderURL(folder).appendingPathComponent(id + ".md")
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        return Library.parse(markdown: text, id: id, folder: folder)
    }

    func entries(folder: String, offset: Int, limit: Int) -> [Entry] {
        let ids = entryIDs(folder: folder)
        guard offset < ids.count else { return [] }
        return ids[offset..<min(offset + limit, ids.count)].compactMap { entry(folder: folder, id: $0) }
    }

    func entryCount(folder: String) -> Int { entryIDs(folder: folder).count }

    /// Reserves a fresh id + audio URL in the folder (collision-safe).
    func newEntrySlot(folder: String) -> (id: String, audioURL: URL) {
        try? FileManager.default.createDirectory(at: folderURL(folder), withIntermediateDirectories: true)
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        formatter.locale = Locale(identifier: "en_US_POSIX")
        var id = formatter.string(from: Date())
        var suffix = 1
        while FileManager.default.fileExists(atPath: folderURL(folder).appendingPathComponent(id + ".wav").path) {
            suffix += 1
            id = formatter.string(from: Date()) + "-\(suffix)"
        }
        return (id, folderURL(folder).appendingPathComponent(id + ".wav"))
    }

    func save(_ entry: Entry) {
        let url = folderURL(entry.folder).appendingPathComponent(entry.markdownFilename)
        do {
            try Library.render(entry).write(to: url, atomically: true, encoding: .utf8)
        } catch {
            Log.error("Failed to save transcript \(entry.id): \(error.localizedDescription)")
        }
    }

    func delete(_ entry: Entry) {
        let dir = folderURL(entry.folder)
        try? FileManager.default.removeItem(at: dir.appendingPathComponent(entry.markdownFilename))
        try? FileManager.default.removeItem(at: dir.appendingPathComponent(entry.audioFilename))
    }

    func audioURL(_ entry: Entry) -> URL {
        folderURL(entry.folder).appendingPathComponent(entry.audioFilename)
    }

    // MARK: Markdown format

    static let dateFormatter: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        return formatter
    }()

    static func render(_ entry: Entry) -> String {
        var lines: [String] = ["---"]
        lines.append("date: \(dateFormatter.string(from: entry.date))")
        lines.append("duration: \(String(format: "%.1f", entry.duration))")
        lines.append("audio: \(entry.audioFilename)")
        lines.append("stt: \(entry.sttLabel)")
        if !entry.cleanupLabel.isEmpty { lines.append("cleanup: \(entry.cleanupLabel)") }
        lines.append("status: \(entry.status)")
        lines.append("---")
        lines.append("")
        lines.append(entry.cleaned)
        if !entry.raw.isEmpty && entry.raw != entry.cleaned {
            lines.append("")
            lines.append("## Raw transcript")
            lines.append("")
            lines.append(entry.raw)
        }
        lines.append("")
        return lines.joined(separator: "\n")
    }

    static func parse(markdown: String, id: String, folder: String) -> Entry {
        var entry = Entry(id: id, folder: folder, date: Date(timeIntervalSince1970: 0), duration: 0,
                          sttLabel: "", cleanupLabel: "", status: "complete", cleaned: "", raw: "")
        var lines = markdown.components(separatedBy: "\n")

        if lines.first == "---", let end = lines.dropFirst().firstIndex(of: "---") {
            for line in lines[1..<end] {
                guard let colon = line.firstIndex(of: ":") else { continue }
                let key = String(line[..<colon])
                let value = String(line[line.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
                switch key {
                case "date": entry.date = dateFormatter.date(from: value) ?? entry.date
                case "duration": entry.duration = Double(value) ?? 0
                case "stt": entry.sttLabel = value
                case "cleanup": entry.cleanupLabel = value
                case "status": entry.status = value
                default: break
                }
            }
            lines.removeSubrange(0...end)
        }

        let body = lines.joined(separator: "\n")
        if let range = body.range(of: "\n## Raw transcript\n") {
            entry.cleaned = String(body[..<range.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
            entry.raw = String(body[range.upperBound...]).trimmingCharacters(in: .whitespacesAndNewlines)
        } else {
            entry.cleaned = body.trimmingCharacters(in: .whitespacesAndNewlines)
            entry.raw = entry.cleaned
        }
        // Fall back to parsing the id for files with a damaged header.
        if entry.date == Date(timeIntervalSince1970: 0) {
            let formatter = DateFormatter()
            formatter.dateFormat = "yyyyMMdd-HHmmss"
            formatter.locale = Locale(identifier: "en_US_POSIX")
            entry.date = formatter.date(from: String(id.prefix(15))) ?? entry.date
        }
        return entry
    }
}
