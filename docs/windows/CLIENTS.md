# Windows — provider clients

Implementation of `docs/providers.md` on Windows. Lives entirely in the
**portable** `VoiceVector.Core` library (plain `HttpClient`), so it compiles
and self-tests on any OS — the WinUI app just injects the DPAPI-stored key.

## Files

| File | Role |
|---|---|
| `src/VoiceVector.Core/ProviderClient.cs` | one class for every kind: `TranscribeAsync`, `ChatAsync`, `ListModelsAsync`, `TestAsync`; static shared `HttpClient` (300 s timeout) |
| `src/VoiceVector.Core/AppConfig.cs` | `ProviderKind` + capability extensions (`SupportsTranscription()` etc.) and `ProviderProfile.Preset` |
| `src/VoiceVector.Core/CleanupEngine.cs` | prompt assembly (embedded copies of `shared/prompts/`), vocabulary parsing, reply post-processing |
| `src/VoiceVector.Core/WebhookSender.cs` | webhook delivery, 3 attempts, 2 s·attempt backoff |
| `src/VoiceVector.App/Services/DictationController.cs` | pipeline call sites; key injection via `DpapiKeyStore.GetApiKey(profile.Id)` |

## Behavior notes (mirror of `docs/mac/CLIENTS.md`)

- Auth: `xi-api-key` for ElevenLabs; otherwise `Bearer`, with literal
  `voicevector` when no key is stored (local servers want the header present).
- Multipart via `MultipartFormDataContent` (framework, not hand-rolled —
  the .NET BCL provides it; the zero-dep rule is about *packages*).
- Vercel gateway STT: base64-WAV JSON to `/v4/ai/transcription-model` at the
  gateway root (`/v1` suffix stripped), model in the `ai-model-id` header.
- ElevenLabs: `tag_audio_events=false`, ≤100 `keyterms` (≤50 chars each);
  OpenAI-compatible: vocabulary as the whisper `prompt` field.
- `TestAsync` matches key scope: ElevenLabs → 0.3 s silent WAV
  (`WavWriter.SilentWav`) to the real STT endpoint; Fireworks/gateway →
  `GET /models`; custom → models first, silent-WAV fallback.
- Errors surface as `HttpRequestException("HTTP <code>: <300-char snippet>")`.
- Only `choices[0].message.content` is read from chat responses;
  `CleanupEngine.PostProcess` strips accidental code fences and falls back to
  raw on empty replies.

## Windows-relevant provider endpoints

- **Azure AI Foundry** (cloud): OpenAI-compatible kind, base URL
  `https://<resource>.openai.azure.com/openai/v1`, API key, model field =
  deployment name. Chat is GA on that route; audio transcription is v1-preview.
- **Foundry Local** (on-device, per-user winget install, no admin/no NPU
  required): OpenAI-compatible kind pointed at its localhost endpoint
  (`foundry service status` prints the port); discover model ids via
  `/v1/models`. Chat/cleanup is the primary use.
