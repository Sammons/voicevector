import Foundation

/// Pure logic for interpreting hotkey presses. Recording starts on the very
/// first key-down (so no speech is clipped); a stray single tap is discarded
/// after the tap window expires.
///
/// Semantics (both always active):
/// - Hold-to-talk: press, speak, release (≥ holdThreshold) → commit.
/// - Toggle: double-tap (or single tap, per config) starts; next tap commits.
struct TapStateMachine {
    enum Action: Equatable {
        case startRecording
        case commit
        case discard
    }

    enum Phase: Equatable {
        case idle
        /// Key is down; `tap` is 1 for the first press, 2 for the second.
        case pressed(since: TimeInterval, tap: Int)
        /// First short tap released; waiting to see if a second tap arrives.
        case awaitingSecondTap(deadline: TimeInterval)
        /// Toggle recording in progress, key up.
        case latched
        /// Commit issued on key-down; swallowing the matching key-up.
        case draining
    }

    var startMode: TapStartMode
    var holdThreshold: TimeInterval = 0.35
    var tapWindow: TimeInterval = 0.40

    private(set) var phase: Phase = .idle

    /// Deadline (absolute time) at which `expire(now:)` must be called, if any.
    var pendingDeadline: TimeInterval? {
        if case .awaitingSecondTap(let deadline) = phase { return deadline }
        return nil
    }

    var isActive: Bool { phase != .idle }

    mutating func keyDown(at now: TimeInterval) -> [Action] {
        switch phase {
        case .idle:
            phase = .pressed(since: now, tap: 1)
            return [.startRecording]
        case .awaitingSecondTap:
            phase = .pressed(since: now, tap: 2)
            return []
        case .latched:
            phase = .draining
            return [.commit]
        case .pressed, .draining:
            return [] // auto-repeat or duplicate events
        }
    }

    mutating func keyUp(at now: TimeInterval) -> [Action] {
        switch phase {
        case .pressed(let since, let tap):
            if now - since >= holdThreshold {
                // Held: push-to-talk (works on the first or second press).
                phase = .idle
                return [.commit]
            }
            if tap >= tapsRequired {
                phase = .latched
                return []
            }
            phase = .awaitingSecondTap(deadline: now + tapWindow)
            return []
        case .draining:
            phase = .idle
            return []
        case .idle, .awaitingSecondTap, .latched:
            return []
        }
    }

    /// Call when `pendingDeadline` passes without a second tap.
    mutating func expire(at now: TimeInterval) -> [Action] {
        if case .awaitingSecondTap(let deadline) = phase, now >= deadline {
            phase = .idle
            return [.discard]
        }
        return []
    }

    /// External cancel (Esc) or forced reset.
    mutating func cancel() -> [Action] {
        let wasActive = isActive
        phase = .idle
        return wasActive ? [.discard] : []
    }

    private var tapsRequired: Int { startMode == .doubleTap ? 2 : 1 }
}
