# Changelog

All notable changes to VoiceVector. Each release's entry doubles as its
GitHub release notes. Versions follow [semantic versioning](https://semver.org);
all three apps share one version number.

## v0.6.5 — 2026-09-20

A bug sweep of both apps after the v0.6.2–v0.6.4 hotfixes. Nothing here
changes the storage format or config schema; existing files keep working.

**Both platforms**

- **Removing a provider no longer breaks hotkeys that pointed at it.** A
  per-hotkey transcriber/cleanup/review/router override referencing a deleted
  provider used to make that hotkey fail ("No transcription provider") while
  Settings showed a valid default. Overrides now revert to Default on removal,
  and a stale override falls back to the global default at dictation time.
- **Interrupted recordings are no longer lost.** A `.wav` whose pipeline never
  finished (quit, crash, force-kill, an update mid-transcription) had no `.md`
  and was invisible. At launch each one now gets a failed entry with Retry.
- **A discarded take can't leak into a later Retry.** Streamed segments from
  an Esc-cancelled recording were kept and prepended to the next retried
  transcript (and still uploaded). They're dropped on discard.
- **Retry keeps the original date** instead of restamping the entry to now.
- **Pre-release versions compare correctly** in the updater
  (`0.6.4-beta.1` no longer parses as `0.6.1`).
- **An unreadable config.json is preserved, not overwritten.** It's renamed to
  `config.json.broken-<timestamp>` beside fresh defaults; Windows now writes the
  file atomically so a crash mid-save can't truncate it.

**macOS**

- **Quit from the menu bar or ⌘Q works while Settings is open** (same AppKit
  sheet deferral that broke the updater in v0.6.2).
- **Pairing requests show while Settings is open** instead of queueing behind
  it until the other machine times out.
- **Safer in-app update:** the new bundle is copied in beside the old one and
  swapped with rollback (a failed copy leaves the installed app intact); paths
  with spaces or quotes are handled; updating a quarantined
  (App-Translocated) copy is refused with an explanation.
- **Hotkey capture** targets the row you clicked even if rows are deleted
  meanwhile, only one row can be in "Press a key…" at a time, and Esc cancels.
- Config decoding is tolerant for provider, hotkey and webhook entries and for
  unknown enum values (a newer build's config no longer resets an older one).
- A library write failure is announced instead of silently dropping the entry.
- The "Transcription" role toggle is disabled for a provider with no STT model
  (the Dictation tab couldn't display that selection).
- The recording HUD no longer re-renders 20×/s while hidden.

**Windows**

- **A failure anywhere in the dictation pipeline no longer wedges the app in
  "Processing"** with every hotkey ignored; the entry is saved as failed with
  Retry and the error is shown.
- **Microphone failures are loud:** if the device can't be opened (privacy
  setting, unplugged, unsupported format) the hotkey reports "Could not start
  recording" instead of silently recording nothing.
- **Only one instance runs.** Autostart plus a Start Menu click no longer
  yields two copies each with its own keyboard hook (double pastes, two tray
  icons).
- **Hotkey capture can't be left armed:** leaving Settings, rebuilding the page,
  or pressing Esc disarms "Press a key…" (previously the next keystroke in any
  app was swallowed and became the hotkey); arming one row disarms another.
- **Finishing a dictation no longer rebuilds Settings under you** (lost focus,
  lost caret, scroll reset); only the library view refreshes. The footer folder
  picker now shows newly created folders.
- **Cleanup prompt editor:** switching mode shows that mode's built-in prompt
  (editing the stale one saved the wrong prompt as custom); the first edit
  immediately labels it custom and offers Reset.
- API-key save failures (DPAPI/disk) are shown next to the field.
- Update script: matches the process by name as well as PID, retries the copy,
  and relaunches the old exe if the copy never succeeds.
- Start-with-Windows registry command is quoted; the tray icon is removed on
  every exit path; the keyboard hook can't crash the process if a hotkey is
  added or removed mid-keystroke; the HUD only advertises "Enter: paste / Esc:
  discard" when those keys actually do that; text fields show the focus ring
  again.
- **Per-hotkey "Default (…)" pickers went stale.** After choosing a global
  transcription provider, a hotkey's Transcriber picker could still read
  "Default (none)" — on macOS because menu pickers keep an item's old title
  when only its text changes, on Windows because the hotkey rows were built
  once and not refreshed. Both now update immediately (same fix for the
  per-hotkey Cleanup model picker on macOS).

## v0.6.4 — 2026-09-19

Windows hotfix:

- **"Update & Restart" threw a threading error.** After downloading
  the new exe the updater asked WPF to shut down from a background thread
  ("The calling thread cannot access this object because a different thread
  owns it"), so the app stayed open with the swap script waiting on it. The
  shutdown is now marshalled to the UI thread, and the swap script stops
  waiting after 20 s and closes the app itself, mirroring the macOS fix in
  v0.6.3.

## v0.6.3 — 2026-09-18

Two hotfixes from user testing of v0.6.2:

- **Windows: dropdowns were dead.** Every ComboBox in v0.6.2 (the
  "Add provider…" picker, STT/cleanup/review/router provider pickers, hotkey
  mode, the library folder picker) ignored clicks: the Fluent template drew
  its invisible open/close button *underneath* the opaque face, so the face
  swallowed every click. The button now sits on top and the dropdowns open.
- **macOS: "Update & Relaunch" never quit the app.** The updater staged the
  new build, then asked AppKit to terminate while the Settings sheet was
  still open — and AppKit silently defers termination behind a SwiftUI
  sheet, so the app sat there until force-quit. Settings now closes before
  the quit, the app exits directly if AppKit still balks, the swap script
  stops waiting after 20 s and quits the app itself, and updating is refused
  while a dictation is in flight.

## v0.6.2 — 2026-09-14

Windows delivery and polish, from user testing:

- **Single-exe download — no zip.** The Windows release is now one signed
  `VoiceVector-windows-x64.exe` (SmartScreen shows a verified publisher);
  no zip to extract, no `.config` sidecar, and the in-app updater swaps the
  exe directly.
- **Real app icon everywhere** — exe, title bar, and system tray (the tray
  previously showed the default application glyph).
- **Fluent styling across the app** — checkboxes, combo dropdowns, radio
  buttons, text fields and tooltips now match the violet/Fluent look instead
  of WPF's legacy default chrome.
- **Optional user-land installer** — first run offers to copy the app to
  `%LOCALAPPDATA%\Programs\VoiceVector` with a per-user Start Menu shortcut
  and optional start-with-Windows. Never needs admin; declining keeps the
  portable single file.

## v0.6.1 — 2026-09-13

- **Windows builds are now Authenticode-signed.** Releases are signed via
  Azure Artifact Signing with a public-trust certificate issued to
  **Sammons Software LLC**; the signing key never leaves Microsoft's HSM and
  CI authenticates keylessly through GitHub OIDC. The exe in this and all
  future releases shows a verified publisher in SmartScreen instead of the
  "unknown publisher" warning. Past unsigned releases keep validating
  forever via their timestamp.
- No code changes since v0.6.0 — this release exists to ship the signature.

## v0.6.0 — 2026-08-31

Big release: review-before-pasting, multi-machine peering, and AI routing.
Multi-machine and routing are labeled **Experimental** in Settings — new and
still rough; great to try, don't rely on them yet.

- **Router picks a numbered destination, not an opaque window id.** The router
  model now gets one flat, numbered list of destinations (0 = leave the cursor
  where it is) and replies with a single small number, which the app maps back
  to the real machine + window. Previously it had to echo a 5-digit window id
  verbatim, which small models frequently got wrong — so routing failed and
  fell back to a local paste more often than it should. Much more reliable.
- **See where routed text is going, and pick the field.** When the router
  targets a window, the staging card now shows a thumbnail of that window with
  the destination input field highlighted; press ⇥ (⇧⇥ to go back) to cycle
  through the window's input fields before you hit ⏎. The receiving machine
  focuses the exact field you chose. Works for a window on this machine or a
  paired one (macOS; the target must share its screen). New `window` peer
  request and a `field` index on `deliver` (docs/multi-machine.md).
- **Routed text lands in the app's input field.** After the receiving machine
  foregrounds the target app, its text input isn't always focused, so the
  paste went nowhere. macOS now walks the foregrounded window's Accessibility
  tree and focuses the first editable text field/area (the chat box, terminal,
  etc.) before pasting. Needs the receiving Mac's Accessibility permission.
  (Windows foregrounds and pastes into the app's focused control; Linux/Wayland
  pastes into the focused window — no per-field control there.)
- **Cross-machine routing actually reaches paired machines.** Pairing now
  records the peer's network address on *both* sides (the receiving side reads
  it from the connection), so you no longer have to type it in by hand. And a
  paired machine is offered to the router even when it isn't sharing its
  screens — "send this to <machine>" routes to its focused window (window 0);
  sharing screens still adds per-window targeting. (Delivery into a machine
  still requires that machine's "May paste into me" permission.)
- **Auto-submit (press Enter after pasting)** — a per-hotkey option, off by
  default. After the text is pasted the app presses Enter, so a dictated chat
  message or terminal command is sent without touching the keyboard. It also
  travels with a routed delivery: the *receiving* machine presses Enter, which
  is the point for the cross-machine case where no one is at that keyboard.
  Leave it off for editors, where Enter just inserts a line.
- **Speak the destination.** When you open a router dictation by naming where
  it should go — "Hey Slack, …", "Send this to the terminal —", "Tell Ben
  that …" — the router now honours that spoken destination (it's given your
  original words alongside the window lists), and the cleanup strips the
  addressing phrase out of the pasted text instead of leaving it in.
- **Routed text only pastes once the target window is actually focused.** On
  macOS and Windows the app now confirms the destination window came to the
  front before pasting; if the system refuses the focus change, the text is
  copied with a "click it and press ⌘V/Ctrl+V" notice instead of landing in
  whatever window happened to be focused (e.g. the terminal). (Linux/Wayland
  can't focus another app's window, so it pastes into the focused one as
  before.)
- **The AI router's choice is validated and bounced back.** The destination
  must be a machine and window that were actually offered (or window 0 for the
  current focus); an invalid or hallucinated pick is returned to the router
  with a specific error telling it what was wrong, up to three tries, before
  falling back to the normal local paste. No more silent routing to nothing.
- **A spoken revision that transcribes to nothing now says so** ("Didn't catch
  that…") instead of silently doing nothing.
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
