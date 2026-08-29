import AppKit
import SwiftUI

/// `VoiceVector --snapshot-ui <dir>` renders the settings sheet and each
/// settings pane offscreen to PNGs, then exits. The app draws its own views,
/// so no screen-recording permission is involved — this is how UI layout gets
/// reviewed (and regression-checked) from the CLI.
@MainActor
enum SnapshotUI {
    static func run(outputDir: String) -> Never {
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let state = AppState()
        let dir = URL(fileURLWithPath: (outputDir as NSString).expandingTildeInPath,
                      isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)

        snapshot(SettingsView(), state: state,
                 size: NSSize(width: 620, height: 560), to: dir, name: "settings")
        // Panes render taller than the sheet so scrolled-away content is visible.
        snapshot(pane(DictationSettings()), state: state,
                 size: NSSize(width: 596, height: 900), to: dir, name: "dictation")
        snapshot(pane(ProvidersSettings()), state: state,
                 size: NSSize(width: 596, height: 500), to: dir, name: "providers")
        snapshot(pane(FoldersSettings()), state: state,
                 size: NSSize(width: 596, height: 500), to: dir, name: "folders")
        snapshot(pane(GeneralSettings()), state: state,
                 size: NSSize(width: 596, height: 700), to: dir, name: "general")
        snapshot(pane(AboutSettings()), state: state,
                 size: NSSize(width: 596, height: 620), to: dir, name: "about")
        print("snapshots written to \(dir.path)")
        exit(0)
    }

    private static func pane<V: View>(_ view: V) -> some View {
        view.padding(12).background(Color(nsColor: .windowBackgroundColor))
    }

    private static func snapshot<V: View>(_ view: V, state: AppState, size: NSSize,
                                          to dir: URL, name: String) {
        let host = NSHostingView(rootView: AnyView(view.environmentObject(state)))
        host.frame = NSRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = host
        // Keep it off every display; cacheDisplay renders regardless.
        window.setFrameOrigin(NSPoint(x: -10_000, y: -10_000))
        window.orderFrontRegardless()
        host.layoutSubtreeIfNeeded()
        // Let SwiftUI finish its async layout/render passes.
        for _ in 0..<12 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
            print("snapshot \(name): no bitmap rep")
            return
        }
        host.cacheDisplay(in: host.bounds, to: rep)
        if let png = rep.representation(using: NSBitmapImageRep.FileType.png, properties: [:]) {
            try? png.write(to: dir.appendingPathComponent("\(name).png"))
        }
        window.orderOut(nil)
    }
}
