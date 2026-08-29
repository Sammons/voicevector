#!/bin/sh
# Installs VoiceVector for the current user (no sudo): ~/.local/bin + a
# .desktop file so the desktop portals can identify the app.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
prefix="${PREFIX:-$HOME/.local}"
install -Dm755 "$here/voicevector" "$prefix/bin/voicevector"
install -Dm644 "$here/io.sammons.voicevector.desktop" "$prefix/share/applications/io.sammons.voicevector.desktop"
update-desktop-database "$prefix/share/applications" 2>/dev/null || true
echo "Installed $prefix/bin/voicevector"
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "Note: $prefix/bin is not on your PATH." ;; esac
if [ -r /dev/input/event0 ] 2>/dev/null; then
  echo "Raw input mode available (modifier-only hotkeys, like Right Alt)."
else
  echo "Optional — for modifier-only hotkeys (Right Alt to talk), add yourself to the input group:"
  echo "    sudo usermod -aG input \$USER    (then log out and back in)"
  echo "Without it, VoiceVector uses the system's shortcut portal (key combos, no setup)."
fi
