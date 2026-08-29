# Windows — UI

WinUI 3 built **entirely in code** — no XAML files at all (`App.cs` supplies
its own `Main` with `DISABLE_XAML_GENERATED_MAIN` and merges
`XamlControlsResources` programmatically). This avoids the Windows-only XAML
compiler, which is what lets the whole app cross-compile from Linux/macOS for
type-checking. Mica backdrop, one violet accent, Fluent controls.

## Windows

| Class | Notes |
|---|---|
| `MainWindow.cs` | the one window. Header (brand, status line, Record pill button, gear) + `InfoBar` (all notices/errors — no toast notifications, unpackaged toasts aren't worth the registration) + a `ContentControl` that swaps between three builders: wizard / library / settings. Closing cancels via `AppWindow.Closing` and hides to the tray; Quit lives in the tray menu. |
| `HudWindow.cs` | floating recording pill: borderless `OverlappedPresenter`, always-on-top, hidden from switchers, and `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` on the HWND so it never steals focus; shown with `SW_SHOWNOACTIVATE`. Live 14-bar waveform from `Recorder.Level` on a 50 ms `DispatcherTimer`, elapsed clock, or the processing step. Positioned bottom-center of the work area. |

## Panels (built by `MainWindow` + `SettingsUi.cs`)

- **Library**: folder `Expander`s (count in header), newest-first entries,
  25/page "Show more"; a row click expands it inline to Cleaned/Raw blocks
  with Copy, an STT/cleanup metadata line, Retry (failures), Show Files
  (Explorer `/select`), Delete. Badges: `failed` / `cleanup failed` / `raw`.
  Footer: active-folder `ComboBox` + New Folder `ContentDialog`.
- **Settings** (`SettingsUi`, also reused inside the wizard): Providers
  (per-profile card: base URL for custom kinds with Azure Foundry / Foundry
  Local hints, `PasswordBox` key → DPAPI on change, model fields,
  use-for-transcription / use-for-cleanup checkboxes, Test), Dictation
  (hotkey recorder button driven by `KeyboardHook.CaptureHandler`, tap-mode
  radios, cleanup mode radios, cleanup-provider combo, vocabulary box, the
  full cleanup-prompt editor with reset-to-built-in), Folders & Webhooks
  (per-folder toggle + URL + attach-audio), General (chime/auto-paste
  toggles, library path + Open in Explorer, rerun wizard, recent errors).
- **Wizard**: welcome (includes the mic-privacy pointer — desktop apps use
  the global Windows toggle, there's no per-app prompt to gate on) →
  providers → hotkey → done.

## Refresh model

Deliberately simple: state lives in `App.Config`/`App.Library`;
`MainWindow.RefreshContent()` rebuilds the current panel's visual tree on any
change (`DictationController.LibraryChanged`, wizard step, settings toggles
that alter structure). At this app's scale that's cheap and eliminates a whole
class of binding bugs; there is no MVVM layer. Dictation state updates go
through `DictationController.StateChanged` → status line, Record button,
HUD show/hide, tray tooltip.

## Tray

`Services/TrayIcon.cs`: `Shell_NotifyIcon` on the main window's HWND via
`SetWindowSubclass`; left-click opens the window, right-click shows a Win32
`TrackPopupMenu` (Start/Stop Dictation, Open, Quit — quit disposes the icon,
unhooks the keyboard, exits). Uses the stock application icon for now
(swap in a real .ico later via `NOTIFYICONDATAW.hIcon`).
