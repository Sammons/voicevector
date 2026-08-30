# CLAUDE.md — VoiceVector monorepo

Minimalist native dictation apps (macOS + Windows) sharing contracts, not code.
Hotkey → record WAV → cloud/self-hosted STT → LLM cleanup → paste into the
frontmost app → save as Markdown → optional per-folder webhook.

## Layout

| Path | What |
|---|---|
| `apps/macos/` | Swift + AppKit/SwiftUI, SwiftPM only (no Xcode). See its `ARCHITECTURE.md`. |
| `apps/windows/` | C# / .NET Framework 4.8 + WPF, single exe (no admin). |
| `apps/linux/` | C + GTK 4 / libadwaita, system libraries only. `make -C apps/linux` (app), `make -C apps/linux test` (core self-test, GLib + libcurl only). See `docs/linux/README.md`. |
| `docs/` | Cross-platform contracts (storage format, providers, webhook, config) + per-platform notes in `docs/mac/`, `docs/windows/`. |
| `shared/prompts/` | Canonical cleanup system prompts. Both apps embed copies; their self-tests assert equality with these files. |

## Build & test

- macOS (must run on the Mac; from the OrbStack Linux box prefix with `mac`):
  `make macos` (app bundle at `apps/macos/build/VoiceVector.app`),
  `make macos-test` (builds debug + runs `VoiceVector --self-test`).
- Windows app (Windows box only): `make windows`, or
  `dotnet publish src\VoiceVector.App -c Release -r win-x64 -p:Platform=x64 --self-contained`.
- Windows core logic (any OS): `make windows-test` runs
  `VoiceVector.SelfTest` — works on Linux
  (`DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1`, SDK in `~/.dotnet`).
- WinUI shell syntax-check from Linux/macOS:
  `dotnet build apps/windows/src/VoiceVector.App -p:Platform=x64 -p:WindowsAppSDKSelfContained=false -p:EnableCoreMrtTooling=false`.

## Hard rules

1. **Zero third-party runtime dependencies.** Apple frameworks / .NET + Windows
   App SDK only. Test harnesses are the built-in self-tests (`--self-test` /
   `VoiceVector.SelfTest`), not XCTest/xunit — Command Line Tools ship no test
   framework and we don't add packages for one.
2. **The two apps must stay behavior-identical** where they overlap:
   - `TapStateMachine` (Swift, C#, C) — same semantics, mirrored self-tests.
   - Storage format — byte-compatible Markdown/WAV per
     `docs/storage-format.md`; the C# self-test asserts byte-identical
     rendering vs the macOS output. Change the format ⇒ update both apps,
     both self-tests, and the doc in the same commit.
   - Cleanup prompts — canonical text lives in `shared/prompts/`; each app
     embeds a copy and its self-test compares against the files.
3. **Config decoding is tolerant** (missing keys → defaults, unknown keys
   ignored). Adding a config field must never reset a user's config.json —
   on macOS give new Codable fields a custom `init(from:)` with
   `decodeIfPresent` (see `CleanupConfig`).
4. **Secrets never touch config files** — macOS Keychain / Windows DPAPI,
   keyed by provider UUID. See `docs/mac/SECRETS.md`, `docs/windows/SECRETS.md`.
5. **Failures are loud and never lose audio** — the WAV is on disk before any
   network call; failed entries are saved with an error status and retryable.

## macOS gotchas (this machine)

- The app is **ad-hoc signed** (no identity in the keychain): every rebuild
  changes the signature, which **invalidates the Accessibility grant**. Don't
  rebuild casually; after a rebuild run
  `tccutil reset Accessibility io.sammons.voicevector` so the stale toggle
  doesn't confuse the user, then have them re-grant. A real cert via
  `VV_SIGN_ID` ends this.
- macOS 26 silently drops synthesized ⌘V from ad-hoc binaries; the app
  auto-detects this and pastes via AppleScript/System Events instead
  (`PasteService.synthesizedEventsLikelyBlocked`).
- Provider connection tests must exercise the endpoint the key is scoped to
  (e.g. ElevenLabs STT-only keys 401 on `/v1/user`) — see `TestAsync`/`test()`.

## Key docs

`docs/storage-format.md` · `docs/providers.md` · `docs/webhook-payload.md` ·
`docs/config-schema.md` · `ARCHITECTURE.md` (cross-platform) ·
`apps/macos/ARCHITECTURE.md` (macOS deep dive) · `docs/mac/*`, `docs/windows/*`
(per-platform clients / secrets / UI notes).
