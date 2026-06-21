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

# Signing: set via env or a gitignored `macos/.signing.env` (sourced here).
# Empty SIGN_IDENTITY → ad-hoc (fine to run locally; not for distribution).
[[ -f "$MACOS_DIR/.signing.env" ]] && source "$MACOS_DIR/.signing.env"
SIGN_IDENTITY="${SIGN_IDENTITY:-}"
DEV_TEAM="${DEV_TEAM:-}"

# ── 1. Build the IME (skips notarization; not needed for local install) ───────
echo "==> Building DSInput.app (the IME to embed)…"
NOTARYTOOL_KEY="${NOTARYTOOL_KEY:-/nonexistent/key.p8}" bash "$MACOS_DIR/build.sh"
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

OUT="$BUILD_DIR/DSInputInstaller.app"
rm -rf "$OUT"
cp -R "$INSTALLER_APP" "$OUT"

echo ""
echo "Built: $OUT"
echo "Run it:  open \"$OUT\""
