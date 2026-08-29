import Foundation
import Security

/// Minimal Keychain wrapper for provider API keys. Generic passwords under the
/// service "io.sammons.voicevector", account = provider profile id.
enum Keychain {
    private static let service = "io.sammons.voicevector"

    static func setAPIKey(_ key: String, for profileID: UUID) {
        let account = profileID.uuidString
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        if key.isEmpty {
            SecItemDelete(query as CFDictionary)
            return
        }
        let data = Data(key.utf8)
        // ThisDeviceOnly: keys never leave this Mac via keychain sync/backup.
        let attrs: [String: Any] = [
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
        ]
        let status = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if status == errSecItemNotFound {
            var add = query
            add.merge(attrs) { _, new in new }
            let addStatus = SecItemAdd(add as CFDictionary, nil)
            if addStatus != errSecSuccess {
                Log.error("Keychain add failed (\(addStatus))")
            }
        } else if status != errSecSuccess {
            Log.error("Keychain update failed (\(status))")
        }
    }

    static func apiKey(for profileID: UUID) -> String {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: profileID.uuidString,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data,
              let key = String(data: data, encoding: .utf8) else { return "" }
        return key
    }

    static func deleteAPIKey(for profileID: UUID) {
        setAPIKey("", for: profileID)
    }
}
