import AppKit
import CoreGraphics

/// One display's capture plus the caption sent as a text part before it.
struct ScreenshotAttachment {
    let jpeg: Data
    let caption: String

    static func caption(index: Int, total: Int, active: Bool, outlined: Bool) -> String {
        var text = "Display \(index) of \(total)"
        if active {
            text += " — ACTIVE: the dictated text will be inserted here"
            if outlined { text += "; the target window is outlined in red" }
        }
        return text + "."
    }
}

/// Screenshots of every display, for LLM context. Needs the Screen Recording
/// permission; without it the captures are blank and we return nothing
/// rather than send empty images.
enum ScreenCapture {
    static var permissionGranted: Bool { CGPreflightScreenCaptureAccess() }

    /// Shows the system prompt (once) and opens the Privacy pane on denial.
    static func requestPermission() {
        if !CGRequestScreenCaptureAccess() {
            let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!
            NSWorkspace.shared.open(url)
        }
    }

    /// One capture per display, the display holding the frontmost window
    /// first (its captioned "active", with the target window outlined in red),
    /// then the rest left-to-right. Empty when permission is missing.
    static func allScreens(maxWidth: CGFloat = 1280) -> [ScreenshotAttachment] {
        guard permissionGranted else { return [] }
        let target = targetWindowBounds()
        let active = activeScreen(targetBounds: target)
        let ordered = NSScreen.screens.sorted { a, b in
            let fa = a == active, fb = b == active
            if fa != fb { return fa }
            return a.frame.minX < b.frame.minX
        }
        var shots: [(jpeg: Data, active: Bool, outlined: Bool)] = []
        for screen in ordered {
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID,
                  let image = CGDisplayCreateImage(number),
                  image.width > 8, image.height > 8 else { continue }
            let isActive = screen == active
            var highlight: CGRect?
            if isActive, let target {
                // CGDisplayBounds and CGWindow bounds share the global top-left origin.
                let display = CGDisplayBounds(number)
                let scale = CGFloat(image.width) / display.width
                highlight = CGRect(x: (target.minX - display.minX) * scale, y: (target.minY - display.minY) * scale,
                                   width: target.width * scale, height: target.height * scale)
            }
            guard let data = jpeg(image, maxWidth: maxWidth, highlight: highlight) else { continue }
            shots.append((data, isActive, highlight != nil))
        }
        // Captions count captures, not screens, in case one failed.
        return shots.enumerated().map { i, shot in
            ScreenshotAttachment(jpeg: shot.jpeg, caption: ScreenshotAttachment.caption(
                index: i + 1, total: shots.count, active: shot.active, outlined: shot.outlined))
        }
    }

    /// Bounds (global, top-left origin) of the frontmost app's front window.
    private static func targetWindowBounds() -> CGRect? {
        guard let app = NSWorkspace.shared.frontmostApplication,
              app.processIdentifier != ProcessInfo.processInfo.processIdentifier,
              let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements],
                                                       kCGNullWindowID) as? [[String: Any]],
              let front = windows.first(where: {
                  ($0[kCGWindowOwnerPID as String] as? Int32) == app.processIdentifier
                      && ($0[kCGWindowLayer as String] as? Int) == 0
              }),
              let b = front[kCGWindowBounds as String] as? [String: CGFloat] else { return nil }
        return CGRect(x: b["X"] ?? 0, y: b["Y"] ?? 0, width: b["Width"] ?? 0, height: b["Height"] ?? 0)
    }

    /// The screen containing the target window's center; else the screen
    /// under the mouse; else the main screen.
    private static func activeScreen(targetBounds: CGRect?) -> NSScreen? {
        if let targetBounds {
            let primaryHeight = NSScreen.screens.first?.frame.height ?? 0
            // CGWindow bounds are top-left origin; flip into AppKit coordinates.
            let center = NSPoint(x: targetBounds.midX, y: primaryHeight - targetBounds.midY)
            if let screen = NSScreen.screens.first(where: { $0.frame.contains(center) }) { return screen }
        }
        let mouse = NSEvent.mouseLocation
        return NSScreen.screens.first(where: { $0.frame.contains(mouse) }) ?? NSScreen.main
    }

    private static func jpeg(_ image: CGImage, maxWidth: CGFloat, highlight: CGRect? = nil) -> Data? {
        let scale = min(1, maxWidth / CGFloat(image.width))
        let size = NSSize(width: CGFloat(image.width) * scale, height: CGFloat(image.height) * scale)
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width),
                                   pixelsHigh: Int(size.height), bitsPerSample: 8,
                                   samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                   colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)
        guard let rep, let context = NSGraphicsContext(bitmapImageRep: rep) else { return nil }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = context
        context.cgContext.interpolationQuality = .high
        context.cgContext.draw(image, in: CGRect(origin: .zero, size: size))
        if let highlight {
            // Image pixel space is top-left origin; the bitmap context is bottom-left.
            let r = CGRect(x: highlight.minX * scale, y: size.height - (highlight.maxY * scale),
                           width: highlight.width * scale, height: highlight.height * scale)
            let cg = context.cgContext
            cg.setStrokeColor(CGColor(red: 1, green: 0.1, blue: 0.1, alpha: 1))
            cg.setLineWidth(6)
            cg.stroke(r.insetBy(dx: 3, dy: 3))
        }
        NSGraphicsContext.restoreGraphicsState()
        return rep.representation(using: .jpeg, properties: [.compressionFactor: 0.6])
    }
}
