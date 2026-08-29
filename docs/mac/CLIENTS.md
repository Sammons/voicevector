# macOS — provider clients

Implementation of `docs/providers.md` on macOS. All network code is URLSession
with a hand-rolled multipart encoder — no SDKs.

## Files

| File | Role |
|---|---|
| `Sources/VoiceVector/Providers/Providers.swift` | `ProviderClient` — one struct for every provider kind: `transcribe`, `chat`, `listModels`, `test` |
| `Sources/VoiceVector/Providers/HTTP.swift` | URLSession wrapper (`HTTP.send` throws `HTTPError.status` with a 300-char body snippet), URL joining, JSON helpers |
| `Sources/VoiceVector/Providers/Multipart.swift` | RFC 2388 multipart/form-data encoder |
| `Sources/VoiceVector/Cleanup/CleanupEngine.swift` | prompt assembly + chat call + reply post-processing |
| `Sources/VoiceVector/Core/AppConfig.swift` | `ProviderKind` capability matrix (`supportsTranscription` / `supportsChat` / `supportsModelListing`) and presets |

## Behavior notes

- `ProviderClient` resolves the API key from the Keychain at init
  (injectable for tests / the settings Test button, which uses the in-flight
  text field value rather than the stored one).
- Auth: `xi-api-key` for ElevenLabs; `Authorization: Bearer` otherwise, with
  the literal `voicevector` as the token when the key is empty (local servers
  such as Ollama require the header but ignore the value).
- Vercel gateway STT is the odd one out: JSON body with base64 WAV to
  `/v4/ai/transcription-model` at the gateway root (the client strips the
  `/v1` suffix from the profile base URL), model in the `ai-model-id` header.
- ElevenLabs STT sends `tag_audio_events=false` and up to 100 `keyterms`
  fields from the vocabulary; OpenAI-compatible STT sends the vocabulary as
  the whisper `prompt`.
- **Connection tests match key scope**: ElevenLabs posts a 0.3 s silent WAV
  (`WavWriter.silentWav`) to the real STT endpoint because STT-scoped keys
  401 on anything else; chat-capable kinds probe `GET /models`; custom
  endpoints try models first, then fall back to the silent-WAV STT probe.
- Cleanup replies are trimmed and unwrapped from accidental code fences;
  an empty reply falls back to the raw transcript
  (`CleanupEngine.cleanup`). Reasoning-model `reasoning_content` is ignored —
  only `choices[0].message.content` is read.
- Timeouts: 60 s request / 300 s resource (uploads), ephemeral session.
- Pipeline call sites live in
  `Sources/VoiceVector/Pipeline/DictationController.swift` — transcription
  failure saves an `error:` entry (retryable); cleanup failure appends
  `(failed — raw used)` to the entry's cleanup label and raises a
  notification; a missing cleanup provider is labeled `not run — …`.
