import Foundation
import Network
import Security

/// One machine's routing context: its window list and screenshots.
struct MachineContext {
    let machine: String
    let isLocal: Bool
    var fingerprint: String = ""       // the pinned peer fingerprint (empty when local)
    let windows: [WindowInfo]          // empty for peers that can't enumerate
    let windowLines: String            // pre-rendered "id: App — Title" lines
    let screens: [ScreenshotAttachment]
}

/// TLS listener + client for multi-machine peering (docs/multi-machine.md).
/// All callbacks fire on the main queue.
final class PeerService {
    static let shared = PeerService()

    /// Wired by the app: current config, persisting a discovered peer, and
    /// handling an inbound delivery (activate window + paste + save).
    /// Push config from the main actor; internal reads use the queue-confined
    /// snapshot so handlers never touch @MainActor AppState across threads.
    private var mmConfig = MultiMachineConfig()
    func updateConfig(_ mm: MultiMachineConfig) { queue.async { self.mmConfig = mm; self.applyLocked() } }
    var addPeer: ((PeerRef) -> Void)?
    var onDeliver: ((String, UInt32, Bool, @escaping (Bool, String) -> Void) -> Void)?
    /// Inbound pairing UI: (peer name, code, answer) — call answer(true/false).
    var onIncomingPair: ((String, String, @escaping (Bool) -> Void) -> Void)?

    private let queue = DispatchQueue(label: "io.sammons.voicevector.peer")
    private var listener: NWListener?
    private var identity: PeerCrypto.Identity?
    private var secIdentity: SecIdentity?
    private var pairingBusy = false

    /// Queue-confined; only read on `queue`.
    private var config: MultiMachineConfig { mmConfig }
    /// Valid TCP port, clamped (the Settings field is unconstrained).
    private func clampedPort(_ port: Int) -> UInt16 { UInt16(clamping: max(1, min(65535, port))) }

    var fingerprintHex: String { identity?.fingerprintHex ?? "" }

    // MARK: Lifecycle

    /// Starts/stops the listener to match the config. Runs on `queue`.
    func applyConfig() { queue.async { [self] in applyLocked() } }

    private func ensureIdentityLocked() {
        guard identity == nil else { return }
        let dir = ConfigStore.defaultURL.deletingLastPathComponent()
        identity = PeerCrypto.loadIdentity(machineName: mmConfig.resolvedMachineName, directory: dir)
        if let identity { secIdentity = PeerCrypto.secIdentity(for: identity) }
    }

    private func applyLocked() {
        guard mmConfig.enabled else { stopLocked(); return }
        ensureIdentityLocked()
        guard identity != nil, secIdentity != nil else {
            Log.error("Multi-machine: no identity — listener not started")
            return
        }
        let port = clampedPort(mmConfig.port)
        if let listener, listener.port?.rawValue == port { return }
        stopLocked()
        startListenerLocked(port: port)
    }

    private func stopLocked() {
        listener?.cancel()
        listener = nil
    }

    private func startListenerLocked(port: UInt16) {
        guard let parameters = tlsParameters(expectFingerprint: nil),
              let nwPort = NWEndpoint.Port(rawValue: port) else { return }
        do {
            let listener = try NWListener(using: parameters, on: nwPort)
            listener.newConnectionHandler = { [weak self] connection in
                self?.serve(connection)
            }
            listener.stateUpdateHandler = { state in
                if case .failed(let error) = state { Log.error("Peer listener failed: \(error)") }
            }
            listener.start(queue: queue)
            self.listener = listener
        } catch {
            Log.error("Peer listener: \(error.localizedDescription)")
        }
    }

    /// TLS both ways with our identity; fingerprints are checked at the app
    /// layer (pairing) or against `expectFingerprint` (peer sessions).
    private func tlsParameters(expectFingerprint: String?) -> NWParameters? {
        guard let secIdentity, let sec = sec_identity_create(secIdentity) else { return nil }
        let tls = NWProtocolTLS.Options()
        let options = tls.securityProtocolOptions
        sec_protocol_options_set_local_identity(options, sec)
        sec_protocol_options_set_challenge_block(options, { _, complete in complete(sec) }, queue)
        sec_protocol_options_set_peer_authentication_required(options, true)
        sec_protocol_options_set_verify_block(options, { _, trust, complete in
            // Self-signed certs never chain; accept here, pin at the app layer.
            _ = trust
            complete(true)
        }, queue)
        let parameters = NWParameters(tls: tls)
        parameters.allowLocalEndpointReuse = true
        return parameters
    }

    /// The peer's certificate fingerprint, once a connection is ready.
    private func peerFingerprint(of connection: NWConnection) -> String? {
        guard let metadata = connection.metadata(definition: NWProtocolTLS.definition)
                as? NWProtocolTLS.Metadata else { return nil }
        var fingerprint: String?
        sec_protocol_metadata_access_peer_certificate_chain(metadata.securityProtocolMetadata) { certificate in
            if fingerprint == nil {
                let der = sec_certificate_copy_ref(certificate).takeRetainedValue()
                if let data = SecCertificateCopyData(der) as Data? {
                    fingerprint = PeerCrypto.fingerprint(of: data).hexString
                }
            }
        }
        return fingerprint
    }

    // MARK: Frame plumbing

    private final class FrameReader {
        var buffer = Data()
    }

    private func readFrame(_ connection: NWConnection, _ reader: FrameReader,
                           handler: @escaping ([String: Any]?) -> Void) {
        switch PeerCrypto.parseFrame(reader.buffer) {
        case .frame(let object, let consumed):
            reader.buffer.removeSubrange(0..<consumed)
            handler(object)
            return
        case .invalid:
            connection.cancel(); handler(nil); return
        case .incomplete:
            break
        }
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, done, error in
            guard let self else { return }
            if let data { reader.buffer.append(data) }
            // Never let a peer balloon our memory: the biggest legit frame is
            // one screenshot context, well under maxFrame.
            if reader.buffer.count > PeerCrypto.maxFrame {
                connection.cancel(); handler(nil); return
            }
            switch PeerCrypto.parseFrame(reader.buffer) {
            case .frame(let object, let consumed):
                reader.buffer.removeSubrange(0..<consumed)
                handler(object)
            case .invalid:
                connection.cancel(); handler(nil)
            case .incomplete:
                if done || error != nil { handler(nil) }
                else { self.readFrame(connection, reader, handler: handler) }
            }
        }
    }

    /// Cancels a connection that has not finished its single request/response
    /// within `seconds` — no post-handshake hang can wedge the app.
    private func armTimeout(_ connection: NWConnection, seconds: Double = 20) {
        queue.asyncAfter(deadline: .now() + seconds) { [weak connection] in
            connection?.cancel()
        }
    }

    private func send(_ connection: NWConnection, _ object: [String: Any],
                      then: (() -> Void)? = nil) {
        guard let data = PeerCrypto.frame(object) else { return }
        connection.send(content: data, completion: .contentProcessed { _ in then?() })
    }

    // MARK: Server

    private func serve(_ connection: NWConnection) {
        let reader = FrameReader()
        armTimeout(connection, seconds: 120)   // long enough for a human to compare codes
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .failed, .cancelled:
                connection.cancel(); return
            case .ready:
                break
            default:
                return
            }
            self.readFrame(connection, reader) { hello in
                guard let hello, hello["t"] as? String == "hello",
                      let purpose = hello["purpose"] as? String else { connection.cancel(); return }
                let peerName = hello["name"] as? String ?? "?"
                switch purpose {
                case "pair": self.servePairing(connection, reader, peerName: peerName)
                case "peer": self.servePeer(connection, reader, peerName: peerName)
                default: connection.cancel()
                }
            }
        }
        connection.start(queue: queue)
    }

    private func servePairing(_ connection: NWConnection, _ reader: FrameReader, peerName: String) {
        guard !pairingBusy, let identity else {
            send(connection, ["t": "err", "err": "busy"]) { connection.cancel() }
            return
        }
        guard let clientFP = peerFingerprint(of: connection).flatMap({ Data(hexString: $0) }) else {
            connection.cancel(); return
        }
        pairingBusy = true
        var released = false
        let unbusy = { self.queue.async { if !released { released = true; self.pairingBusy = false } } }
        var nonce = Data(count: 32)
        _ = nonce.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, 32, $0.baseAddress!) }
        send(connection, ["t": "hello", "ver": 1, "name": config.resolvedMachineName])
        readFrame(connection, reader) { [self] commit in
            guard let commit, commit["t"] as? String == "commit",
                  let theirCommit = commit["h"] as? String else { unbusy(); connection.cancel(); return }
            send(connection, ["t": "commit", "h": PeerCrypto.commitment(for: nonce)])
            readFrame(connection, reader) { [self] reveal in
                guard let reveal, reveal["t"] as? String == "reveal",
                      let theirNonce = (reveal["n"] as? String).flatMap({ Data(hexString: $0) }),
                      PeerCrypto.commitment(for: theirNonce) == theirCommit else {
                    unbusy(); connection.cancel(); return
                }
                send(connection, ["t": "reveal", "n": nonce.hexString])
                let code = PeerCrypto.pairingCode(fpClient: clientFP, fpServer: identity.fingerprint,
                                                  nonceClient: theirNonce, nonceServer: nonce)
                var remoteConfirmed = false, localAnswer: Bool?
                let finish: () -> Void = { [self] in
                    if remoteConfirmed, localAnswer == true {
                        DispatchQueue.main.async {
                            self.addPeer?(PeerRef(name: peerName, fingerprint: clientFP.hexString, address: ""))
                        }
                        send(connection, ["t": "confirm"]) { unbusy(); connection.cancel() }
                    }
                }
                // The peer's confirm/deny arrives while we wait for the user.
                readFrame(connection, reader) { answer in
                    if answer?["t"] as? String == "confirm" { remoteConfirmed = true; finish() }
                    else { unbusy(); connection.cancel() }
                }
                DispatchQueue.main.async {
                    self.onIncomingPair?(peerName, code) { accepted in
                        self.queue.async {
                            localAnswer = accepted
                            if !accepted { self.send(connection, ["t": "deny"]) { unbusy(); connection.cancel() } }
                            else { finish() }
                        }
                    }
                }
            }
        }
    }

    private func servePeer(_ connection: NWConnection, _ reader: FrameReader, peerName: String) {
        guard config.enabled,
              let fingerprint = peerFingerprint(of: connection),
              let peer = config.peers.first(where: { $0.fingerprint == fingerprint }) else {
            send(connection, ["t": "err", "err": "untrusted"]) { connection.cancel() }
            return
        }
        send(connection, ["t": "hello", "ver": 1, "name": config.resolvedMachineName])
        readFrame(connection, reader) { [self] request in
            guard let request, let type = request["t"] as? String else { connection.cancel(); return }
            switch type {
            case "context":
                guard peer.allowScreens else {
                    send(connection, ["t": "err", "err": "screens not allowed"]) { connection.cancel() }
                    return
                }
                DispatchQueue.main.async {
                    let context = PeerService.localContext(machineName: self.config.resolvedMachineName)
                    let screens = context.screens.map { ["jpeg": $0.jpeg.base64EncodedString(),
                                                         "caption": $0.caption] }
                    let windows = context.windows.map { ["id": Int($0.id), "app": $0.app, "title": $0.title] }
                    self.queue.async {
                        self.send(connection, ["t": "context", "machine": context.machine,
                                               "screens": screens, "windows": windows]) { connection.cancel() }
                    }
                }
            case "deliver":
                guard peer.allowDeliver else {
                    send(connection, ["t": "err", "err": "deliver not allowed"]) { connection.cancel() }
                    return
                }
                let text = request["text"] as? String ?? ""
                let window = (request["window"] as? NSNumber)?.uint32Value ?? 0
                let submit = request["submit"] as? Bool ?? false
                guard !text.isEmpty else {
                    send(connection, ["t": "err", "err": "empty"]) { connection.cancel() }
                    return
                }
                DispatchQueue.main.async {
                    guard let onDeliver = self.onDeliver else {
                        self.queue.async { self.send(connection, ["t": "err", "err": "not ready"]) { connection.cancel() } }
                        return
                    }
                    onDeliver(text, window, submit) { ok, message in
                        self.queue.async {
                            self.send(connection, ok ? ["t": "ok"] : ["t": "err", "err": message]) { connection.cancel() }
                        }
                    }
                }
            default:
                connection.cancel()
            }
        }
    }

    // MARK: Client

    private func endpoint(for address: String, defaultPort: Int) -> NWEndpoint? {
        var host = address, port = defaultPort
        if let colon = address.lastIndex(of: ":"), address.filter({ $0 == ":" }).count == 1 {
            host = String(address[..<colon])
            port = Int(address[address.index(after: colon)...]) ?? defaultPort
        }
        guard !host.isEmpty, port > 0, port <= 65535,
              let nwPort = NWEndpoint.Port(rawValue: UInt16(clamping: port)) else { return nil }
        return .hostPort(host: NWEndpoint.Host(host), port: nwPort)
    }

    /// Connects, sends hello, hands over once the server's hello arrives.
    private func openSession(address: String, purpose: String,
                             completion: @escaping (NWConnection?, FrameReader, String, String) -> Void) {
        queue.async { [self] in
            ensureIdentityLocked()   // identity is needed even with the listener off
            guard let endpoint = endpoint(for: address, defaultPort: config.port),
                  let parameters = tlsParameters(expectFingerprint: nil) else {
                DispatchQueue.main.async { completion(nil, FrameReader(), "", "bad address") }
                return
            }
            let connection = NWConnection(to: endpoint, using: parameters)
            let reader = FrameReader()
            var finished = false
            let fail: (String) -> Void = { message in
                guard !finished else { return }
                finished = true
                connection.cancel()
                DispatchQueue.main.async { completion(nil, reader, "", message) }
            }
            connection.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                switch state {
                case .ready:
                    self.send(connection, ["t": "hello", "ver": 1,
                                           "name": self.config.resolvedMachineName, "purpose": purpose])
                    self.readFrame(connection, reader) { hello in
                        guard !finished else { return }
                        guard let hello, hello["t"] as? String == "hello" else {
                            fail((try? hello?["err"] as? String) ?? "handshake failed"); return
                        }
                        finished = true
                        let name = hello["name"] as? String ?? "?"
                        let fingerprint = self.peerFingerprint(of: connection) ?? ""
                        completion(connection, reader, name, fingerprint)
                    }
                case .failed(let error):
                    fail(error.localizedDescription)
                case .cancelled:
                    fail("connection closed")
                default: break
                }
            }
            connection.start(queue: queue)
            queue.asyncAfter(deadline: .now() + 8) { fail("timed out") }
        }
    }

    /// Outbound pairing. `onCode` shows the number; call its answer callback.
    func pair(address: String,
              onCode: @escaping (String, @escaping (Bool) -> Void) -> Void,
              completion: @escaping (Result<PeerRef, Error>) -> Void) {
        let fail: (String) -> Void = { message in
            DispatchQueue.main.async {
                completion(.failure(NSError(domain: "VoiceVector", code: 1,
                                            userInfo: [NSLocalizedDescriptionKey: message])))
            }
        }
        openSession(address: address, purpose: "pair") { [self] connection, reader, serverName, serverFPHex in
            guard let connection, let identity = identity,
                  let serverFP = Data(hexString: serverFPHex) else { fail("could not connect"); return }
            var nonce = Data(count: 32)
            _ = nonce.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, 32, $0.baseAddress!) }
            queue.async { [self] in
                send(connection, ["t": "commit", "h": PeerCrypto.commitment(for: nonce)])
                readFrame(connection, reader) { [self] commit in
                    guard let commit, commit["t"] as? String == "commit",
                          let theirCommit = commit["h"] as? String else { connection.cancel(); fail("pairing refused"); return }
                    send(connection, ["t": "reveal", "n": nonce.hexString])
                    readFrame(connection, reader) { [self] reveal in
                        guard let reveal, reveal["t"] as? String == "reveal",
                              let theirNonce = (reveal["n"] as? String).flatMap({ Data(hexString: $0) }),
                              PeerCrypto.commitment(for: theirNonce) == theirCommit else {
                            connection.cancel(); fail("pairing failed"); return
                        }
                        let code = PeerCrypto.pairingCode(fpClient: identity.fingerprint, fpServer: serverFP,
                                                          nonceClient: nonce, nonceServer: theirNonce)
                        DispatchQueue.main.async {
                            onCode(code) { accepted in
                                self.queue.async { [self] in
                                    guard accepted else {
                                        send(connection, ["t": "deny"]) { connection.cancel() }
                                        fail("cancelled")
                                        return
                                    }
                                    send(connection, ["t": "confirm"])
                                    readFrame(connection, reader) { answer in
                                        connection.cancel()
                                        guard answer?["t"] as? String == "confirm" else {
                                            fail("the other machine denied the pairing"); return
                                        }
                                        let peer = PeerRef(name: serverName, fingerprint: serverFPHex,
                                                           address: address)
                                        DispatchQueue.main.async { completion(.success(peer)) }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /// One peer's context, or nil on any failure (pin mismatch included).
    func fetchContext(peer: PeerRef, completion: @escaping (MachineContext?) -> Void) {
        guard !peer.address.isEmpty else { completion(nil); return }
        openSession(address: peer.address, purpose: "peer") { [self] connection, reader, _, fingerprint in
            guard let connection, fingerprint == peer.fingerprint else {
                connection?.cancel()
                DispatchQueue.main.async { completion(nil) }
                return
            }
            queue.async { [self] in
                send(connection, ["t": "context"])
                readFrame(connection, reader) { response in
                    connection.cancel()
                    guard let response, response["t"] as? String == "context" else {
                        DispatchQueue.main.async { completion(nil) }; return
                    }
                    let machine = response["machine"] as? String ?? peer.name
                    let screens = (response["screens"] as? [[String: Any]] ?? []).compactMap { s -> ScreenshotAttachment? in
                        guard let b64 = s["jpeg"] as? String, let jpeg = Data(base64Encoded: b64) else { return nil }
                        let caption = s["caption"] as? String ?? ""
                        return ScreenshotAttachment(jpeg: jpeg, caption: "Machine \"\(machine)\" — " + caption)
                    }
                    let windows = (response["windows"] as? [[String: Any]] ?? []).compactMap { w -> WindowInfo? in
                        guard let id = (w["id"] as? NSNumber)?.uint32Value else { return nil }
                        return WindowInfo(id: id, app: w["app"] as? String ?? "?",
                                          title: w["title"] as? String ?? "", pid: 0)
                    }
                    let context = MachineContext(machine: machine, isLocal: false, fingerprint: peer.fingerprint,
                                                 windows: windows,
                                                 windowLines: WindowInventory.describe(windows), screens: screens)
                    DispatchQueue.main.async { completion(context) }
                }
            }
        }
    }

    /// Sends routed text to a peer, which activates `window` and pastes.
    func deliver(text: String, window: UInt32, submit: Bool, peer: PeerRef,
                 completion: @escaping (String?) -> Void) {
        guard !peer.address.isEmpty else { completion("peer has no address"); return }
        openSession(address: peer.address, purpose: "peer") { [self] connection, reader, _, fingerprint in
            guard let connection, fingerprint == peer.fingerprint else {
                connection?.cancel()
                DispatchQueue.main.async { completion("could not reach \(peer.name)") }
                return
            }
            queue.async { [self] in
                send(connection, ["t": "deliver", "text": text, "window": Int(window), "submit": submit])
                readFrame(connection, reader) { response in
                    connection.cancel()
                    let ok = response?["t"] as? String == "ok"
                    let message = ok ? nil : (response?["err"] as? String ?? "delivery failed")
                    DispatchQueue.main.async { completion(message) }
                }
            }
        }
    }

    /// This machine's context, captioned for the router. `captureScreens`
    /// controls whether missing screenshots are grabbed now (peer context
    /// requests: yes; a router hotkey with screenshot context off: no).
    static func localContext(machineName: String, windows: [WindowInfo]? = nil,
                             screens: ScreenshotSet? = nil, captureScreens: Bool = true) -> MachineContext {
        let list = windows ?? WindowInventory.list()
        let set = screens ?? (captureScreens ? ScreenCapture.allScreens() : nil)
        let attachments = (set?.attachments ?? []).map {
            ScreenshotAttachment(jpeg: $0.jpeg, caption: "Machine \"\(machineName)\" — " + $0.caption)
        }
        return MachineContext(machine: machineName, isLocal: true, windows: list,
                              windowLines: WindowInventory.describe(list), screens: attachments)
    }
}
