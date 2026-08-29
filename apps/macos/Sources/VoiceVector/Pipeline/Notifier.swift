import Foundation
import UserNotifications
import AppKit

/// User-facing notifications with a graceful fallback: if notification
/// permission is denied (or the app is unbundled), messages surface in the HUD
/// via `fallback` instead of disappearing.
enum Notifier {
    static var fallback: ((String, String) -> Void)?

    private static var requested = false

    static func requestPermissionIfNeeded() {
        guard !requested else { return }
        requested = true
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert]) { _, _ in }
    }

    static func show(title: String, body: String) {
        let center = UNUserNotificationCenter.current()
        center.getNotificationSettings { settings in
            guard settings.authorizationStatus == .authorized else {
                DispatchQueue.main.async { fallback?(title, body) }
                return
            }
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = body
            let request = UNNotificationRequest(identifier: UUID().uuidString,
                                                content: content, trigger: nil)
            center.add(request) { error in
                if error != nil {
                    DispatchQueue.main.async { fallback?(title, body) }
                }
            }
        }
    }
}
