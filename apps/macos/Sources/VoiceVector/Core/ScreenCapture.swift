import AppKit
import CoreGraphics

/// Screenshot of the frontmost app's main window, for LLM context. Needs the
/// Screen Recording permission; without it the capture is blank and we
/// return nil rather than send an empty image.
enum ScreenCapture {
    static var permissionGranted: Bool { CGPreflightScreenCaptureAccess() }

    /// Shows the system prompt (once) and opens the Privacy pane on denial.
    static func requestPermission() {
        if !CGRequestScreenCaptureAccess() {
            let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!
            NSWorkspace.shared.open(url)
        }
    }

    /// JPEG, at most `maxWidth` px wide. nil when nothing sensible is on screen.
    static func frontmostWindowJPEG(maxWidth: CGFloat = 1280) -> Data? {
        guard permissionGranted,
              let app = NSWorkspace.shared.frontmostApplication,
              app.processIdentifier != ProcessInfo.processInfo.processIdentifier else { return nil }
        let pid = app.processIdentifier
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
            return nil
        }
        // Frontmost app's largest on-screen, layer-0 window.
        let candidates = windows.filter { info in
            (info[kCGWindowOwnerPID as String] as? Int32) == pid
                && (info[kCGWindowLayer as String] as? Int) == 0
        }
        guard let window = candidates.max(by: { area($0) < area($1) }),
              let id = window[kCGWindowNumber as String] as? CGWindowID,
              let image = CGWindowListCreateImage(.null, .optionIncludingWindow, id,
                                                  [.boundsIgnoreFraming, .bestResolution]),
              image.width > 8, image.height > 8 else { return nil }
        return jpeg(image, maxWidth: maxWidth)
    }

    private static func area(_ info: [String: Any]) -> CGFloat {
        guard let bounds = info[kCGWindowBounds as String] as? [String: CGFloat] else { return 0 }
        return (bounds["Width"] ?? 0) * (bounds["Height"] ?? 0)
    }

    private static func jpeg(_ image: CGImage, maxWidth: CGFloat) -> Data? {
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
        NSGraphicsContext.restoreGraphicsState()
        return rep.representation(using: .jpeg, properties: [.compressionFactor: 0.6])
    }
}
