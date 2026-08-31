# config.json schema

Location: macOS `~/Library/Application Support/VoiceVector/config.json`;
Windows `%APPDATA%\VoiceVector\config.json`. Pretty-printed, hand-editable.
Decoding is tolerant: missing keys take defaults, unknown keys are ignored —
adding fields must never reset a user's config.

```jsonc
{
  "version": 1,
  "wizardCompleted": false,
  "dictationProfiles": [ {
      "id": "<uuid>",
      "name": "Default",
      "hotkey": { "keyCode": 61, "modifiers": 0, "isModifierOnly": true },
      //  keyCode is the platform code: macOS virtual keycode (61 = Right ⌥),
      //  Windows VK code (0xA5 = Right Alt). Not portable across OSes — each
      //  app keeps its own config file; the *library* is what syncs.
      "cleanupMode": "rich",            // "off" | "light" | "rich"; absent = legacy:
                                        //   inherit cleanup.mode (off if cleanupEnabled=false)
      "cleanupEnabled": true,           // legacy switch, kept in sync with cleanupMode
      "cleanupProviderID": null,        // null = cleanup.providerID
      "customPrompt": "",               // "" = cleanup.customPrompt, else built-in for mode
      "sttProviderID": null,            // null = top-level sttProviderID
      "vocabulary": "",                 // extra terms, appended to cleanup.vocabulary
      "reviewBeforePaste": false,       // stage the text in the HUD; speak changes; ⏎ pastes, Esc discards
      "reviewProviderID": null,         // model that applies spoken changes; null = cleanup provider
      "screenshotContext": false        // attach screenshots of every display to cleanup + review calls
      "routerEnabled": false,           // AI routing (needs reviewBeforePaste); see docs/multi-machine.md
      "routerProviderID": null,         // chat provider for the router; null = cleanup provider
      "autoSubmit": false               // press Enter after pasting (chat/terminal/routed delivery)
  } ],
  //  Any number of hotkeys, each with its own cleanup policy; always ≥ 1.
  //  The first profile is the "primary" one shown in the wizard/status line.
  //  Legacy configs with a top-level "hotkey" object migrate into profile[0].
  "tapStartMode": "doubleTap",          // "doubleTap" | "singleTap"
  "providers": [ {
      "id": "<uuid>", "kind": "elevenLabs|fireworks|vercelGateway|openAICompatible",
      "name": "…", "baseURL": "…", "sttModel": "…", "chatModel": "…"
  } ],
  "sttProviderID": "<uuid or null>",
  "cleanup": {                          // defaults inherited by profiles (set by the wizard);
    "mode": "off|light|rich",           //   the UI edits cleanup per hotkey, not here
    "providerID": "<uuid or null>",
    "vocabulary": "comma, separated, terms",   // global: STT keyterms + appended to every prompt
    "customPrompt": ""                  // empty = built-in prompt for mode
  },
  "playSounds": true,
  "chunkedTranscription": true,        // transcribe phrases during silence pauses
  "autoPaste": true,
  "appleScriptPaste": false,            // macOS only; ignored elsewhere
  "activeFolder": "Inbox",
  "folderWebhooks": { "<folder>": { "url": "", "includeAudio": false, "enabled": false } },
  "libraryPath": "~/Documents/VoiceVector",
  "keepMicWarmAfterRecording": true,   // keep the input open 15 s after a take
  "keepMicAlwaysWarm": false,          // keep the input open while the app runs
  //  Both trade the OS mic-in-use indicator for instant starts on external
  //  interfaces (which take ~0.5 s to open).
  "multiMachine": {                    // peering — docs/multi-machine.md
    "enabled": false,                  // run the TLS listener
    "machineName": "",                 // "" = the host name
    "port": 47800,
    "peers": [ {
      "name": "crankshaft",
      "fingerprint": "<64 hex>",       // SHA-256 of the peer's certificate (DER)
      "address": "100.114.151.71",     // host or host:port; "" = inbound-only
      "allowScreens": false,           // peer may fetch my screens/windows
      "allowDeliver": false            // peer may paste into me
    } ]
  }
}
```

API keys: secret store keyed by provider `id`
(macOS Keychain service `io.sammons.voicevector`, account = uuid;
Windows DPAPI-encrypted file `%APPDATA%\VoiceVector\keys\<uuid>`).

## Hotkey semantics (identical everywhere)

Recording starts on the first key-down (nothing clipped). Release ≥ 350 ms →
hold-to-talk commit. Short tap: `doubleTap` mode waits 400 ms for a second
tap (none → discard the stray recording); after the start gesture the
recording latches and the next tap commits. `singleTap` latches immediately.
Esc cancels. See `TapStateMachine` in either app — the two implementations
must stay behavior-identical (mirrored self-tests).

With multiple profiles, the profile whose hotkey started the gesture owns it
until it resolves; identical hotkey specs dedupe first-wins. Effective cleanup
per dictation (`CleanupEngine.effective`/`Effective` on both platforms):
a profile's explicit `cleanupMode` wins; a legacy profile without one inherits
`cleanup.mode` (forced to off when its `cleanupEnabled` is false). The
profile's `cleanupProviderID`/`customPrompt`/`sttProviderID` override the
globals when set, and its `vocabulary` is appended to the global list. Single-pass eligibility is evaluated against
the profile's effective provider.
