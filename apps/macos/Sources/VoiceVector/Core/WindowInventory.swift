import AppKit
import CoreGraphics

/// A window another machine (or the router) can target.
struct WindowInfo {
    let id: UInt32          // CGWindowID
    let app: String
    let title: String
    let pid: pid_t
}

/// Private but stable since 10.5: CGWindowID of an AX window element.
@_silgen_name("_AXUIElementGetWindow")
private func _AXUIElementGetWindow(_ element: AXUIElement, _ out: UnsafeMutablePointer<UInt32>) -> AXError

/// Lists targetable windows and raises one. Titles come with the Screen
/// Recording permission (same one screenshot context needs); activation
/// uses Accessibility (same one paste needs).
enum WindowInventory {
    /// Front-to-back, layer-0, on-screen windows of other apps.
    static func list() -> [WindowInfo] {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let raw = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else { return [] }
        let ownPID = ProcessInfo.processInfo.processIdentifier
        return raw.compactMap { info in
            guard (info[kCGWindowLayer as String] as? Int) == 0,
                  let pid = info[kCGWindowOwnerPID as String] as? Int32, pid != ownPID,
                  let id = info[kCGWindowNumber as String] as? UInt32,
                  let bounds = info[kCGWindowBounds as String] as? [String: CGFloat],
                  (bounds["Width"] ?? 0) >= 60, (bounds["Height"] ?? 0) >= 40 else { return nil }
            let app = info[kCGWindowOwnerName as String] as? String ?? "?"
            let title = info[kCGWindowName as String] as? String ?? ""
            return WindowInfo(id: id, app: app, title: title, pid: pid)
        }
    }

    /// Activates the app owning `windowID` and raises that window.
    /// Returns false when the window is gone.
    @discardableResult
    static func activate(windowID: UInt32) -> Bool {
        guard let target = list().first(where: { $0.id == windowID }) else { return false }
        guard let app = NSRunningApplication(processIdentifier: target.pid) else { return false }
        app.activate()
        let element = AXUIElementCreateApplication(target.pid)
        var value: CFTypeRef?
        if AXUIElementCopyAttributeValue(element, kAXWindowsAttribute as CFString, &value) == .success,
           let windows = value as? [AXUIElement] {
            for window in windows {
                var id: UInt32 = 0
                if _AXUIElementGetWindow(window, &id) == .success, id == windowID {
                    AXUIElementPerformAction(window, kAXRaiseAction as CFString)
                    break
                }
            }
        }
        return true
    }

    /// Router input: one numbered line per window.
    static func describe(_ windows: [WindowInfo]) -> String {
        windows.map { window in
            let title = window.title.isEmpty ? "(untitled)" : window.title
            return "\(window.id): \(window.app) — \(title)"
        }.joined(separator: "\n")
    }
}
