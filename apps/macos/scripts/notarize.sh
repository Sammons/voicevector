#!/bin/bash
# Notarizes and staples build/VoiceVector.app (requires Developer ID signing).
# One-time setup:
#   xcrun notarytool store-credentials vv-notary \
#     --apple-id <your-apple-id> --team-id <TEAMID> --password <app-specific-password>
# (App-specific password: account.apple.com → Sign-In and Security → App-Specific Passwords)
set -euo pipefail
cd "$(dirname "$0")/.."

APP="build/VoiceVector.app"
ZIP="build/VoiceVector-notarize.zip"

[ -d "$APP" ] || { echo "Build first: make app (with VV_SIGN_ID='Developer ID Application: …')"; exit 1; }
SIGN_INFO=$(codesign -dvv "$APP" 2>&1)
case "$SIGN_INFO" in
    *"Authority=Developer ID"*) ;;
    *) echo "App is not Developer ID signed — notarization requires it."; exit 1 ;;
esac

rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "› submitting to Apple notary service (waits for verdict)…"
xcrun notarytool submit "$ZIP" --keychain-profile vv-notary --wait
echo "› stapling ticket"
xcrun stapler staple "$APP"
spctl -a -vv "$APP"
echo "✓ notarized & stapled"
