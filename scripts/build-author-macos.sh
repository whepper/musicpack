#!/usr/bin/env bash
# Build the standalone MusicPack Author macOS application bundle.
#
#   scripts/build-author-macos.sh
#
# Produces:
#   author/src-tauri/target/release/bundle/macos/MusicPack Author.app
#
# The standalone app embeds a fully static `musicpack` backend (built with
# -DMPC_BUILD_SHARED=OFF) so it needs nothing but macOS system libraries at
# runtime. This script:
#   1. builds the static backend,
#   2. verifies it depends only on system dylibs,
#   3. stages it as the Tauri sidecar,
#   4. builds the Svelte frontend + the Tauri release bundle,
#   5. smoke-tests the resulting .app.
#
# Requirements: Xcode CLT, CMake >= 3.16, Node.js + npm, the Rust toolchain.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AUTHOR_DIR="$ROOT/author"
BUILD_DIR="${MUSICPACK_AUTHOR_BUILD_DIR:-$ROOT/build-author}"
CONFIG="${CONFIG:-Release}"

HOST="$(rustc -vV | sed -n 's/^host: //p')"
case "$HOST" in
  aarch64-apple-darwin) TRIPLE="aarch64-apple-darwin" ;;
  x86_64-apple-darwin) TRIPLE="x86_64-apple-darwin" ;;
  *)
    echo "error: unsupported host '$HOST' (this script is macOS-only)" >&2
    exit 1
    ;;
esac

echo "== 1. building the static musicpack backend ($HOST) =="
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DMPC_BUILD_SHARED=OFF \
  -DMPC_BUILD_TESTS=OFF \
  -DMPC_BUILD_MPCGAIN=OFF \
  -DMPC_BUILD_MPCCHAP=OFF \
  -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" -j --target musicpack_cmd

BACKEND="$BUILD_DIR/musicpack/musicpack"
[ -x "$BACKEND" ] || { echo "error: backend not produced at $BACKEND" >&2; exit 1; }

echo "== 2. verifying backend runtime dependencies =="
"$ROOT/scripts/verify-backend-dylibs.sh" "$BACKEND"

echo "== 3. staging the Tauri sidecar =="
SIDECAR_DIR="$AUTHOR_DIR/src-tauri/binaries"
mkdir -p "$SIDECAR_DIR"
cp "$BACKEND" "$SIDECAR_DIR/musicpack-$TRIPLE"
chmod +x "$SIDECAR_DIR/musicpack-$TRIPLE"

echo "== 4. building the Tauri application =="
(
  cd "$AUTHOR_DIR"
  npm ci
  npm run tauri build
)

APP="$AUTHOR_DIR/src-tauri/target/release/bundle/macos/MusicPack Author.app"
[ -d "$APP" ] || { echo "error: bundle not produced at $APP" >&2; exit 1; }

echo "== 5. smoke test =="
"$ROOT/scripts/smoke-author-macos.sh" "$APP" "$ROOT"

echo
echo "Built: $APP"
