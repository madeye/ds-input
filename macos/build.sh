#!/usr/bin/env bash
# build.sh — Build DSInput.app (DS Input IME for macOS), Swift frontend.
#
# Usage: bash macos/build.sh [--debug] [--notarize]
#
# What it does:
#   1. Regenerates the Xcode project from project.yml (XcodeGen).
#   2. Builds with xcodebuild (its pre-build phase compiles the Rust core via
#      cargo; the Swift sources link libdsime.a through the bridging header).
#   3. Code-signs with a Developer ID identity + hardened runtime.
#   4. Only with --notarize: notarizes + staples (for distribution). Notarization
#      is NOT needed to register/test the IME locally — see section 5.
#
# Why xcodebuild (not swiftc/clang by hand): the real macOS input methods
# (WeType, vChewing, the system ones) are all Xcode-built — the resulting bundle
# carries CFBundleSupportedPlatforms, the DT* SDK keys, PkgInfo, and a proper
# LC_BUILD_VERSION. A hand-assembled bundle is missing pieces the input-source
# scanner relies on, so we let Xcode produce the bundle.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS_DIR="$SCRIPT_DIR"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CORE_DIR="$REPO_ROOT/core"
BUILD_DIR="$MACOS_DIR/build"
DERIVED_DIR="$BUILD_DIR/DerivedData"
XCODEPROJ="$MACOS_DIR/DSInput.xcodeproj"

# ── 0. Args ──────────────────────────────────────────────────────────────────
#   --debug      build the Debug config (unoptimized, …​.dsinput.debug id)
#   --notarize   notarize + staple (for DISTRIBUTION). Off by default: a locally
#                built/installed bundle carries no quarantine flag, so Gatekeeper
#                never checks notarization — it is NOT needed to register/test the
#                IME locally, only to ship it to other machines.
CONFIG="Release"
NOTARIZE=0
for arg in "$@"; do
    case "$arg" in
        --debug)    CONFIG="Debug" ;;
        --notarize) NOTARIZE=1 ;;
        *) echo "ERROR: unknown arg '$arg' (use --debug / --notarize)." >&2; exit 2 ;;
    esac
done
echo "==> Configuration: $CONFIG   (notarize: $([[ $NOTARIZE == 1 ]] && echo yes || echo no))"

# ── 1. Tooling ───────────────────────────────────────────────────────────────
if ! command -v xcodegen >/dev/null 2>&1; then
    echo "ERROR: xcodegen not found (brew install xcodegen)." >&2
    exit 1
fi

# ── 2. Signing config ────────────────────────────────────────────────────────
# Set these via the environment, or put them in a gitignored `macos/.signing.env`
# (sourced below) so your credentials never land in the repo:
#   SIGN_IDENTITY      Developer ID Application identity, e.g.
#                      "Developer ID Application: Your Name (TEAMID)".
#                      Empty → ad-hoc signing (the IME will NOT register in
#                      System Settings; ad-hoc is launch-only).
#   DEV_TEAM           your 10-char Apple Developer team id (DEVELOPMENT_TEAM).
#   NOTARYTOOL_KEY     path to your App Store Connect API key (.p8).
#   NOTARYTOOL_KEY_ID  the API key id.
#   NOTARYTOOL_ISSUER  the API key issuer id.
[[ -f "$MACOS_DIR/.signing.env" ]] && source "$MACOS_DIR/.signing.env"
SIGN_IDENTITY="${SIGN_IDENTITY:-}"
DEV_TEAM="${DEV_TEAM:-}"
NOTARYTOOL_KEY="${NOTARYTOOL_KEY:-}"
NOTARYTOOL_KEY_ID="${NOTARYTOOL_KEY_ID:-}"
NOTARYTOOL_ISSUER="${NOTARYTOOL_ISSUER:-}"

# ── 3. Regenerate the Xcode project ──────────────────────────────────────────
echo "==> Regenerating Xcode project (xcodegen)…"
( cd "$MACOS_DIR" && xcodegen generate --spec project.yml >/dev/null )

# ── 4. Build with xcodebuild ─────────────────────────────────────────────────
echo "==> Building with xcodebuild…"
rm -rf "$DERIVED_DIR"
XCB_ARGS=(
    -project "$XCODEPROJ"
    -scheme DSInput
    -configuration "$CONFIG"
    -derivedDataPath "$DERIVED_DIR"
)
if [[ -n "$SIGN_IDENTITY" ]]; then
    # CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO (from project.yml) keeps the debug
    # get-task-allow entitlement out of the distribution build, which would
    # otherwise fail notarization. --timestamp is required for notarization.
    XCB_ARGS+=(
        CODE_SIGN_STYLE=Manual
        CODE_SIGN_IDENTITY="$SIGN_IDENTITY"
        DEVELOPMENT_TEAM="$DEV_TEAM"
        OTHER_CODE_SIGN_FLAGS="--timestamp"
    )
else
    echo "==> WARNING: no SIGN_IDENTITY — ad-hoc signing (IME will NOT register)."
    XCB_ARGS+=( CODE_SIGN_IDENTITY="-" CODE_SIGNING_REQUIRED=NO )
fi
xcodebuild "${XCB_ARGS[@]}" build

BUILT_APP="$DERIVED_DIR/Build/Products/$CONFIG/DSInput.app"
if [[ ! -d "$BUILT_APP" ]]; then
    echo "ERROR: build did not produce $BUILT_APP" >&2
    exit 1
fi

# Copy the product to a stable path (build/DSInput.app) for install.sh.
APP_BUNDLE="$BUILD_DIR/DSInput.app"
rm -rf "$APP_BUNDLE"
cp -R "$BUILT_APP" "$APP_BUNDLE"
echo "    Bundle: $APP_BUNDLE"

# Guard: a signed distribution build must not carry get-task-allow (notarization
# rejects it). Only meaningful when we actually sign with a Developer ID.
if [[ -n "$SIGN_IDENTITY" ]] && codesign -d --entitlements - --xml "$APP_BUNDLE" 2>/dev/null \
        | plutil -p - 2>/dev/null | grep -q "get-task-allow.*1"; then
    echo "ERROR: bundle has com.apple.security.get-task-allow — would fail notarization." >&2
    exit 1
fi

# ── 5. Notarize + staple (distribution only) ─────────────────────────────────
# Notarization is for SHIPPING the bundle to other machines (Gatekeeper checks the
# stapled ticket on downloaded, quarantined apps). It is NOT required to register
# or test the IME locally, so it is skipped unless --notarize is passed.
if [[ "$NOTARIZE" != 1 ]]; then
    echo "==> Skipping notarization (local build). Pass --notarize for a release."
elif [[ -z "$SIGN_IDENTITY" ]]; then
    echo "==> Cannot notarize an ad-hoc build (set SIGN_IDENTITY)." >&2
    exit 1
elif [[ -f "$NOTARYTOOL_KEY" ]]; then
    echo "==> Notarizing (uploads to Apple and waits)…"
    NOTARIZE_ZIP="$BUILD_DIR/DSInput-notarize.zip"
    /usr/bin/ditto -c -k --keepParent "$APP_BUNDLE" "$NOTARIZE_ZIP"
    xcrun notarytool submit "$NOTARIZE_ZIP" \
        --key "$NOTARYTOOL_KEY" --key-id "$NOTARYTOOL_KEY_ID" \
        --issuer "$NOTARYTOOL_ISSUER" --wait
    rm -f "$NOTARIZE_ZIP"
    echo "==> Stapling…"
    xcrun stapler staple "$APP_BUNDLE"
    spctl -a -vvv -t exec "$APP_BUNDLE" || true
else
    echo "ERROR: --notarize given but notarytool key not found at $NOTARYTOOL_KEY." >&2
    exit 1
fi

# ── 6. Done ──────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║  DSInput.app built successfully!                                  ║"
echo "╠══════════════════════════════════════════════════════════════════╣"
echo "║  Install:  bash macos/install.sh                                 ║"
echo "║  Then log out / back in and add DS Input under                   ║"
echo "║  System Settings ▸ Keyboard ▸ Input Sources ▸ (+) ▸ Simplified.  ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
