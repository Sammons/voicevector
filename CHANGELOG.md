# Changelog

All notable changes to VoiceVector. Each release's entry doubles as its
GitHub release notes. Versions follow [semantic versioning](https://semver.org);
both the macOS and Windows apps share one version number.

## v0.3.2 — 2026-08-29

Recording feel, especially with external audio interfaces.

- **Instant HUD.** The recording overlay appears the moment the hotkey fires;
  the microphone opens in the background. USB interfaces (Focusrite Scarlett
  and friends) take about half a second to open, and that used to block the
  whole UI.
- **Warm microphone (Settings → General).** Two independent options: keep the
  microphone open for 15 seconds after a recording (on by default — back-to-back
  takes start instantly) and always keep it open while VoiceVector runs (off by
  default). Either shows the OS mic-in-use indicator while the mic is open.
- **Level meter that works at any gain.** Conservatively gained interfaces
  produce speech around −40 dBFS — perfectly transcribable, but the old linear
  meter barely moved. The meter is now a fixed, quantized scale (−60 dBFS
  silence → −15 dBFS full, three steps), and handles Float32/Int16/Int32 and
  multichannel input, taking the loudest channel.
- **Voice detection relative to the noise floor** so silence-gap streaming
  (transcribe-while-you-pause) works on quiet inputs too.
- `VoiceVector --probe-audio [seconds]` (macOS) prints the default input's
  format, per-channel levels, and device open latency, for diagnosing this
  class of problem.

## v0.3.1 — 2026-08-29

- **About tab** in Settings: version, Sammons Software LLC attribution, and the
  full licensing terms with links to the license, commercial terms, and a
  purchase contact (sales@sammons.io).
- License text no longer references PolyForm (their terms require modified
  texts to drop the name).

## v0.3.0 — 2026-08-29

First release under the **VoiceVector Community License**: free for
individuals and for organizations with fewer than 1,000 employees and
contractors; larger organizations need a commercial license (US $50 per seat
per year). No feature differences, no license keys, no telemetry. Earlier
pre-release builds were withdrawn.

What the apps do at this point:

- Hotkey → record → cloud speech-to-text → optional LLM cleanup → paste into
  the frontmost app, with every recording kept as WAV + Markdown in folders
  you can see. Optional per-folder webhooks.
- **Any number of hotkeys**, each with its own cleanup mode (raw / light /
  rich), cleanup model, prompt, transcriber, and extra vocabulary.
- Providers: ElevenLabs (Scribe), Vercel AI Gateway, Cerebras, Fireworks, and
  any OpenAI-compatible endpoint (Azure AI Foundry, Foundry Local, Ollama…).
  Single-pass mode when transcription and cleanup use the same model.
- Streamed transcription during pauses so long dictations finish instantly.
- Hold-to-talk plus double- or single-tap latching; Esc cancels.
- macOS: signed and notarized, layout-aware paste that works in Terminal, VS
  Code, and Claude Code's TUI. Windows: a 60 KB single exe on the .NET
  Framework that ships with Windows; no admin required.
- One-click updates on both platforms, verifying the publisher on macOS.
