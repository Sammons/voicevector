# Provider API contracts

Every provider is (baseURL, apiKey, sttModel, chatModel). API keys live in the
platform secret store (macOS Keychain / Windows DPAPI), never in config files.
All requests are plain HTTPS with generous timeouts (60 s request / 300 s
upload); no streaming anywhere.

## Kinds

| kind | STT | Chat | Models list | default baseURL |
|---|---|---|---|---|
| `elevenLabs` | ✓ | – | – | `https://api.elevenlabs.io` |
| `fireworks` | – (removed June 2026) | ✓ | ✓ | `https://api.fireworks.ai/inference/v1` |
| `cerebras` | – | ✓ | ✓ | `https://api.cerebras.ai/v1` |
| `vercelGateway` | ✓ (beta) | ✓ | ✓ | `https://ai-gateway.vercel.sh/v1` |
| `openAICompatible` | if server supports | ✓ | ✓ | user-set (`http://localhost:11434/v1`) |

`openAICompatible` covers: Ollama & LM Studio (chat only — no STT endpoint),
vLLM / Speaches / LocalAI / whisper.cpp server (STT), **Azure AI Foundry**
(`https://<resource>.openai.azure.com/openai/v1`, model = deployment name),
and **Foundry Local** (localhost, per-user install, on-device).

## ElevenLabs STT

`POST {base}/v1/speech-to-text`, header `xi-api-key: <key>`, multipart:
`model_id` (e.g. `scribe_v2`), `tag_audio_events=false`, repeated `keyterms`
fields (≤100 × ≤50 chars) from the vocabulary list, `file` (audio/wav).
Response: `{ "text": … }`. Errors: `{"detail": {...}}` (object or array).

## OpenAI-compatible

- STT: `POST {base}/audio/transcriptions`, `Authorization: Bearer <key>`
  (send `Bearer voicevector` when no key — some local servers require the
  header), multipart: `model`, `response_format=json`, `prompt` = vocabulary
  joined with ", " (whisper biasing), `file`. Response `{ "text": … }`.
- Chat: `POST {base}/chat/completions`, JSON
  `{model, messages:[{role:"system"|"user",content}], temperature: 0.2}`.
  Read `choices[0].message.content` only.
- Models: `GET {base}/models` → `{ "data": [ { "id": … } ] }`.

## Vercel AI Gateway

Chat + models: OpenAI-compatible on the `/v1` base above. STT is custom:
`POST https://ai-gateway.vercel.sh/v4/ai/transcription-model` (strip `/v1`
from base), headers `Authorization: Bearer`, `ai-model-id: <model>`,
`ai-gateway-protocol-version: 0.0.1`,
`ai-transcription-model-specification-version: 4` (both enforced though the
docs' cURL omits them; values mirror @ai-sdk/gateway),
JSON body `{ "audio": <base64 wav>, "mediaType": "audio/wav" }`.
Response `{ "text": …, "durationInSeconds": … }`. Batch STT models:
`openai/gpt-4o-mini-transcribe`, `openai/gpt-4o-transcribe`,
`fish-audio/transcribe-1` (streaming-only models won't work here).

## Connection test semantics

- elevenLabs / openAICompatible-without-models: POST a 0.3 s silent WAV to the
  real STT endpoint (scoped keys can't call anything else).
- fireworks / cerebras / vercelGateway / openAICompatible: `GET /models`.

## Single-pass mode

When the STT and cleanup provider ids match AND `sttModel == chatModel`,
the apps skip the two-stage pipeline and send one chat completion: the
cleanup system prompt plus the WAV as an OpenAI `input_audio` content part.
Falls back to two-pass on any error. No raw transcript is produced.

## Cleanup call

System prompt from `shared/prompts` (or the user's custom prompt) +
vocabulary suffix; user message = raw transcript wrapped in
`<transcript>…</transcript>` delimiters (data, not conversation);
temperature 0.2; strip wrapping code fences / echoed delimiters /
whitespace from the reply; empty reply ⇒ keep raw.

## Review (spoken revisions of a staged draft)

With a hotkey's **Review before pasting** on, the cleaned text is held in the
HUD and each spoken instruction is sent to the review model as one chat call:
system = `shared/prompts/review.txt` (+ vocabulary line), user =
`<draft>…</draft>\n<instruction>…</instruction>`. The reply replaces the draft
after the same sanitizing as cleanup (code fences and echoed delimiters
stripped; empty ⇒ unchanged). The review model defaults to the cleanup
provider and can be any chat-capable provider.

**Screenshot context** attaches one JPEG per display (≤1280 px wide) to the
cleanup call and every revision, with a one-line note appended to the system
prompt. The user content becomes an array of parts: the text, then for each
display a `text` caption followed by an `image_url` part (`detail: low`). The
display holding the frontmost window comes first and its caption reads
`Display 1 of N — ACTIVE: the dictated text will be inserted here; the target
window is outlined in red.` (a red outline is drawn around that window in the
image); the others are `Display i of N.` On Linux under Wayland the focused
window is not knowable, so with several displays the caption says the active
one is unknown. If the model rejects image input the call is retried
text-only.
