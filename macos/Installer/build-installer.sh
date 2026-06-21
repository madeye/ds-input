#!/usr/bin/env bash
# build-installer.sh — build DSInputInstaller.app: a guided, per-user installer
# that embeds DSInput.app and installs it into ~/Library/Input Methods.
#
# Usage: bash macos/Installer/build-installer.sh
#
# Steps:
#   1. Build the IME (macos/build.sh) — Developer-ID signed so it can register.
#   2. xcodebuild the installer app (unsigned).
#   3. Embed the signed DSInput.app into the installer's Resources.
#   4. Sign the installer (outer bundle; the embedded IME keeps its signature).
#
# Output: macos/Installer/build/DSInputInstaller.app

set -euo pipefail

INSTALLER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS_DIR="$(dirname "$INSTALLER_DIR")"
BUILD_DIR="$INSTALLER_DIR/build"
DERIVED_DIR="$BUILD_DIR/DerivedData"
XCODEPROJ="$INSTALLER_DIR/DSInputInstaller.xcodeproj"

# Args:
#   --notarize   notarize + staple the signed installer (for distribution).
#   --zip        also produce DSInputInstaller.zip next to the .app.
NOTARIZE=0
ZIP=0
for arg in "$@"; do
    case "$arg" in
        --notarize) NOTARIZE=1 ;;
        --zip)      ZIP=1 ;;
        *) echo "ERROR: unknown arg '$arg' (use --notarize / --zip)." >&2; exit 2 ;;
    esac
done

# Signing: set via env or a gitignored `macos/.signing.env` (sourced here).
# Empty SIGN_IDENTITY → ad-hoc (fine to run locally; not for distribution).
[[ -f "$MACOS_DIR/.signing.env" ]] && source "$MACOS_DIR/.signing.env"
SIGN_IDENTITY="${SIGN_IDENTITY:-}"
DEV_TEAM="${DEV_TEAM:-}"
NOTARYTOOL_KEY="${NOTARYTOOL_KEY:-}"
NOTARYTOOL_KEY_ID="${NOTARYTOOL_KEY_ID:-}"
NOTARYTOOL_ISSUER="${NOTARYTOOL_ISSUER:-}"

# ── 1. Build the IME (signed; not separately notarized — see step 5) ─────────
echo "==> Building DSInput.app (the IME to embed)…"
bash "$MACOS_DIR/build.sh"
IME_APP="$MACOS_DIR/build/DSInput.app"
[[ -d "$IME_APP" ]] || { echo "ERROR: $IME_APP not produced." >&2; exit 1; }

# ── 2. Build the installer (unsigned) ────────────────────────────────────────
echo "==> Building the installer app…"
( cd "$INSTALLER_DIR" && xcodegen generate --spec project.yml >/dev/null )
rm -rf "$DERIVED_DIR"
xcodebuild -project "$XCODEPROJ" -scheme DSInputInstaller -configuration Release \
    -derivedDataPath "$DERIVED_DIR" CODE_SIGNING_ALLOWED=NO build >/dev/null
INSTALLER_APP="$DERIVED_DIR/Build/Products/Release/DSInputInstaller.app"
[[ -d "$INSTALLER_APP" ]] || { echo "ERROR: installer not produced." >&2; exit 1; }

# ── 3. Embed the IME ─────────────────────────────────────────────────────────
echo "==> Embedding DSInput.app…"
mkdir -p "$INSTALLER_APP/Contents/Resources"
rm -rf "$INSTALLER_APP/Contents/Resources/DSInput.app"
cp -R "$IME_APP" "$INSTALLER_APP/Contents/Resources/DSInput.app"

# ── 4. Sign the installer (the embedded IME is already signed) ───────────────
if [[ -n "$SIGN_IDENTITY" ]]; then
    echo "==> Signing the installer with: $SIGN_IDENTITY"
    codesign --force --options runtime --timestamp \
        --sign "$SIGN_IDENTITY" "$INSTALLER_APP"
    codesign --verify --deep --strict "$INSTALLER_APP" && echo "    signature OK"
else
    echo "==> Ad-hoc signing the installer."
    codesign --force --deep --sign - "$INSTALLER_APP" || true
fi

# ── 5. Notarize + staple the installer (distribution) ────────────────────────
# The embedded IME is signed (hardened runtime + timestamp) but not separately
# notarized — Apple notarizes everything in this submission, and the installer
# strips the IME's quarantine at install time, so the copied IME loads fine.
if [[ "$NOTARIZE" == 1 ]]; then
    [[ -n "$SIGN_IDENTITY" && -f "${NOTARYTOOL_KEY:-/nonexistent}" ]] || {
        echo "ERROR: --notarize needs SIGN_IDENTITY + NOTARYTOOL_KEY/KEY_ID/ISSUER" >&2
        echo "       (set them in macos/.signing.env)." >&2; exit 1; }
    echo "==> Notarizing the installer (uploads to Apple and waits)…"
    NZIP="$BUILD_DIR/DSInputInstaller-notarize.zip"
    /usr/bin/ditto -c -k --keepParent "$INSTALLER_APP" "$NZIP"
    xcrun notarytool submit "$NZIP" \
        --key "$NOTARYTOOL_KEY" --key-id "$NOTARYTOOL_KEY_ID" \
        --issuer "$NOTARYTOOL_ISSUER" --wait
    rm -f "$NZIP"
    echo "==> Stapling…"
    xcrun stapler staple "$INSTALLER_APP"
    spctl -a -vvv -t exec "$INSTALLER_APP" 2>&1 | head -3 || true
fi

OUT="$BUILD_DIR/DSInputInstaller.app"
rm -rf "$OUT"
cp -R "$INSTALLER_APP" "$OUT"

if [[ "$ZIP" == 1 ]]; then
    OUTZIP="$BUILD_DIR/DSInputInstaller.zip"
    rm -f "$OUTZIP"
    /usr/bin/ditto -c -k --keepParent "$OUT" "$OUTZIP"
    echo "Zipped: $OUTZIP"
fi

echo ""
echo "Built: $OUT"
echo "Run it:  open \"$OUT\""
