import Foundation
import os

/// Lightweight logging facade. Everything goes to the unified log; errors are
/// also mirrored to an in-memory ring buffer surfaced in Settings → General.
enum Log {
    private static let logger = Logger(subsystem: "io.sammons.voicevector", category: "app")
    private static let lock = NSLock()
    private static var ring: [String] = []
    private static let ringLimit = 200

    static func info(_ message: String) {
        logger.info("\(message, privacy: .public)")
    }

    static func error(_ message: String) {
        logger.error("\(message, privacy: .public)")
        lock.lock()
        defer { lock.unlock() }
        let stamp = ISO8601DateFormatter().string(from: Date())
        ring.append("\(stamp)  \(message)")
        if ring.count > ringLimit { ring.removeFirst(ring.count - ringLimit) }
    }

    static var recentErrors: [String] {
        lock.lock()
        defer { lock.unlock() }
        return ring
    }
}
