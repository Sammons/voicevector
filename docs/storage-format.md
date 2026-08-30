# VoiceVector library storage format

Both apps implement exactly this. The library is just files; the apps are
viewers. A folder synced between machines is a shared library.

## Layout

```
<library root>/                    macOS default: ~/Documents/VoiceVector
  <Folder>/                        one directory per user folder ("Inbox" always exists)
    <id>.wav                       original recording
    <id>.md                        transcript
    <id>-screen-N.jpg              optional: screenshot context, one per display (N = 1…)
```

- `id` = local timestamp `yyyyMMdd-HHmmss` (POSIX locale), with `-2`, `-3`…
  appended on collision. Ids sort newest-first lexicographically (descending).
- Recordings are RIFF WAV, 16-bit PCM, mono, 16 000 Hz.

## Transcript markdown

```markdown
---
date: 2026-08-25T21:15:32Z          # ISO-8601 UTC (internet date-time)
duration: 12.4                      # seconds, one decimal
audio: 20260825-161530.wav          # sibling file name
stt: ElevenLabs/scribe_v2           # provider name/model
cleanup: Vercel AI Gateway/openai/gpt-4o-mini   # omitted if cleanup never configured
status: complete                    # "complete" | "error: <message>"
screenshots: 2                      # optional: number of <id>-screen-N.jpg files
activeScreenshot: 1                 # optional: 1-based N of the display the text went into (absent = unknown)
screenshotOutline: true             # optional: the target window is outlined in red in that image
---

<cleaned text — the primary body>

## Raw transcript

<raw STT text — omitted when identical to cleaned>
```

Parsing rules (must match in both apps):
- Front matter is the first `---` … `---` block; `key: value` lines; unknown
  keys ignored; a damaged/missing header falls back to parsing the id as the
  date and treating the whole body as cleaned text.
- Body before `\n## Raw transcript\n` is the cleaned text; after it, the raw
  text. No raw section ⇒ raw = cleaned.
- `cleanup` values may carry suffixes: `… (failed — raw used)` or
  `not run — no cleanup provider selected`; UIs surface these as states.
- Screenshot keys are written only when screenshots exist (`screenshots` first,
  then `activeScreenshot` only when known, then `screenshotOutline` only when
  true). The JPEGs (≤1280 px wide) are written when the hotkey fires — one set
  per entry, never replaced by later revisions — and deleted with the entry.
  A retry from the library reuses the saved set instead of capturing again.
