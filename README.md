# VoiceVector

Minimalist native dictation. Press a hotkey anywhere, speak, and the cleaned-up
transcript is pasted where your cursor is. Recordings and transcripts stay
local as plain WAV + Markdown files; folders can forward dictations to
webhooks. Zero third-party runtime dependencies on every platform.

| App | Stack | Status |
|---|---|---|
| [`apps/macos`](apps/macos) | Swift + AppKit/SwiftUI, SwiftPM (no Xcode needed) | working |
| [`apps/windows`](apps/windows) | C# / WPF on .NET Framework 4.8 (ships in Windows) — single ~2 MB exe, no admin, no installs | rewritten, hardware validation pending |

The two apps share no binary code — they share **contracts**:

- [`docs/storage-format.md`](docs/storage-format.md) — the WAV + Markdown
  library layout. Both apps read/write the same format, so a synced folder is
  a synced library.
- [`docs/providers.md`](docs/providers.md) — provider API contracts
  (ElevenLabs, Vercel AI Gateway, Fireworks, Cerebras, Azure AI Foundry /
  Foundry Local / any OpenAI-compatible endpoint).
- [`docs/webhook-payload.md`](docs/webhook-payload.md) — the webhook JSON.
- [`docs/config-schema.md`](docs/config-schema.md) — config.json fields.
- [`shared/prompts/`](shared/prompts) — canonical cleanup system prompts;
  each app embeds a copy and its self-test guards the invariants.
- Per-platform implementation notes: [`docs/mac/`](docs/mac) and
  [`docs/windows/`](docs/windows) (`CLIENTS.md`, `SECRETS.md`, `UI.md`), plus
  the cross-platform [`ARCHITECTURE.md`](ARCHITECTURE.md) and the contributor
  guide in [`CLAUDE.md`](CLAUDE.md).

Features beyond the basics: streamed transcription during silence pauses
(long dictations finish in the time of the last sentence), single-pass mode
(same provider + model for both stages sends audio + cleanup prompt in one
call), a customizable cleanup prompt, and one-click in-app updates.

**Download:** prebuilt apps for both platforms are on the
[Releases page](https://github.com/Sammons/voicevector/releases), built and
E2E-tested by CI. macOS builds are Developer ID signed and notarized — they
open first try. Windows builds are unsigned for now — use "Run anyway" or
`Unblock-File` on the zip before extracting.

Build: `make macos` / `make windows` (see each app's README for details).
Both apps carry a built-in dependency-free test suite: `--self-test`.
Releases: push a `v*` tag — CI builds, tests, and attaches both zips.

## License

VoiceVector is a [Sammons Software LLC](https://sammons.io) product,
source-available under the
[VoiceVector Community License](LICENSE): free for individuals and for organizations with fewer than
1,000 employees and contractors, including commercial use. Organizations
of 1,000 or more need a [commercial license](COMMERCIAL.md) —
US $50 per seat per year, no feature differences, no telemetry.
