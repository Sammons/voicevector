import Foundation
import CryptoKit
import Security

/// Building blocks for multi-machine peering (docs/multi-machine.md):
/// the machine identity (P-256 key in the keychain + hand-built self-signed
/// certificate), frame codec, and the pairing code. Pure logic lives here so
/// the self-test can cover it; sockets live in PeerService.
enum PeerCrypto {

    // MARK: Frames (4-byte big-endian length + UTF-8 JSON)

    static let maxFrame = 32 * 1024 * 1024

    static func frame(_ object: [String: Any]) -> Data? {
        guard let body = try? JSONSerialization.data(withJSONObject: object) else { return nil }
        var length = UInt32(body.count).bigEndian
        var data = Data(bytes: &length, count: 4)
        data.append(body)
        return data
    }

    enum FrameResult {
        case incomplete
        case invalid                    // oversized length or non-object JSON: close
        case frame([String: Any], consumed: Int)
    }

    /// Parses one complete frame from the front of `buffer`. Distinguishes a
    /// still-incomplete frame from an invalid one so the reader never buffers
    /// forever on a bogus length or wedges on malformed JSON.
    static func parseFrame(_ buffer: Data) -> FrameResult {
        guard buffer.count >= 4 else { return .incomplete }
        let length = buffer.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
        guard length >= 0, length <= maxFrame else { return .invalid }
        guard buffer.count >= 4 + length else { return .incomplete }
        let body = buffer.subdata(in: 4..<(4 + length))
        guard let object = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] else { return .invalid }
        return .frame(object, consumed: 4 + length)
    }

    // MARK: Pairing code (numeric comparison; see docs/multi-machine.md)

    /// 6-digit code over both certificate digests and both revealed nonces.
    static func pairingCode(fpClient: Data, fpServer: Data, nonceClient: Data, nonceServer: Data) -> String {
        var input = Data()
        input.append(fpClient); input.append(fpServer)
        input.append(nonceClient); input.append(nonceServer)
        let digest = Data(SHA256.hash(data: input))
        let value = digest.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        return String(format: "%06d", value % 1_000_000)
    }

    static func commitment(for nonce: Data) -> String {
        Data(SHA256.hash(data: nonce)).hexString
    }

    static func fingerprint(of certificateDER: Data) -> Data {
        Data(SHA256.hash(data: certificateDER))
    }

    // MARK: Identity (key in keychain, certificate DER beside the config)

    private static let keyTag = "io.sammons.voicevector.peer".data(using: .utf8)!

    struct Identity {
        let key: SecKey
        let certificate: SecCertificate
        let certificateDER: Data
        var fingerprint: Data { PeerCrypto.fingerprint(of: certificateDER) }
        var fingerprintHex: String { fingerprint.hexString }
    }

    /// Loads (or creates, once) the machine identity. `directory` holds the
    /// public certificate; the private key never leaves the keychain.
    static func loadIdentity(machineName: String, directory: URL) -> Identity? {
        let certURL = directory.appendingPathComponent("peer-cert.der")
        if let key = existingKey(),
           let der = try? Data(contentsOf: certURL),
           let cert = SecCertificateCreateWithData(nil, der as CFData),
           publicKeyMatches(cert: cert, key: key) {
            return Identity(key: key, certificate: cert, certificateDER: der)
        }
        // First run (or the cert/key went missing): start fresh.
        deleteKey()
        guard let key = createKey() else { Log.error("Peer identity: key creation failed"); return nil }
        guard let der = makeSelfSignedCertificate(key: key, commonName: "VoiceVector-\(machineName)"),
              let cert = SecCertificateCreateWithData(nil, der as CFData) else {
            Log.error("Peer identity: certificate creation failed")
            return nil
        }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try? der.write(to: certURL, options: .atomic)
        return Identity(key: key, certificate: cert, certificateDER: der)
    }

    /// SecIdentity for TLS: requires the certificate to be in the keychain.
    static func secIdentity(for identity: Identity) -> SecIdentity? {
        let add: [String: Any] = [kSecClass as String: kSecClassCertificate,
                                  kSecValueRef as String: identity.certificate]
        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess || status == errSecDuplicateItem else {
            Log.error("Peer identity: certificate keychain add failed (\(status))")
            return nil
        }
        var secIdentity: SecIdentity?
        let idStatus = SecIdentityCreateWithCertificate(nil, identity.certificate, &secIdentity)
        if idStatus != errSecSuccess { Log.error("Peer identity: SecIdentity failed (\(idStatus))") }
        return secIdentity
    }

    private static func existingKey() -> SecKey? {
        let query: [String: Any] = [kSecClass as String: kSecClassKey,
                                    kSecAttrApplicationTag as String: keyTag,
                                    kSecAttrKeyType as String: kSecAttrKeyTypeECSECPrimeRandom,
                                    kSecReturnRef as String: true]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess else { return nil }
        return (item as! SecKey)
    }

    private static func deleteKey() {
        let query: [String: Any] = [kSecClass as String: kSecClassKey,
                                    kSecAttrApplicationTag as String: keyTag]
        SecItemDelete(query as CFDictionary)
    }

    private static func createKey() -> SecKey? {
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrKeySizeInBits as String: 256,
            kSecPrivateKeyAttrs as String: [kSecAttrIsPermanent as String: true,
                                            kSecAttrApplicationTag as String: keyTag],
        ]
        return SecKeyCreateRandomKey(attributes as CFDictionary, nil)
    }

    private static func publicKeyMatches(cert: SecCertificate, key: SecKey) -> Bool {
        guard let certKey = SecCertificateCopyKey(cert),
              let publicKey = SecKeyCopyPublicKey(key),
              let a = SecKeyCopyExternalRepresentation(certKey, nil) as Data?,
              let b = SecKeyCopyExternalRepresentation(publicKey, nil) as Data? else { return false }
        return a == b
    }

    // MARK: Minimal X.509 (self-signed, P-256, ECDSA-SHA256)

    /// Apple ships no API for creating certificates, so build the DER by
    /// hand. Structure per RFC 5280; only what a self-signed peer cert needs.
    static func makeSelfSignedCertificate(key: SecKey, commonName: String, now: Date = Date()) -> Data? {
        guard let publicKey = SecKeyCopyPublicKey(key),
              let point = SecKeyCopyExternalRepresentation(publicKey, nil) as Data?, point.count == 65 else {
            return nil
        }
        let oidECDSAWithSHA256 = Data([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02])
        let oidECPublicKey = Data([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01])
        let oidPrime256v1 = Data([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07])
        let oidCommonName = Data([0x55, 0x04, 0x03])

        let sigAlgorithm = der(0x30, der(0x06, oidECDSAWithSHA256))
        let name = der(0x30, der(0x31, der(0x30,
            der(0x06, oidCommonName) + der(0x0C, Data(commonName.utf8)))))

        let formatter = DateFormatter()
        formatter.dateFormat = "yyMMddHHmmss'Z'"
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.locale = Locale(identifier: "en_US_POSIX")
        let notBefore = der(0x17, Data(formatter.string(from: now).utf8))          // UTCTime
        let notAfter = der(0x18, Data("21260101000000Z".utf8))                     // GeneralizedTime
        let validity = der(0x30, notBefore + notAfter)

        var serialBytes = [UInt8](repeating: 0, count: 8)
        _ = SecRandomCopyBytes(kSecRandomDefault, 8, &serialBytes)
        serialBytes[0] = (serialBytes[0] & 0x7F) | 0x01   // positive, non-zero
        let serial = der(0x02, Data(serialBytes))

        let spki = der(0x30,
            der(0x30, der(0x06, oidECPublicKey) + der(0x06, oidPrime256v1))
            + der(0x03, Data([0x00]) + point))

        let version = der(0xA0, der(0x02, Data([0x02])))   // [0] EXPLICIT v3
        let tbs = der(0x30, version + serial + sigAlgorithm + name + validity + name + spki)

        guard let signature = SecKeyCreateSignature(key, .ecdsaSignatureMessageX962SHA256,
                                                    tbs as CFData, nil) as Data? else { return nil }
        return der(0x30, tbs + sigAlgorithm + der(0x03, Data([0x00]) + signature))
    }

    private static func der(_ tag: UInt8, _ content: Data) -> Data {
        var out = Data([tag])
        let count = content.count
        if count < 0x80 {
            out.append(UInt8(count))
        } else {
            var lengthBytes: [UInt8] = []
            var remaining = count
            while remaining > 0 { lengthBytes.insert(UInt8(remaining & 0xFF), at: 0); remaining >>= 8 }
            out.append(UInt8(0x80 | lengthBytes.count))
            out.append(contentsOf: lengthBytes)
        }
        out.append(content)
        return out
    }
}

extension Data {
    var hexString: String { map { String(format: "%02x", $0) }.joined() }

    init?(hexString: String) {
        let chars = Array(hexString)
        guard chars.count % 2 == 0 else { return nil }
        var bytes: [UInt8] = []
        for i in stride(from: 0, to: chars.count, by: 2) {
            guard let byte = UInt8(String(chars[i...i+1]), radix: 16) else { return nil }
            bytes.append(byte)
        }
        self.init(bytes)
    }
}
