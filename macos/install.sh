#!/usr/bin/env bash
# install.sh — install DSInput.app into ~/Library/Input Methods.
#
# The Swift frontend registers via the system's login-time input-source scan, so
# after installing you must LOG OUT and BACK IN, then add DS Input under
# System Settings ▸ Keyboard ▸ Input Sources ▸ (+). (There is no live "--register"
# path: TISRegisterInputSource on a brand-new source returns noErr but doesn't
# surface it until the next login — same as WeType / vChewing.)
#
# The bundle must be Developer-ID signed for the system to load it. Notarization
# is required for DISTRIBUTION (Gatekeeper unquarantines downloaded apps) but NOT
# for a locally-built, locally-installed bundle — it carries no quarantine flag.
#
# Run `macos/build.sh` first to produce build/DSInput.app.
#
# Usage: bash macos/install.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_SRC="$SCRIPT_DIR/build/DSInput.app"
DEST_DIR="$HOME/Library/Input Methods"
DEST_APP="$DEST_DIR/DSInput.app"

if [[ ! -d "$APP_SRC" ]]; then
    echo "ERROR: $APP_SRC not found — run 'bash macos/build.sh' first." >&2
    exit 1
fi

echo "==> Stopping any running DSInput…"
pkill -x DSInput 2>/dev/null || true

echo "==> Installing to ${DEST_APP}…"
mkdir -p "$DEST_DIR"
rm -rf "$DEST_APP"
cp -R "$APP_SRC" "$DEST_DIR/"

# Strip any OTHER on-disk copy of this bundle id from the LaunchServices index.
# The Text Input service resolves io.github.madeye.inputmethod.dsinput THROUGH LaunchServices;
# if a stray copy (the build/ output, or an ad-hoc Xcode DerivedData build) is also
# registered, LS can bind the id to that shadow copy — which lives outside an Input
# Methods directory (and may be ad-hoc signed), so registration silently no-ops and
# the IME never appears in System Settings. De-dupe so DEST_APP is the sole claimant.
echo "==> De-duplicating LaunchServices registrations for the bundle id…"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Support/lsregister"
BUNDLE_ID="io.github.madeye.inputmethod.dsinput"
if [[ -x "$LSREGISTER" ]]; then
    # Unregister every registered path for this id whose location is NOT the install
    # destination (the build/ source copy, DerivedData Debug/Release builds, etc.).
    while IFS= read -r stray; do
        [[ -z "$stray" || "$stray" == "$DEST_APP" ]] && continue
        echo "    unregistering stray: $stray"
        "$LSREGISTER" -u "$stray" 2>/dev/null || true
    done < <("$LSREGISTER" -dump 2>/dev/null | awk '
        /^[[:space:]]*path:/ {
            line = $0
            sub(/^[[:space:]]*path:[[:space:]]*/, "", line)   # drop the "path:" label
            sub(/[[:space:]]+\(0x[0-9a-f]+\)[[:space:]]*$/, "", line)  # drop trailing (0x…)
            p = line                                          # keep full path incl. spaces
        }
        /identifier:.*'"$BUNDLE_ID"'/ { print p }')
    # Re-assert the installed copy as the canonical registration.
    "$LSREGISTER" -f "$DEST_APP" 2>/dev/null || true
else
    echo "    (lsregister not found — skipping de-dup)" >&2
fi

# A stale system-wide copy in /Library/Input Methods shares the same bundle id and
# would re-collide on the next login scan (and may win id resolution). We can't
# remove it without root, so surface it for the user to delete.
SYSTEM_APP="/Library/Input Methods/DSInput.app"
if [[ -d "$SYSTEM_APP" ]]; then
    echo ""
    echo "⚠  A system-wide copy exists and will collide: $SYSTEM_APP"
    echo "   Remove it (needs admin), then log out:"
    echo "     sudo rm -rf \"$SYSTEM_APP\""
fi

echo ""
echo "Done — installed to $DEST_APP."
echo "Now LOG OUT and BACK IN (the input-source scan only runs at login), then add"
echo "DS Input under System Settings ▸ Keyboard ▸ Input Sources ▸ (+) ▸ Simplified."
echo "Open Preferences from the input menu and set your API key before typing."
