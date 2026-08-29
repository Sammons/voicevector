import Foundation

/// Forwards a finished dictation to the folder's webhook. JSON body by default;
/// multipart with the WAV attached when `includeAudio` is on. Two retries with
/// backoff; failures are logged, never block the pipeline.
enum WebhookSender {
    static func send(entry: Entry, audioURL: URL, config: WebhookConfig) async {
        guard config.enabled, let url = URL(string: config.url), !config.url.isEmpty else { return }

        var request: URLRequest
        let metadata: [String: Any] = [
            "app": "voicevector-macos",
            "id": entry.id,
            "folder": entry.folder,
            "date": Library.dateFormatter.string(from: entry.date),
            "duration": entry.duration,
            "raw": entry.raw,
            "cleaned": entry.cleaned,
            "stt": entry.sttLabel,
            "cleanup": entry.cleanupLabel,
        ]

        do {
            if config.includeAudio, let audio = try? Data(contentsOf: audioURL) {
                var form = Multipart()
                let json = try JSONSerialization.data(withJSONObject: metadata)
                form.addField(name: "payload", value: String(data: json, encoding: .utf8) ?? "{}")
                form.addFile(name: "audio", filename: entry.audioFilename,
                             contentType: "audio/wav", data: audio)
                request = HTTP.request(url, method: "POST", headers: [:],
                                       body: form.encoded(), contentType: form.contentType)
            } else {
                let json = try JSONSerialization.data(withJSONObject: metadata)
                request = HTTP.request(url, method: "POST", headers: [:],
                                       body: json, contentType: "application/json")
            }
        } catch {
            Log.error("Webhook payload build failed: \(error.localizedDescription)")
            return
        }

        for attempt in 1...3 {
            do {
                _ = try await HTTP.send(request)
                Log.info("Webhook delivered for \(entry.id)")
                return
            } catch {
                Log.error("Webhook attempt \(attempt) for \(entry.id) failed: \(error.localizedDescription)")
                if attempt < 3 {
                    try? await Task.sleep(nanoseconds: UInt64(attempt) * 2_000_000_000)
                }
            }
        }
    }
}
