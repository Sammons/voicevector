# VoiceVector — cross-platform architecture

Two native apps, one product. They share **contracts** (file formats, API
shapes, prompts, gesture semantics) rather than code, so each app is fully
idiomatic for its platform with zero third-party runtime dependencies.

```
        hotkey gesture                    per-platform         portable/pure
  ┌───────────────────────┐   ┌──────────────────────┐   ┌──────────────────────┐
  │ macOS: CGEventTap     │   │ Recorder → 16 kHz    │   │ TapStateMachine      │
  │ Win:   WH_KEYBOARD_LL │──▶│ mono WAV on disk     │──▶│ (identical Swift/C#) │
  └───────────────────────┘   └──────────┬───────────┘   └──────────────────────┘
                                         ▼
                              ┌──────────────────────┐   providers (HTTP only)
                              │ DictationController  │──▶ STT: ElevenLabs,
                              │ record → transcribe  │    Vercel gateway, any
                              │ → clean → paste →    │    OpenAI-compatible
                              │ save → webhook       │──▶ Cleanup: chat
                              └──────────┬───────────┘    completions (gateway,
                                         ▼                Fireworks, Azure
                              ┌──────────────────────┐    Foundry, Foundry Local)
                              │ Library: <Folder>/   │
                              │ <stamp>.wav + .md    │──▶ per-folder webhook
                              └──────────────────────┘
```

## What is shared (normative, in `docs/` + `shared/`)

| Contract | Doc | Enforcement |
|---|---|---|
| Library layout & Markdown format | `docs/storage-format.md` | C# self-test asserts byte-identical rendering vs macOS output |
| Provider API shapes & test semantics | `docs/providers.md` | both clients implement the same matrix |
| Webhook payload | `docs/webhook-payload.md` | C# self-test checks shape |
| config.json schema & tolerant decoding | `docs/config-schema.md` | round-trip + partial-decode self-tests in both apps |
| Cleanup prompts | `shared/prompts/*.txt` | both self-tests diff embedded copies against these files |
| Hotkey gesture semantics | `docs/config-schema.md` §Hotkey | mirrored TapStateMachine self-tests |

A library folder synced between machines is a shared library — both apps read
and write the same files.

## Per-platform stacks

| Concern | macOS (`apps/macos`) | Windows (`apps/windows`) |
|---|---|---|
| Language/UI | Swift, AppKit shell + SwiftUI views | C# .NET 9, WinUI 3 built in code (no XAML compiler) |
| Packaging | SwiftPM + `scripts/make_app.sh` → signed .app | unpackaged self-contained publish folder, `asInvoker` |
| Hotkey | CGEventTap (`HotkeyEngine`) | `SetWindowsHookEx` low-level hook (`KeyboardHook`) |
| Gesture logic | `Hotkey/TapStateMachine.swift` | `VoiceVector.Core/TapStateMachine.cs` |
| Audio | AVAudioEngine + AVAudioConverter | AudioGraph frame output node |
| Paste | pasteboard snapshot → ⌘V CGEvent (AppleScript fallback on macOS 26 ad-hoc) | clipboard snapshot → SendInput Ctrl+V, Win+V-history-excluded writes |
| Secrets | Keychain (`docs/mac/SECRETS.md`) | DPAPI files (`docs/windows/SECRETS.md`) |
| Chimes | AVAudioEngine synthesized buffers | Core-generated WAV + MediaPlayer |
| Background presence | status item, window closable | tray icon, close-to-tray |
| Portable logic | in-app (single target) + `--self-test` | `VoiceVector.Core` library + `VoiceVector.SelfTest` (runs on Linux/CI) |

## Design decisions (both apps)

- **Files first.** No database; Markdown + WAV are the source of truth, the
  apps are viewers. Everything greppable, nothing to migrate.
- **One dictation at a time.** The controller is a small state machine
  (idle → recording → transcribing → cleaning → pasting); no queue.
- **Recording starts on the first key-down** so speech is never clipped; a
  stray tap's recording is discarded after the 400 ms tap window.
- **Failure never loses audio**: the WAV is written during recording; errors
  save an entry with `status: error…` and a Retry affordance.
- **Loud fallbacks**: cleanup failure → raw transcript + visible notice;
  paste impossible → transcript left on the clipboard + notice.
- **STT and cleanup are independently pluggable** per provider profile;
  Fireworks is chat-only (audio retired June 2026), ElevenLabs STT-only.

Deep dives: `apps/macos/ARCHITECTURE.md`, `docs/mac/*`, `docs/windows/*`.
