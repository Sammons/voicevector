import Foundation

struct TranscriptionResult {
    var text: String
    var provider: String
    var model: String
}

/// Builds the concrete API calls for a profile. One type handles all kinds —
/// the differences are small (paths, auth header, multipart fields).
struct ProviderClient {
    let profile: ProviderProfile
    /// Injectable for tests; defaults to the Keychain.
    var apiKey: String

    init(profile: ProviderProfile, apiKey: String? = nil) {
        self.profile = profile
        self.apiKey = apiKey ?? Keychain.apiKey(for: profile.id)
    }

    private var authHeaders: [String: String] {
        switch profile.kind {
        case .elevenLabs:
            return ["xi-api-key": apiKey]
        case .fireworks, .cerebras, .vercelGateway, .openAICompatible:
            // Local servers (Ollama etc.) want *a* bearer token even if unused.
            return ["Authorization": "Bearer \(apiKey.isEmpty ? "voicevector" : apiKey)"]
        }
    }

    // MARK: STT

    func transcribe(audioURL: URL, vocabulary: [String]) async throws -> TranscriptionResult {
        try await transcribe(audioData: Data(contentsOf: audioURL),
                             filename: audioURL.lastPathComponent, vocabulary: vocabulary)
    }

    func transcribe(audioData audio: Data, filename: String,
                    vocabulary: [String]) async throws -> TranscriptionResult {
        switch profile.kind {
        case .elevenLabs:
            return try await transcribeElevenLabs(audio: audio, filename: filename,
                                                  vocabulary: vocabulary)
        case .fireworks, .cerebras:
            throw HTTPError.badResponse("\(profile.kind.displayName) does not offer transcription — pick another STT provider.")
        case .vercelGateway:
            return try await transcribeVercel(audio: audio)
        case .openAICompatible:
            return try await transcribeOpenAI(audio: audio, filename: filename,
                                              vocabulary: vocabulary)
        }
    }

    /// Vercel AI Gateway uses a custom transcription endpoint (not the OpenAI
    /// multipart shape): model in the `ai-model-id` header, base64 audio in a
    /// JSON body, posted to /v4/ai/transcription-model at the gateway root.
    private func transcribeVercel(audio: Data) async throws -> TranscriptionResult {
        var root = profile.baseURL.hasSuffix("/") ? String(profile.baseURL.dropLast()) : profile.baseURL
        if root.hasSuffix("/v1") { root = String(root.dropLast(3)) }
        let url = try HTTP.url(base: root, path: "/v4/ai/transcription-model")
        let payload: [String: Any] = [
            "audio": audio.base64EncodedString(),
            "mediaType": "audio/wav",
        ]
        var headers = authHeaders
        headers["ai-model-id"] = profile.sttModel
        // Both enforced by the gateway though its docs' cURL omits them; the
        // protocol-version value was verified empirically (0.0.1 passes the
        // gate, integers are rejected).
        headers["ai-gateway-protocol-version"] = "0.0.1"
        headers["ai-transcription-model-specification-version"] = "4"
        let body = try JSONSerialization.data(withJSONObject: payload)
        let request = HTTP.request(url, method: "POST", headers: headers,
                                   body: body, contentType: "application/json")
        let json = try HTTP.json(try await HTTP.send(request))
        guard let text = json["text"] as? String else {
            throw HTTPError.badResponse("no `text` field in gateway transcription response")
        }
        return TranscriptionResult(text: text, provider: profile.name, model: profile.sttModel)
    }

    private func transcribeElevenLabs(audio: Data, filename: String,
                                      vocabulary: [String]) async throws -> TranscriptionResult {
        let url = try HTTP.url(base: profile.baseURL, path: "/v1/speech-to-text")
        var form = Multipart()
        form.addField(name: "model_id", value: profile.sttModel)
        form.addField(name: "tag_audio_events", value: "false")
        for term in vocabulary.prefix(100) {
            form.addField(name: "keyterms", value: String(term.prefix(50)))
        }
        form.addFile(name: "file", filename: filename, contentType: "audio/wav", data: audio)
        let request = HTTP.request(url, method: "POST", headers: authHeaders,
                                   body: form.encoded(), contentType: form.contentType)
        let json = try HTTP.json(try await HTTP.send(request))
        guard let text = json["text"] as? String else {
            throw HTTPError.badResponse("no `text` field in ElevenLabs response")
        }
        return TranscriptionResult(text: text, provider: profile.name, model: profile.sttModel)
    }

    private func transcribeOpenAI(audio: Data, filename: String,
                                  vocabulary: [String]) async throws -> TranscriptionResult {
        let url = try HTTP.url(base: profile.baseURL, path: "/audio/transcriptions")
        var form = Multipart()
        form.addField(name: "model", value: profile.sttModel)
        form.addField(name: "response_format", value: "json")
        if !vocabulary.isEmpty {
            // Whisper-style biasing: the prompt primes vocabulary.
            form.addField(name: "prompt", value: vocabulary.joined(separator: ", "))
        }
        form.addFile(name: "file", filename: filename, contentType: "audio/wav", data: audio)
        let request = HTTP.request(url, method: "POST", headers: authHeaders,
                                   body: form.encoded(), contentType: form.contentType)
        let json = try HTTP.json(try await HTTP.send(request))
        guard let text = json["text"] as? String else {
            throw HTTPError.badResponse("no `text` field in transcription response")
        }
        return TranscriptionResult(text: text, provider: profile.name, model: profile.sttModel)
    }

    // MARK: Chat (cleanup)

    /// `images` (one per display) are sent after the text as caption text
    /// parts each followed by an OpenAI image_url part; models without vision
    /// reject them, so callers retry without.
    func chat(system: String, user: String, images: [ScreenshotAttachment] = [],
              temperature: Double = 0.2) async throws -> String {
        guard profile.kind.supportsChat else {
            throw HTTPError.badResponse("\(profile.kind.displayName) has no chat endpoint")
        }
        let url = try HTTP.url(base: profile.baseURL, path: "/chat/completions")
        let userContent: Any
        if images.isEmpty {
            userContent = user
        } else {
            userContent = [["type": "text", "text": user]] + images.flatMap(imageParts)
        }
        let payload: [String: Any] = [
            "model": profile.chatModel,
            "messages": [
                ["role": "system", "content": system],
                ["role": "user", "content": userContent],
            ],
            "temperature": temperature,
        ]
        let body = try JSONSerialization.data(withJSONObject: payload)
        let request = HTTP.request(url, method: "POST", headers: authHeaders,
                                   body: body, contentType: "application/json")
        let json = try HTTP.json(try await HTTP.send(request))
        guard let choices = json["choices"] as? [[String: Any]],
              let message = choices.first?["message"] as? [String: Any],
              let content = message["content"] as? String else {
            throw HTTPError.badResponse("no choices[0].message.content in chat response")
        }
        return content
    }

    private func imageParts(_ shot: ScreenshotAttachment) -> [[String: Any]] {
        [["type": "text", "text": shot.caption],
         ["type": "image_url",
          "image_url": ["url": "data:image/jpeg;base64," + shot.jpeg.base64EncodedString(),
                        "detail": "low"]]]
    }

    /// One-shot audio → text via an audio-capable chat model (OpenAI
    /// `input_audio` content part). Used for single-pass dictation.
    func chatWithAudio(system: String, audio: Data, images: [ScreenshotAttachment] = [],
                       temperature: Double = 0.2) async throws -> String {
        guard profile.kind.supportsChat else {
            throw HTTPError.badResponse("\(profile.kind.displayName) has no chat endpoint")
        }
        let url = try HTTP.url(base: profile.baseURL, path: "/chat/completions")
        var parts: [[String: Any]] = [
            ["type": "input_audio",
             "input_audio": ["data": audio.base64EncodedString(), "format": "wav"]],
        ]
        parts += images.flatMap(imageParts)
        let payload: [String: Any] = [
            "model": profile.chatModel,
            "messages": [
                ["role": "system", "content": system],
                ["role": "user", "content": parts],
            ],
            "temperature": temperature,
        ]
        let body = try JSONSerialization.data(withJSONObject: payload)
        let request = HTTP.request(url, method: "POST", headers: authHeaders,
                                   body: body, contentType: "application/json")
        let json = try HTTP.json(try await HTTP.send(request))
        guard let choices = json["choices"] as? [[String: Any]],
              let message = choices.first?["message"] as? [String: Any],
              let content = message["content"] as? String else {
            throw HTTPError.badResponse("no choices[0].message.content in chat response")
        }
        return content
    }

    // MARK: Model listing & connectivity test

    /// GET /models (OpenAI-compatible). ElevenLabs has no equivalent.
    func listModels() async throws -> [String] {
        guard profile.kind.supportsModelListing else { return [] }
        let url = try HTTP.url(base: profile.baseURL, path: "/models")
        let request = HTTP.request(url, method: "GET", headers: authHeaders)
        let json = try HTTP.json(try await HTTP.send(request))
        guard let data = json["data"] as? [[String: Any]] else {
            throw HTTPError.badResponse("no `data` array in models response")
        }
        return data.compactMap { $0["id"] as? String }.sorted()
    }

    /// Auth/connectivity check used by the wizard's "Test" button. For STT
    /// providers this posts a tiny silent clip to the real transcription
    /// endpoint — scoped keys (e.g. an ElevenLabs speech-to-text-only key)
    /// aren't allowed to call anything else.
    func test() async throws -> String {
        switch profile.kind {
        case .elevenLabs:
            _ = try await transcribeSample()
            return "Connected — speech-to-text works (\(profile.sttModel))."
        case .fireworks, .cerebras, .vercelGateway:
            // Model listing works with any valid key; gateway STT is a beta on
            // gradual rollout, so a failed transcription would be misleading.
            let models = try await listModels()
            return models.isEmpty ? "Connected." : "Connected — \(models.count) models available."
        case .openAICompatible:
            // Model listing is the broadest cheap probe; fall back to a real
            // STT call for servers that don't implement /models.
            if let models = try? await listModels() {
                return models.isEmpty ? "Connected." : "Connected — \(models.count) models available."
            }
            _ = try await transcribeSample()
            return "Connected — speech-to-text works (\(profile.sttModel))."
        }
    }

    private func transcribeSample() async throws -> TranscriptionResult {
        let sample = WavWriter.silentWav(seconds: 0.3)
        let temp = FileManager.default.temporaryDirectory
            .appendingPathComponent("vv-test-\(UUID().uuidString).wav")
        try sample.write(to: temp)
        defer { try? FileManager.default.removeItem(at: temp) }
        return try await transcribe(audioURL: temp, vocabulary: [])
    }
}
