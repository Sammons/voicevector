#!/bin/bash
# Builds VoiceVector.app from the SwiftPM product — no Xcode required.
#   scripts/make_app.sh [debug|release]
# Signing: uses $VV_SIGN_ID if set, otherwise the first valid codesigning
# identity in the keychain, otherwise ad-hoc. (On macOS 26+ an ad-hoc build
# automatically falls back to AppleScript-based pasting at runtime.)
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
APP="build/VoiceVector.app"

echo "› swift build -c $CONFIG"
swift build -c "$CONFIG"

BIN=".build/$CONFIG/VoiceVector"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN" "$APP/Contents/MacOS/VoiceVector"
cp scripts/Info.plist "$APP/Contents/Info.plist"

# Version stamp: releases set VV_VERSION (e.g. 0.2.0); local builds are -dev.
VERSION="${VV_VERSION:-0.0.0-dev}"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$APP/Contents/Info.plist"

echo "› generating icon"
ICONSET="build/AppIcon.iconset"
swift scripts/make_icon.swift "$ICONSET" >/dev/null
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

SIGN_ID="${VV_SIGN_ID:-}"
# Prefer a Developer ID certificate when one exists; else any identity.
if [ -z "$SIGN_ID" ]; then
    SIGN_ID="$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -o '"Developer ID Application[^"]*"' | head -1 | tr -d '"' || true)"
fi
if [ -z "$SIGN_ID" ]; then
    SIGN_ID="$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -o '"[^"]*"' | head -1 | tr -d '"' || true)"
fi
if [[ "$SIGN_ID" == Developer\ ID\ Application* ]]; then
    echo "› codesign (Developer ID, hardened runtime): $SIGN_ID"
    codesign --force --options runtime --timestamp \
        --entitlements scripts/entitlements.plist --sign "$SIGN_ID" "$APP"
elif [ -n "$SIGN_ID" ]; then
    echo "› codesign with identity: $SIGN_ID"
    # No hardened runtime for self-signed identities: it demands entitlements
    # the cert can't notarize anyway; stable identity is what keeps TCC alive.
    codesign --force --sign "$SIGN_ID" "$APP"
else
    echo "› codesign ad-hoc (no identity found)"
    codesign --force --sign - "$APP"
fi

echo "✓ $APP"
