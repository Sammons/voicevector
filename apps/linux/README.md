# VoiceVector for Linux

C + GTK 4 / libadwaita, dynamically linked against libraries every desktop
already ships (GLib/GIO, libcurl, libsecret, libpulse). One small ELF, no
runtime to install, no third-party code.

| | |
|---|---|
| Hotkey | XDG **GlobalShortcuts** portal by default (sudo-free, one system dialog; press *and* release are delivered, so hold-to-talk works). Optional **raw input** mode reads `/dev/input` for modifier-only keys — needs one-time membership in the `input` group. |
| Paste | XDG **RemoteDesktop** portal (persisted permission) → clipboard + "press Ctrl+V" fallback. |
| Audio | PulseAudio / PipeWire (via `libpulse-simple`). |
| Secrets | `libsecret` (GNOME Keyring, KWallet). |
| Storage | Byte-identical to macOS/Windows — `docs/storage-format.md`. |

## Build

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libadwaita-1-dev \
     libcurl4-openssl-dev libsecret-1-dev libpulse-dev libglib2.0-dev
make            # build/voicevector
make core test  # headless core + self-test (only GLib + libcurl needed)
```

The cleanup and review prompts are generated into `build/prompts.h` from
`shared/prompts/` at build time; the self-test also compares them against the
files at runtime, like the other two apps.
