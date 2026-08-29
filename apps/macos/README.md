# VoiceVector for macOS

Minimalist native dictation for the Mac. Press a hotkey anywhere, speak, and the
cleaned-up transcript is pasted right where your cursor is. Recordings and
transcripts stay on your Mac as plain WAV + Markdown files; folders can forward
new dictations to a webhook (same spirit as the VoiceVector iOS/watchOS app).

**Zero third-party dependencies.** Pure Swift + Apple frameworks, built with
SwiftPM. No Xcode required — the command line tools are enough.

## Build & run

```sh
make app     # builds build/VoiceVector.app (release, signed)
make run     # build + open
make test    # runs the built-in self-test suite
```

Signing: `make app` prefers a **Developer ID Application** identity in your
keychain (hardened runtime + entitlements), else any identity (`$VV_SIGN_ID`
overrides), else ad-hoc. Release builds from CI are Developer ID signed and
notarized (`make notarize` does the same locally via the `vv-notary`
keychain profile). A stable identity keeps Accessibility/mic/Keychain grants
alive across rebuilds; ad-hoc builds re-prompt after every rebuild and, on
macOS 26+, paste via AppleScript instead of direct CGEvent (Apple gates
synthesized events from ad-hoc binaries).

## First run

The wizard walks through:
1. **Permissions** — Microphone (recording) and Accessibility (global hotkey +
   paste). The AppleScript paste fallback additionally prompts for Automation
   → System Events the first time it fires.
2. **Provider** — add ElevenLabs (STT), Fireworks or Cerebras (cleanup
   LLMs), Vercel AI Gateway (STT + cleanup), and/or any OpenAI-compatible
   endpoint (vLLM, Speaches, LocalAI, whisper.cpp server for STT; Ollama/
   LM Studio for cleanup — they have no STT endpoint; Azure AI Foundry and
   Foundry Local work via the OpenAI-compatible kind). Keys go in the
   Keychain, never in config files. Test the connection from the wizard.
3. **Hotkey** — default is Right ⌥. Hold-to-talk always works; tap mode is
   double-tap-to-start/tap-to-stop (or single-tap, your choice). Esc cancels.

## Dictation features

- **Streamed transcription** (on by default): after ~2 s of silence,
  completed phrases transcribe in the background while you keep talking —
  at stop only the tail transcribes, then cleanup runs once over the whole
  text. Entries show "(streamed N parts)" when it engaged.
- **Single-pass mode**: point transcription and cleanup at the *same
  provider and same model* (an audio-capable chat model, e.g. a Gemini
  model via the gateway) and the pipeline sends audio + cleanup prompt in
  one call. No separate raw transcript in this mode.
- **Cleanup prompt**: visible and editable in Settings → Dictation; the
  transcript is passed as delimited data so models edit it rather than
  answer it. Custom vocabulary is appended automatically.
- **One-click updates**: Settings → General checks GitHub Releases and
  swaps the app in place; downloads are verified against the running app's
  signing team before installing.

## Where things live

- `~/Documents/VoiceVector/<Folder>/<timestamp>.wav` — original recording
- `~/Documents/VoiceVector/<Folder>/<timestamp>.md` — transcript (front matter
  + cleaned text + raw text)
- `~/Library/Application Support/VoiceVector/config.json` — settings (readable,
  hand-editable)
- API keys — macOS Keychain, service `io.sammons.voicevector`

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md).
