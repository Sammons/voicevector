# Webhook payload

Each folder may have `{url, includeAudio, enabled}`. On a finished dictation
(fire-and-forget, 3 attempts with 2 s·attempt backoff):

Without audio — `POST <url>`, `Content-Type: application/json`:

```json
{
  "app": "voicevector-macos",          // or "voicevector-windows"
  "id": "20260825-161530",
  "folder": "Inbox",
  "date": "2026-08-25T21:15:32Z",
  "duration": 12.4,
  "raw": "um hello world",
  "cleaned": "Hello world.",
  "stt": "ElevenLabs/scribe_v2",
  "cleanup": "Vercel AI Gateway/openai/gpt-4o-mini"
}
```

With audio — multipart/form-data: field `payload` = the JSON above as a
string, plus file field `audio` (`<id>.wav`, `audio/wav`).
