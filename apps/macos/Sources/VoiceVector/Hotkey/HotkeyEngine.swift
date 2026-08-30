import Foundation
import AppKit
import Carbon.HIToolbox

/// Global hotkey listener built on a CGEventTap so modifier-only keys (Right ⌥,
/// Fn, …) work as hotkeys. Regular-key hotkeys are swallowed so they don't
/// reach the focused app; modifier-only ones pass through harmlessly.
///
/// All callbacks fire on the main queue.
final class HotkeyEngine {
    /// (action, profileID) — the dictation profile whose hotkey initiated
    /// the gesture decides the cleanup policy downstream.
    var onAction: ((TapStateMachine.Action, UUID) -> Void)?
    /// Fires for any key press while capture mode is on (hotkey recorder UI).
    var onCaptureKey: ((HotkeySpec) -> Void)?

    private(set) var profiles: [DictationProfile]
    private var machine: TapStateMachine
    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var expiryTimer: DispatchSourceTimer?
    private var modifierIsDown: [UUID: Bool] = [:]
    /// Profile that initiated the in-flight gesture.
    private var activeProfileID: UUID?
    var captureMode = false

    /// True while a dictation gesture is in progress (used to swallow Esc).
    var recordingActive = false
    /// True while a draft is staged for review: ⏎ accepts, Esc discards
    /// (both swallowed so they never reach the app underneath).
    var reviewActive = false
    var onReviewAccept: (() -> Void)?
    var onReviewDiscard: (() -> Void)?

    init(profiles: [DictationProfile], startMode: TapStartMode) {
        self.profiles = HotkeyEngine.dedupe(profiles)
        self.machine = TapStateMachine(startMode: startMode)
    }

    func reconfigure(profiles: [DictationProfile], startMode: TapStartMode) {
        self.profiles = HotkeyEngine.dedupe(profiles)
        machine = TapStateMachine(startMode: startMode)
        modifierIsDown = [:]
        activeProfileID = nil
    }

    /// Later profiles with a duplicate hotkey are inert (first one wins).
    private static func dedupe(_ profiles: [DictationProfile]) -> [DictationProfile] {
        var seen: [HotkeySpec] = []
        return profiles.filter { profile in
            if seen.contains(profile.hotkey) { return false }
            seen.append(profile.hotkey)
            return true
        }
    }

    // MARK: Tap lifecycle

    var isRunning: Bool { tap != nil }

    @discardableResult
    func start() -> Bool {
        guard tap == nil else { return true }
        let mask: CGEventMask =
            (1 << CGEventType.keyDown.rawValue) |
            (1 << CGEventType.keyUp.rawValue) |
            (1 << CGEventType.flagsChanged.rawValue)

        let callback: CGEventTapCallBack = { _, type, event, refcon in
            let engine = Unmanaged<HotkeyEngine>.fromOpaque(refcon!).takeUnretainedValue()
            return engine.handle(type: type, event: event)
        }

        guard let tap = CGEvent.tapCreate(tap: .cgSessionEventTap,
                                          place: .headInsertEventTap,
                                          options: .defaultTap,
                                          eventsOfInterest: mask,
                                          callback: callback,
                                          userInfo: Unmanaged.passUnretained(self).toOpaque()) else {
            Log.error("Could not create event tap — is Accessibility permission granted?")
            return false
        }
        self.tap = tap
        let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        runLoopSource = source
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        Log.info("Hotkey tap started")
        return true
    }

    func stop() {
        if let tap { CGEvent.tapEnable(tap: tap, enable: false) }
        if let runLoopSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, .commonModes) }
        tap = nil
        runLoopSource = nil
    }

    // MARK: Event handling (tap thread = main runloop)

    private func handle(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        // macOS disables taps that are slow or when the screen locks; revive.
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            return Unmanaged.passUnretained(event)
        }

        let keyCode = UInt16(event.getIntegerValueField(.keyboardEventKeycode))

        if captureMode {
            return handleCapture(type: type, event: event, keyCode: keyCode)
        }

        // Esc cancels an in-progress dictation and is swallowed.
        if recordingActive, type == .keyDown, keyCode == UInt16(kVK_Escape) {
            emit(machine.cancel())
            return nil
        }

        // Staged review: ⏎ pastes, Esc discards; both swallowed (and their
        // key-ups), everything else passes through.
        if reviewActive, !recordingActive, !machine.isActive {
            if keyCode == UInt16(kVK_Return) || keyCode == UInt16(kVK_ANSI_KeypadEnter) {
                if type == .keyDown { DispatchQueue.main.async { self.onReviewAccept?() } }
                return nil
            }
            if keyCode == UInt16(kVK_Escape) {
                if type == .keyDown { DispatchQueue.main.async { self.onReviewDiscard?() } }
                return nil
            }
        }

        // Try each profile's hotkey; a gesture in progress only accepts
        // events from its initiating profile.
        for profile in profiles {
            guard profile.hotkey.isSet else { continue }
            if machine.isActive && activeProfileID != profile.id { continue }
            if let verdict = interpret(profile.hotkey, profileID: profile.id,
                                       type: type, keyCode: keyCode, event: event) {
                return verdict
            }
        }
        return Unmanaged.passUnretained(event)
    }

    /// Returns the tap verdict when the event belongs to this spec, else nil.
    private func interpret(_ candidate: HotkeySpec, profileID: UUID, type: CGEventType,
                           keyCode: UInt16, event: CGEvent) -> Unmanaged<CGEvent>?? {
        if candidate.isModifierOnly {
            guard type == .flagsChanged, keyCode == candidate.keyCode else { return nil }
            let flagDown = Self.modifierFlagActive(keyCode: keyCode, flags: event.flags)
            if flagDown != (modifierIsDown[profileID] ?? false) {
                modifierIsDown[profileID] = flagDown
                let now = ProcessInfo.processInfo.systemUptime
                if flagDown && !machine.isActive { activeProfileID = profileID }
                emit(flagDown ? machine.keyDown(at: now) : machine.keyUp(at: now))
            }
            return .some(Unmanaged.passUnretained(event))
        }

        guard (type == .keyDown || type == .keyUp), keyCode == candidate.keyCode else {
            return nil
        }
        let relevant: CGEventFlags = [.maskCommand, .maskAlternate, .maskControl,
                                      .maskShift, .maskSecondaryFn]
        let active = event.flags.intersection(relevant).rawValue
        guard machine.isActive || active == candidate.modifiers else { return nil }
        if type == .keyDown, event.getIntegerValueField(.keyboardEventAutorepeat) != 0 {
            return .some(nil) // swallow auto-repeat of the held hotkey
        }
        let now = ProcessInfo.processInfo.systemUptime
        if type == .keyDown && !machine.isActive { activeProfileID = profileID }
        emit(type == .keyDown ? machine.keyDown(at: now) : machine.keyUp(at: now))
        return .some(nil) // swallow the hotkey itself
    }

    private func handleCapture(type: CGEventType, event: CGEvent, keyCode: UInt16) -> Unmanaged<CGEvent>? {
        if type == .keyDown {
            if keyCode == UInt16(kVK_Escape) { return nil } // reserved for cancel
            let relevant: CGEventFlags = [.maskCommand, .maskAlternate, .maskControl, .maskShift, .maskSecondaryFn]
            let mods = event.flags.intersection(relevant).rawValue
            let captured = HotkeySpec(keyCode: keyCode, modifiers: mods, isModifierOnly: false)
            DispatchQueue.main.async { self.onCaptureKey?(captured) }
            return nil
        }
        if type == .flagsChanged, Self.isCapturableModifier(keyCode: keyCode),
           Self.modifierFlagActive(keyCode: keyCode, flags: event.flags) == false {
            // Modifier pressed and released alone → modifier-only hotkey.
            let captured = HotkeySpec(keyCode: keyCode, modifiers: 0, isModifierOnly: true)
            DispatchQueue.main.async { self.onCaptureKey?(captured) }
            return Unmanaged.passUnretained(event)
        }
        return Unmanaged.passUnretained(event)
    }

    private func emit(_ actions: [TapStateMachine.Action]) {
        scheduleExpiry()
        guard let profileID = activeProfileID ?? profiles.first?.id else { return }
        for action in actions {
            DispatchQueue.main.async { self.onAction?(action, profileID) }
        }
    }

    private func scheduleExpiry() {
        expiryTimer?.cancel()
        expiryTimer = nil
        guard let deadline = machine.pendingDeadline else { return }
        let delay = max(0, deadline - ProcessInfo.processInfo.systemUptime)
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + delay)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.emit(self.machine.expire(at: ProcessInfo.processInfo.systemUptime))
        }
        timer.resume()
        expiryTimer = timer
    }

    // MARK: Modifier helpers

    /// Which device-specific flag corresponds to a modifier key code.
    static func modifierFlagActive(keyCode: UInt16, flags: CGEventFlags) -> Bool {
        switch Int(keyCode) {
        case kVK_Command, kVK_RightCommand: return flags.contains(.maskCommand)
        case kVK_Option, kVK_RightOption: return flags.contains(.maskAlternate)
        case kVK_Control, kVK_RightControl: return flags.contains(.maskControl)
        case kVK_Shift, kVK_RightShift: return flags.contains(.maskShift)
        case kVK_Function: return flags.contains(.maskSecondaryFn)
        case kVK_CapsLock: return flags.contains(.maskAlphaShift)
        default: return false
        }
    }

    static func isCapturableModifier(keyCode: UInt16) -> Bool {
        [kVK_Command, kVK_RightCommand, kVK_Option, kVK_RightOption,
         kVK_Control, kVK_RightControl, kVK_Shift, kVK_RightShift, kVK_Function]
            .contains(Int(keyCode))
    }

    // MARK: Display names

    static func describe(_ spec: HotkeySpec) -> String {
        var parts: [String] = []
        let flags = CGEventFlags(rawValue: spec.modifiers)
        if flags.contains(.maskSecondaryFn), spec.keyCode != UInt16(kVK_Function) { parts.append("fn") }
        if flags.contains(.maskControl) { parts.append("⌃") }
        if flags.contains(.maskAlternate) { parts.append("⌥") }
        if flags.contains(.maskShift) { parts.append("⇧") }
        if flags.contains(.maskCommand) { parts.append("⌘") }
        parts.append(keyName(spec.keyCode))
        return parts.joined()
    }

    static func keyName(_ keyCode: UInt16) -> String {
        switch Int(keyCode) {
        case kVK_Command: return "Left ⌘"
        case kVK_RightCommand: return "Right ⌘"
        case kVK_Option: return "Left ⌥"
        case kVK_RightOption: return "Right ⌥"
        case kVK_Control: return "Left ⌃"
        case kVK_RightControl: return "Right ⌃"
        case kVK_Shift: return "Left ⇧"
        case kVK_RightShift: return "Right ⇧"
        case kVK_Function: return "fn"
        case kVK_CapsLock: return "⇪"
        case kVK_Space: return "Space"
        case kVK_Return: return "↩"
        case kVK_Tab: return "⇥"
        case kVK_Delete: return "⌫"
        case kVK_Escape: return "⎋"
        // NB: F-key virtual codes are not contiguous or ordered (kVK_F1 is
        // 0x7A, kVK_F20 is 0x5A) — never build a range from them.
        default:
            if let name = fKeyNames[Int(keyCode)] { return name }
            return characterName(keyCode) ?? "key \(keyCode)"
        }
    }

    private static let fKeyNames: [Int: String] = [
        kVK_F1: "F1", kVK_F2: "F2", kVK_F3: "F3", kVK_F4: "F4", kVK_F5: "F5",
        kVK_F6: "F6", kVK_F7: "F7", kVK_F8: "F8", kVK_F9: "F9", kVK_F10: "F10",
        kVK_F11: "F11", kVK_F12: "F12", kVK_F13: "F13", kVK_F14: "F14", kVK_F15: "F15",
        kVK_F16: "F16", kVK_F17: "F17", kVK_F18: "F18", kVK_F19: "F19", kVK_F20: "F20",
    ]

    /// Resolves a printable key name from the current keyboard layout.
    private static func characterName(_ keyCode: UInt16) -> String? {
        guard let source = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let layoutData = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) else {
            return nil
        }
        let data = Unmanaged<CFData>.fromOpaque(layoutData).takeUnretainedValue() as Data
        return data.withUnsafeBytes { (buffer: UnsafeRawBufferPointer) -> String? in
            guard let layout = buffer.baseAddress?.assumingMemoryBound(to: UCKeyboardLayout.self) else {
                return nil
            }
            var deadKeyState: UInt32 = 0
            var chars = [UniChar](repeating: 0, count: 4)
            var length = 0
            let status = UCKeyTranslate(layout, keyCode, UInt16(kUCKeyActionDisplay), 0,
                                        UInt32(LMGetKbdType()), UInt32(kUCKeyTranslateNoDeadKeysBit),
                                        &deadKeyState, chars.count, &length, &chars)
            guard status == noErr, length > 0 else { return nil }
            return String(utf16CodeUnits: chars, count: length).uppercased()
        }
    }
}
