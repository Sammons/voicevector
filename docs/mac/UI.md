# macOS — UI

AppKit lifecycle hosting SwiftUI views. No storyboards/nibs; the whole UI is
code. One accent color, minimal chrome — everything visual funnels through
`UI/Theme.swift` (violet accent, `vvCard()` container, `Pill` badge).

## Shell (`App/`)

- `Main.swift` — `@main`, runs `SelfTest` when launched with `--self-test`,
  else standard `NSApplication.run`.
- `AppDelegate.swift` — owns the single main `NSWindow` (SwiftUI content via
  `NSHostingView`), the status item, the app menu (so ⌘C/⌘V/⌘Q work in our
  own windows), and swaps window content between `WizardView` and `MainView`
  when `config.wizardCompleted` flips. Last window closing does **not** quit —
  dictation keeps running from the status item.

## Views (`UI/`)

| View | Notes |
|---|---|
| `MainView` | header (brand, status line, Record button, gear) + error banner + `LibraryListView` + footer (active-folder picker, New Folder popover). Settings is a sheet. |
| `LibraryListView` | collapsible folder sections, newest-first, 25/page ("Show more"); rows expand inline to Cleaned/Raw blocks with Copy buttons, metadata line (STT/cleanup labels), Retry (on failure), Show Files, Delete. Refreshes via `.id(dictation.libraryGeneration)` — a generation counter bumped by the controller. |
| `SettingsView` | sheet with tabs: Providers (list + `ProviderEditor`: base URL, key, models list with STT/Chat assign buttons, Test), Dictation (`HotkeyRecorderView`, tap mode, cleanup mode/provider picker + missing-provider warning, vocabulary, `CleanupPromptEditor` — shows the exact system prompt; editing saves a custom prompt, Reset returns to built-in), Folders & Webhooks, General (chimes, auto-paste, AppleScript paste, permissions rows, storage, recent errors). |
| `WizardView` | first run: welcome → permissions (mic + Accessibility with live re-check) → provider cards + compact editors with Test → hotkey + tap mode → done. Continue is gated on permissions. |
| `RecordingHUD` | non-activating borderless `NSPanel` (`.statusBar` level, all Spaces, ignores mouse) centered near the bottom of the screen; SwiftUI content shows a live 14-bar waveform fed by `Recorder.level` at 20 Hz, elapsed clock, or the processing step. Never steals focus from the target app. |
| `HotkeyRecorderView` | toggles `HotkeyEngine.captureMode`; accepts a plain key + modifiers or a lone modifier tap (Right ⌥, fn…). |

## State flow

`AppState` (`UI/AppState.swift`) is the single `ObservableObject`: publishes
`config` (persisting on `didSet`, reconfiguring the hotkey engine on hotkey /
tap-mode change), permission flags, and wires
`HotkeyEngine.onAction → DictationController.handle`. `DictationController`
publishes `state` (drives Record button, status line, HUD show/hide, status
item icon) and `libraryGeneration` (drives list reloads).

Status item: `waveform.circle` idle → red `record.circle.fill` recording →
tinted `waveform.circle.fill` processing; menu offers Start/Stop Dictation,
Open, Settings, Quit.

Chimes (`Core/Chime.swift`): synthesized two-note sine buffers through a tiny
AVAudioEngine — rising = start, falling = stop, low = error; honored by the
`playSounds` setting except errors, which always sound.
