# Linux notes

The Linux app (`apps/linux/`) is C on GTK 4 / libadwaita, dynamically linked
against libraries every desktop ships: GLib/GIO, libcurl, libsecret, libpulse.
It targets Wayland desktops (GNOME 50 on Ubuntu 26.04 is the reference) and
works on X11 sessions through the same portals.

## Hotkeys — two backends

| | Portal shortcut (default) | Raw input |
|---|---|---|
| Mechanism | XDG `GlobalShortcuts` portal; the desktop shows a one-time dialog and the user can rebind/revoke in Settings → Keyboard | reads `/dev/input/event*` (evdev), the same layer the compositor's own input stack uses |
| Setup | none | one-time `sudo usermod -aG input $USER` + re-login |
| Keys | combos only (`Ctrl+Alt+Space`) | any key, including modifier-only (`Right Alt`) |
| Hold-to-talk | yes — the portal delivers press and release | yes |
| Desktops | GNOME 48+, KDE Plasma 6 (wlroots compositors lack the backend) | all |

"Automatic" (the default setting) uses raw input when the account can read
`/dev/input` and the portal otherwise. Raw input cannot swallow keys, so
review accept/discard and cancel are combos on both backends:
**Ctrl+Alt+Enter** pastes a reviewed draft, **Ctrl+Alt+Esc** cancels a
recording or discards a draft. These are bound as portal shortcuts too.

Portal 1.21+/GNOME 50 requires an app id; the app registers
`io.sammons.voicevector` and the `.desktop` file installed by `make install`
/ `install.sh` lets the desktop resolve it.

## Paste

The XDG `RemoteDesktop` portal (keyboard only, `persist_mode = 2`) types
Ctrl+V into the focused app; the one-time permission is restored from a token
kept in `~/.config/voicevector/remote-desktop.token`. If the portal is
unavailable or denied, the text is on the clipboard and the status line says
to press Ctrl+V.

## Screenshots

"Screenshot context" uses the `Screenshot` portal (non-interactive). Some
desktops show a confirmation dialog for unsandboxed apps.

## Secrets

API keys go to the Secret Service (`libsecret`; GNOME Keyring or KWallet)
under schema `io.sammons.voicevector`, attribute `provider` = provider UUID.

## Files

`~/.config/voicevector/config.json` (same schema as the other apps) and the
library at `~/Documents/VoiceVector` by default.
