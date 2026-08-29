# macOS — secrets

## API keys → Keychain

`Sources/VoiceVector/Core/Keychain.swift` — generic passwords:

- service: `io.sammons.voicevector`
- account: the provider profile's UUID (`ProviderProfile.id`)
- value: the API key, UTF-8

Write = update-then-add (`SecItemUpdate` → `SecItemAdd`); empty string deletes.
Removing a provider in the UI calls `Keychain.deleteAPIKey` first
(`AppState.removeProvider`), so keys don't orphan. `config.json` stores only
profile metadata — **never** keys.

Consequence of the UUID keying: if config.json is lost/reset, the profile
UUIDs are gone and existing Keychain items become unreachable orphans. This is
why tolerant config decoding (CLAUDE.md hard rule 3) matters.

## Signing, TCC, and why rebuilds hurt

The app is ad-hoc signed unless `VV_SIGN_ID` (or a keychain identity) is
present at `scripts/make_app.sh` time. Ad-hoc designated requirements are
CDHash-bound, so **every rebuild is a new identity** to macOS:

- The Accessibility grant stops matching → hotkey tap and paste stop working
  while System Settings still shows the (stale) toggle ON. Fix:
  `tccutil reset Accessibility io.sammons.voicevector`, relaunch, re-grant.
- Keychain items remain readable (same bundle path/user), but a real
  signing identity is still the durable fix for TCC.
- macOS 26 additionally drops synthesized key events from ad-hoc binaries at
  the WindowServer level; `PasteService.synthesizedEventsLikelyBlocked`
  detects "ad-hoc + macOS ≥ 26" via `SecCodeCopySigningInformation` (no team
  identifier ⇒ ad-hoc) and routes pasting through AppleScript/System Events,
  which prompts once for the Automation permission.

## Other sensitive surfaces

- Recordings + transcripts are plain files under `~/Documents/VoiceVector` —
  by design (user-owned data), not secret storage.
- Webhook URLs live in config.json in the clear; treat webhook endpoints as
  potentially receiving transcript content and audio.
- `Log` mirrors errors (which may include HTTP body snippets) to the unified
  log as `public` — error snippets are truncated to 300 chars and API keys
  never appear in URLs or logged headers.
