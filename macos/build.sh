#!/usr/bin/env bash
# build.sh — Build DSInput.app (DS Input IME for macOS)
#
# Usage: bash macos/build.sh [--debug]
#
# What it does:
#   1. Builds the Rust core (libdsime.a + libdsime.dylib) via cargo.
#   2. Compiles all Swift sources with swiftc, linking the static core library
#      and the required system frameworks.
#   3. Assembles DSInput.app bundle with the compiled binary + Info.plist.
#   4. Prints install / enable instructions.
#
# Static vs dynamic linking:
#   We prefer libdsime.a so the .app needs no separate dylib.  Rust's static
#   archive pulls in several system frameworks transitively (Security,
#   CoreFoundation, SystemConfiguration, libcurl, libresolv, …).  We enumerate
#   them explicitly below; if a new dependency is added to Cargo.toml you may
#   need to add a -framework / -l flag here.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CORE_DIR="$REPO_ROOT/core"
MACOS_DIR="$SCRIPT_DIR"
SOURCES_DIR="$MACOS_DIR/Sources/DSInput"
RESOURCES_DIR="$MACOS_DIR/Resources"
BUILD_DIR="$MACOS_DIR/build"
APP_BUNDLE="$BUILD_DIR/DSInput.app"

# ── 0. Parse args ────────────────────────────────────────────────────────────
CARGO_PROFILE="release"
SWIFT_OPT="-O"
if [[ "${1:-}" == "--debug" ]]; then
    CARGO_PROFILE="dev"
    SWIFT_OPT="-Onone -g"
fi

echo "==> Build profile: $CARGO_PROFILE"

# ── 1. Build Rust core ───────────────────────────────────────────────────────
echo "==> Building Rust core (profile=$CARGO_PROFILE)…"
(cd "$CORE_DIR" && cargo build --profile "$CARGO_PROFILE")

if [[ "$CARGO_PROFILE" == "release" ]]; then
    CORE_LIB_DIR="$CORE_DIR/target/release"
else
    CORE_LIB_DIR="$CORE_DIR/target/debug"
fi

STATIC_LIB="$CORE_LIB_DIR/libdsime.a"
if [[ ! -f "$STATIC_LIB" ]]; then
    echo "ERROR: Static library not found at $STATIC_LIB" >&2
    exit 1
fi
echo "    libdsime.a: $STATIC_LIB"

# ── 2. Prepare build directory ───────────────────────────────────────────────
echo "==> Preparing build directory…"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# ── 3. Compile Swift sources ─────────────────────────────────────────────────
echo "==> Compiling Swift sources…"

SWIFT_SOURCES=(
    "$SOURCES_DIR/AppDelegate.swift"
    "$SOURCES_DIR/DSInputController.swift"
    "$SOURCES_DIR/PreferencesWindowController.swift"
)

BRIDGING_HEADER="$SOURCES_DIR/dsime_bridge.h"

# macOS deployment target.  We match the SDK version to avoid linker warnings
# about object files built for a newer OS than the deployment target.
# 13.0 would work at runtime, but the Rust static lib is compiled against the
# host SDK (26.x), so we set the deployment target to match.
MACOS_TARGET="$(xcrun --sdk macosx --show-sdk-version 2>/dev/null | cut -d. -f1-2)"
: "${MACOS_TARGET:=13.0}"
echo "    macOS target: $MACOS_TARGET"

# System frameworks needed by the Rust static archive + our own code.
# - InputMethodKit / AppKit / Carbon: IME infrastructure
# - Security / CoreFoundation / SystemConfiguration: pulled in by reqwest/rustls
# - libc++ / libiconv / libresolv: runtime support
EXTRA_LINK_FLAGS=(
    # Rust runtime
    "-lc++"
    "-liconv"
    # System frameworks: reqwest → rustls → Security
    "-framework" "Security"
    "-framework" "CoreFoundation"
    "-framework" "SystemConfiguration"
    # Networking
    "-framework" "CFNetwork"
    # Standard resolver
    "-lresolv"
    # IME + UI
    "-framework" "InputMethodKit"
    "-framework" "AppKit"
    "-framework" "Carbon"
)

BINARY="$BUILD_DIR/DSInput"

swiftc \
    "${SWIFT_SOURCES[@]}" \
    -import-objc-header "$BRIDGING_HEADER" \
    -module-name DSInput \
    -target "arm64-apple-macos${MACOS_TARGET}" \
    $SWIFT_OPT \
    -Xcc "-I$CORE_DIR/include" \
    -Xlinker -force_load \
    -Xlinker "$STATIC_LIB" \
    "${EXTRA_LINK_FLAGS[@]}" \
    -o "$BINARY"

echo "    Binary: $BINARY"

# ── 4. Assemble .app bundle ──────────────────────────────────────────────────
echo "==> Assembling DSInput.app…"

CONTENTS="$APP_BUNDLE/Contents"
MACOS_IN_APP="$CONTENTS/MacOS"
RESOURCES_IN_APP="$CONTENTS/Resources"

mkdir -p "$MACOS_IN_APP"
mkdir -p "$RESOURCES_IN_APP"

cp "$BINARY"                    "$MACOS_IN_APP/DSInput"
cp "$RESOURCES_DIR/Info.plist"  "$CONTENTS/Info.plist"

echo "    Bundle: $APP_BUNDLE"

# ── 5. Code-sign (ad-hoc) ────────────────────────────────────────────────────
echo "==> Ad-hoc signing…"
codesign --force --sign - "$APP_BUNDLE" || true

# ── 6. Done ──────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║  DSInput.app built successfully!                                ║"
echo "╠══════════════════════════════════════════════════════════════════╣"
echo "║  Install:                                                        ║"
echo "║    cp -R $APP_BUNDLE ~/Library/Input\\ Methods/ "
echo "║                                                                  ║"
echo "║  Enable in System Settings:                                      ║"
echo "║    System Settings ▸ Keyboard ▸ Input Sources ▸ (+)             ║"
echo "║    Search for 'DS Input' (or Chinese → DS Input)                 ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
