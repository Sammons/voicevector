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

    /// Brings the app owning `windowID` to the front, raises that window, and
    /// confirms it actually became frontmost. Returns false when the window is
    /// gone OR the system refused the focus change — callers must NOT paste on
    /// false, or the text lands in whatever was already focused.
    ///
    /// A background/accessory app can't reliably use `NSRunningApplication`
    /// activation (macOS focus-stealing prevention denies it when we aren't the
    /// active app). The Accessibility API does not hit that wall — we already
    /// hold that permission — so raise the window and set the application's
    /// `AXFrontmost`, with a System Events fallback, then verify.
    @discardableResult
    static func activate(windowID: UInt32) -> Bool {
        guard let target = list().first(where: { $0.id == windowID }) else { return false }
        let pid = target.pid

        raiseViaAccessibility(pid: pid, windowID: windowID)
        NSRunningApplication(processIdentifier: pid)?.activate(options: [.activateAllWindows])
        if pollFrontmost(pid: pid, timeout: 0.6) { return true }

        // Last resort: ask System Events to front the process (uses the
        // Accessibility permission, works from a background app).
        raiseViaSystemEvents(pid: pid)
        return pollFrontmost(pid: pid, timeout: 0.6)
    }

    private static func pollFrontmost(pid: pid_t, timeout: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        repeat {
            if NSWorkspace.shared.frontmostApplication?.processIdentifier == pid { return true }
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.04))
        } while Date() < deadline
        return NSWorkspace.shared.frontmostApplication?.processIdentifier == pid
    }

    private static func raiseViaAccessibility(pid: pid_t, windowID: UInt32) {
        let appElement = AXUIElementCreateApplication(pid)
        var value: CFTypeRef?
        if AXUIElementCopyAttributeValue(appElement, kAXWindowsAttribute as CFString, &value) == .success,
           let windows = value as? [AXUIElement] {
            for window in windows {
                var id: UInt32 = 0
                if _AXUIElementGetWindow(window, &id) == .success, id == windowID {
                    AXUIElementSetAttributeValue(window, kAXMainAttribute as CFString, kCFBooleanTrue)
                    AXUIElementSetAttributeValue(window, kAXFocusedAttribute as CFString, kCFBooleanTrue)
                    AXUIElementPerformAction(window, kAXRaiseAction as CFString)
                    break
                }
            }
        }
        // Bringing the whole app forward is what actually moves keyboard focus.
        AXUIElementSetAttributeValue(appElement, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    }

    private static func raiseViaSystemEvents(pid: pid_t) {
        let source = """
        tell application "System Events"
            set theProc to the first process whose unix id is \(pid)
            set frontmost of theProc to true
        end tell
        """
        var error: NSDictionary?
        NSAppleScript(source: source)?.executeAndReturnError(&error)
        if let error { Log.error("System Events front failed: \(error)") }
    }

    /// After foregrounding an app, its text input often isn't focused, so a
    /// synthesized paste lands nowhere. Walk the focused window's Accessibility
    /// tree and focus the first editable text field/area (the chat/terminal
    /// input), so the paste goes where the user expects. Best-effort.
    /// Focus a text input in the frontmost app (after we've foregrounded it).
    @discardableResult
    static func focusFrontmostTextInput() -> Bool {
        guard let pid = NSWorkspace.shared.frontmostApplication?.processIdentifier else { return false }
        return focusTextInput(pid: pid)
    }

    @discardableResult
    static func focusTextInput(pid: pid_t) -> Bool {
        let app = AXUIElementCreateApplication(pid)
        var windowRef: CFTypeRef?
        // Prefer the app's focused window; fall back to its main window.
        if AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute as CFString, &windowRef) != .success {
            AXUIElementCopyAttributeValue(app, kAXMainWindowAttribute as CFString, &windowRef)
        }
        guard let windowRef else { return false }
        let window = windowRef as! AXUIElement
        // If something editable is already focused, leave it.
        var focused: CFTypeRef?
        if AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute as CFString, &focused) == .success,
           let element = focused, isEditable(element as! AXUIElement) { return true }
        guard let field = firstTextInput(in: window) else { return false }
        let result = AXUIElementSetAttributeValue(field, kAXFocusedAttribute as CFString, kCFBooleanTrue)
        return result == .success
    }

    private static func isEditable(_ element: AXUIElement) -> Bool {
        var role: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, kAXRoleAttribute as CFString, &role) == .success,
              let r = role as? String else { return false }
        if r == kAXTextFieldRole || r == kAXTextAreaRole { return true }
        // Web content (Electron/browser) exposes editable areas via a settable value.
        var settable: DarwinBoolean = false
        if AXUIElementIsAttributeSettable(element, kAXValueAttribute as CFString, &settable) == .success, settable.boolValue {
            return r == "AXTextArea" || r == "AXTextField" || r == "AXComboBox"
        }
        return false
    }

    /// Breadth-first search for an editable text element in the window. Prefers
    /// a text AREA (chat/terminal inputs) but accepts a single-line field.
    private static func firstTextInput(in window: AXUIElement) -> AXUIElement? {
        var queue: [(AXUIElement, Int)] = [(window, 0)]
        var fallbackField: AXUIElement?
        var scanned = 0
        while !queue.isEmpty, scanned < 4000 {
            let (element, depth) = queue.removeFirst()
            scanned += 1
            var role: CFTypeRef?
            if AXUIElementCopyAttributeValue(element, kAXRoleAttribute as CFString, &role) == .success,
               let r = role as? String, isEditable(element) {
                if r == kAXTextAreaRole { return element }        // best match
                if fallbackField == nil { fallbackField = element } // remember a field
            }
            if depth < 16 {
                var childrenRef: CFTypeRef?
                if AXUIElementCopyAttributeValue(element, kAXChildrenAttribute as CFString, &childrenRef) == .success,
                   let children = childrenRef as? [AXUIElement] {
                    for child in children { queue.append((child, depth + 1)) }
                }
            }
        }
        return fallbackField
    }

    /// Router input: one numbered line per window.
    static func describe(_ windows: [WindowInfo]) -> String {
        windows.map { window in
            let title = window.title.isEmpty ? "(untitled)" : window.title
            return "\(window.id): \(window.app) — \(title)"
        }.joined(separator: "\n")
    }
}
