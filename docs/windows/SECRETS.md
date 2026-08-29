# Windows — secrets

## API keys → DPAPI files

`src/VoiceVector.App/Services/DpapiKeyStore.cs`:

- Location: `%APPDATA%\VoiceVector\keys\<provider-guid>` (one file per
  provider profile, GUID = `ProviderProfile.Id`).
- Encryption: `CryptProtectData` / `CryptUnprotectData` (raw P/Invoke — no
  `System.Security.Cryptography.ProtectedData` package, keeping the zero-dep
  rule) with **CurrentUser** scope: only this Windows user on this machine
  can decrypt. No admin rights involved at any point.
- Empty key ⇒ file deleted; removing a provider in the UI deletes its key
  first (`SettingsUi` remove handler), so keys don't orphan.
- `config.json` (`%APPDATA%\VoiceVector\config.json`) stores profile
  metadata only — never keys.

Caveats:

- DPAPI CurrentUser ciphertext does **not** roam: copying the `keys/` folder
  to another machine or user profile yields undecryptable blobs — re-enter
  keys there. (A synced library folder is fine; it contains no secrets.)
- A password reset performed by an administrator (as opposed to a
  user-initiated change) can invalidate DPAPI master keys — worst case the
  user re-enters API keys.
- Same orphaning consequence as macOS: keys are keyed by profile UUID from
  config.json, which is why tolerant config decoding is a hard rule.

## Other sensitive surfaces

- Recordings/transcripts are plain files in `Documents\VoiceVector` by
  design; webhook URLs are cleartext config and receive transcript content.
- The clipboard paste path marks its transient writes with
  `CanIncludeInClipboardHistory=0`, `CanUploadToCloudClipboard=0`, and
  `ExcludeClipboardContentFromMonitorProcessing`, so transcripts don't linger
  in Win+V history or sync to the cloud clipboard unless pasting is off
  (clipboard-only mode writes unmarked on purpose, so the user can paste).
- `Log` keeps the last 200 error lines in memory only (shown in Settings);
  nothing is written to disk logs.
