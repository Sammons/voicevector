# Changelog

All notable changes to VoiceVector. Each release's entry doubles as its
GitHub release notes. Versions follow [semantic versioning](https://semver.org);
all three apps share one version number.

## Unreleased

- **Multi-machine peering** (Settings → Multi-machine, all three apps).
  VoiceVector instances on the same tailnet/LAN pair with a 6-digit code
  confirmed on both screens (Bluetooth-style numeric comparison over a
  commit-then-reveal exchange), then talk over TLS with pinned self-signed
  identities — no central authority. Per-peer permissions, both off by
  default: "may see my screens" and "may paste into me". Protocol:
  `docs/multi-machine.md`.
- **Route with AI** (per hotkey, needs Review before pasting). A router model
  looks at the draft, your windows and screens — and paired machines' too —
  and picks the window the text was meant for; the staging card shows its
  choice ("→ Slack on crankshaft") and ⏎ sends it there: the target window
  is raised before pasting locally, or the text is delivered to the paired
  machine, which pastes and saves it as a routed entry. Any router failure
  falls back to the normal paste. Prompt: `shared/prompts/router.txt`.
  Linux note: Wayland hides other apps' windows, so a Linux machine offers
  its screens and receives text into the focused window, without per-window
  targeting.

## v0.4.1 — 2026-08-30

- **Screenshot context covers every display.** One image per display is
  attached (not just the frontmost window), each preceded by a caption saying
  which display is active — the one the text will be inserted into — and on
  macOS and Windows that window is outlined in red in its screenshot. Linux
  crops the portal's desktop capture per monitor; Wayland hides the focused
  window, so with several displays the caption says the active one is unknown.
- **Screenshots are saved with the entry.** `<id>-screen-N.jpg` beside the
  WAV and Markdown, described by `screenshots` / `activeScreenshot` /
  `screenshotOutline` front-matter keys (`docs/storage-format.md`). One set per
  dictation, taken when the hotkey fires; a retry from the library reuses it
  instead of capturing whatever is on screen now. Deleted with the entry.
- **Fix (macOS):** the app crashed at launch when "Keep the microphone always
  warm" was on.
- **Fix (macOS):** binding a hotkey to a regular (non-modifier) key crashed
  the app whenever Settings showed that hotkey's name.
- **Fix (macOS):** a newly added hotkey that was never set swallowed the
  letter A everywhere (its placeholder is key code 0, which is A).

## v0.4.0 — 2026-08-30

- **Linux app** (`apps/linux/`, Ubuntu 26.04 / GNOME 50 reference; Wayland
  and X11). C on GTK 4 / libadwaita, dynamically linked against system
  libraries only — a ~230 KB binary, no runtime to install. Hotkeys through
  the XDG GlobalShortcuts portal by default (sudo-free, hold-to-talk works),
  or raw evdev input for modifier-only keys after a one-time `input` group
  step; paste through the RemoteDesktop portal with a clipboard fallback;
  API keys in the Secret Service. Same storage format, profiles, review mode,
  and prompts as the other two apps, checked by a 104-assertion self-test in
  CI. See `docs/linux/README.md`.
- **Review before pasting** (per hotkey). The cleaned text is staged in a
  card above the recording pill instead of being pasted. Press the hotkey and
  say a change — "make it shorter", "turn that into a list", "sign it Ben" —
  as many times as you like; a review model (the cleanup model by default, or
  one you pick) rewrites the draft each time. ⏎ pastes, Esc discards. Neither
  key reaches the app underneath while a draft is staged.
- **Screenshot context** (per hotkey). A screenshot of the frontmost window is
  captured when the hotkey fires and attached to the cleanup call and every
  revision, so the model knows what you're looking at. macOS asks for Screen
  Recording once; models without vision fall back to text automatically.
- Reviewer prompt lives in `shared/prompts/review.txt`, embedded and
  self-test-checked in both apps like the cleanup prompts.

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
