#!/bin/sh
# Prints the CHANGELOG.md section for a tag (e.g. v0.3.2) — used by the release
# workflow as the GitHub release body. For a pre-release tag (a semver suffix
# like v0.5.0-beta.1) the exact section usually doesn't exist yet, so fall back
# to the "Unreleased" section, then to a pointer at the changelog.
set -eu
tag="$1"

section() {
  awk -v tag="$1" '
    /^## / { if (found) exit; if (index($0, "## " tag " ") == 1 || $0 == "## " tag) { found = 1; next } }
    found { print }
  ' CHANGELOG.md
}

blank() { [ -z "$(printf '%s' "$1" | tr -d '[:space:]')" ]; }

notes=$(section "$tag")
# Pre-release tags (contain a hyphen) borrow the in-progress notes.
case "$tag" in
  *-*)
    if blank "$notes"; then notes=$(section "Unreleased"); fi
    ;;
esac
if blank "$notes"; then
  notes="See [CHANGELOG.md](https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md)."
fi
printf '%s\n\nFull changelog: https://github.com/Sammons/voicevector/blob/main/CHANGELOG.md\n' "$notes"
