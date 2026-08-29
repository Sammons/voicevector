# VoiceVector for macOS — Architecture

A minimalist, native macOS dictation app. Press a hotkey anywhere, speak, and the
cleaned-up transcript is pasted into the frontmost app. Recordings and transcripts
are kept locally as plain files; folders can forward new dictations to a webhook
(same spirit as the VoiceVector iOS/watchOS app).

## Goals & non-goals

**Goals**
- Dictation + LLM cleanup, pasted into whatever app is focused (Terminal, VS Code,
  Chrome, anything).
- Cloud/self-hosted STT only — no bundled local models. Providers: ElevenLabs,
  Fireworks, and any OpenAI-compatible endpoint (Ollama, LM Studio, vLLM,
  faster-whisper servers…). Model listing where the API supports it.
- Hotkey with two modes usable simultaneously: **hold-to-talk** (press & hold,
  release to stop) and **double-tap to start / single tap to stop**.
- First-run wizard: permissions → provider config + endpoint test → hotkey.
- Local-first storage: WAV recordings + Markdown transcripts (raw + cleaned) in a
  visible app folder, organized into user folders; per-folder optional webhook.
- One main window (collapsible folders, paginated entries, copy raw/cleaned),
  one settings area, one wizard. Menu-bar status item with recording state.
- Zero third-party dependencies. SwiftPM only, Apple frameworks only. Buildable
  with Command Line Tools alone (no Xcode): `make app`.

**Non-goals (v1)**: paid tiers, diarization, local/offline models, streaming
transcription, iOS sync, localization.

## Build & packaging

- `Package.swift` — single executable target `VoiceVector`, Swift 5 language mode,
  macOS 14+ platform.
- `scripts/make_app.sh` — `swift build -c release`, assembles
  `build/VoiceVector.app` (Info.plist with `NSMicrophoneUsageDescription`,
  generated `.icns`), ad-hoc `codesign`. `make app`, `make run`, `make test`.
- `scripts/make_icon.swift` — draws the app icon with CoreGraphics at build time
  (no binary assets in the repo).
- Unit tests (`swift test`) cover the pure logic: multipart encoding, markdown
  store round-trip, config codables, hotkey state machine, cleanup prompt
  assembly.

Ad-hoc signing is enough for TCC (mic + accessibility) on your own machine; the
grants persist per signature, so `make app` re-signs with the same identifier.

## Runtime shape

AppKit lifecycle (`NSApplication` + `AppDelegate`) hosting SwiftUI views — full
control over the status item, the floating recording HUD (`NSPanel`,
non-activating, so focus never leaves the target app), and window management.

```
HotkeyEngine ──▶ DictationController (state machine) ──▶ PasteService
   CGEventTap        │ idle → recording → transcribing        │ pasteboard swap +
   (listen-only)     │      → cleaning → pasting → idle       │ synthesized ⌘V
                     ▼                                        ▼ fallbacks
                 Recorder (AVAudioEngine → 16 kHz mono WAV)
                     │
                     ▼
                 Library (folders / entries on disk) ──▶ WebhookSender
                     │
                     ▼
                 Providers: STT + Cleanup (URLSession, hand-rolled multipart)
```

### Modules (all in one target, one folder each)

| Module | Responsibility |
|---|---|
| `Core` | `AppConfig` (Codable JSON at `<library>/config.json`), Keychain wrapper for API keys (Security.framework), logging |
| `Audio` | `Recorder`: AVAudioEngine input tap → downsample → `WavWriter` (16-bit PCM mono 16 kHz); live level metering for the HUD |
| `Hotkey` | `HotkeyEngine`: CGEventTap (listen-only, keyboard + flagsChanged) so plain keys, combos, *and modifier-only* hotkeys (e.g. Right ⌥, Fn) work; pure-Swift chord/tap state machine (hold vs double-tap detection, testable) |
| `Providers` | `TranscriptionProvider` / `CleanupProvider` protocols; `ElevenLabsSTT`; `OpenAICompatClient` (used for Fireworks preset, self-hosted, custom endpoints — `/v1/models`, `/v1/audio/transcriptions`, `/v1/chat/completions`); `Multipart` encoder |
| `Cleanup` | Prompt builder: filler removal, punctuation, dictated formatting ("bullet point…", "new line", headings), vocabulary hints; modes **off / light / rich** |
| `Pipeline` | `DictationController`: owns the state machine, error surfacing (notification + entry saved anyway), concurrency (one dictation at a time) |
| `Paste` | `PasteService`: save pasteboard → write transcript (marked `org.nspasteboard.TransientType`) → synthesized ⌘V → restore old contents (changeCount-guarded); secure-input detection; per-app quirks; fallback: leave in clipboard + notify |
| `Store` | `Library`: folders = real subdirectories under `~/Documents/VoiceVector`; entry = `yyyyMMdd-HHmmss.wav` + `.md` (front-matter: date, duration, provider, models; body: cleaned + raw sections). Directory scan + lazy parse, newest-first pagination |
| `Webhook` | Per-folder URL; POST JSON (raw, cleaned, metadata) with optional audio multipart; retry ×2 |
| `UI` | Theme (one accent, generous whitespace, SF Symbols); `MainWindow` (folder sections → entries, copy buttons, search-free v1), `SettingsView` (Providers / Hotkey / Cleanup / Folders & Webhooks), `WizardView`, `RecordingHUD`, status item menu |

### Key decisions

- **Storage is files, not a database.** Markdown + WAV in visible folders; the app
  is just a viewer. Nothing to migrate, everything greppable, webhook payloads
  trivially reproducible.
- **API keys in Keychain**, everything else in a human-readable `config.json`.
- **One event-tap, one permission.** Accessibility trust covers both the
  listen-only hotkey tap and posting ⌘V. Microphone is the only other prompt.
- **STT and cleanup are independently pluggable.** e.g. ElevenLabs Scribe for STT
  + Fireworks small instruct model for cleanup, or one self-hosted box for both.
  Cleanup can be disabled entirely; raw is always kept.
- **Failure never loses audio.** The WAV is written to disk while recording; if
  transcription fails the entry is saved with an error marker and can be retried
  from the list.
