#!/bin/sh
# Prints the CHANGELOG.md section for a tag (e.g. v0.3.2) — used by the
# release workflow as the GitHub release body. Falls back to a pointer at the
# changelog if the section is missing.
set -eu
tag="$1"
notes=$(awk -v tag="$tag" '
  /^## / { if (found) exit; if (index($0, "## " tag " ") == 1 || $0 == "## " tag) { found = 1; next } }
  found { print }
' CHANGELOG.md)
if [ -z "$(printf '%s' "$notes" | tr -d '[:space:]')" ]; then
  notes="See [CHANGELOG.md](https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md)."
fi
printf '%s\n\nFull changelog: https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md\n' "$notes"
