#!/usr/bin/env bash
# Build the standalone MusicPack Author macOS application bundle.
#
#   scripts/build-author-macos.sh
#
# Produces:
#   author/src-tauri/target/release/bundle/macos/MusicPack Author.app
#
# The standalone app embeds a fully static `musicpack` backend, the
# `musicpack-sonic` analyzer, a fully static `mpcenc` encoder, and a
# relocatable ONNX Runtime dylib so it needs nothing but macOS system
# libraries at runtime:
#
#   MusicPack Author.app
#     ├── Contents/MacOS/musicpack           (static backend)
#     ├── Contents/MacOS/mpcenc              (static Musepack encoder)
#     ├── Contents/MacOS/musicpack-sonic     (Sonic analyzer)
#     └── Contents/Frameworks/libonnxruntime*.dylib
#
# FFmpeg (FLAC/WAV decode) is intentionally NOT bundled. The locally installed
# Homebrew build is non-redistributable (--enable-nonfree) and has a large
# Homebrew dylib closure. The packaged app uses MUSICPACK_FFMPEG or fixed
# Homebrew/MacPorts locations, so Finder's minimal PATH is irrelevant.
#
# ONNX Runtime is downloaded from a pinned immutable release asset and
# checksum-verified (arm64 → 1.28.0; x86_64 → 1.23.0, the last Intel macOS
# release). The analyzer's rpath is rewritten to be relocatable
# (@loader_path/../Frameworks). The build fails on any missing piece, external
# dependency, absolute rpath or mixed architecture.
#
# Requirements: Xcode CLT, CMake >= 3.16, Node.js + npm, the Rust toolchain,
# curl. Set SONIC_ONNXRUNTIME_DIR to skip the pinned download.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
AUTHOR_DIR="$ROOT/author"
BUILD_DIR="${MUSICPACK_AUTHOR_BUILD_DIR:-$ROOT/build-author}"
CONFIG="${CONFIG:-Release}"

HOST="$(rustc -vV | sed -n 's/^host: //p')"
case "$HOST" in
  aarch64-apple-darwin)
    TRIPLE="aarch64-apple-darwin"
    ONNX_VERSION="1.28.0"
    ONNX_ASSET="onnxruntime-osx-arm64-1.28.0.tgz"
    ONNX_SHA="1268b359718099bde2cedb55787f182a130067bc4f31e8c88478c445b850d3d8"
    ONNX_ARCH="arm64"
    ;;
  x86_64-apple-darwin)
    TRIPLE="x86_64-apple-darwin"
    ONNX_VERSION="1.23.0"
    ONNX_ASSET="onnxruntime-osx-x86_64-1.23.0.tgz"
    ONNX_SHA="a8e43edcaa349cbfc51578a7fc61ea2b88793ccf077b4bc65aca58999d20cf0f"
    ONNX_ARCH="x86_64"
    ;;
  *)
    echo "error: unsupported host '$HOST' (this script is macOS-only)" >&2
    exit 1
    ;;
esac

echo "== 0. ONNX Runtime ($ONNX_ARCH $ONNX_VERSION) =="
if [ -n "${SONIC_ONNXRUNTIME_DIR:-}" ]; then
  ONNX_DIR="$SONIC_ONNXRUNTIME_DIR"
  [ -f "$ONNX_DIR/lib/libonnxruntime.dylib" ] || {
    echo "error: SONIC_ONNXRUNTIME_DIR has no lib/libonnxruntime.dylib" >&2
    exit 1
  }
  echo "using SONIC_ONNXRUNTIME_DIR=$ONNX_DIR"
else
  ONNX_DIR="$BUILD_DIR/onnxruntime/onnxruntime-osx-$ONNX_ARCH-$ONNX_VERSION"
  if [ ! -f "$ONNX_DIR/lib/libonnxruntime.dylib" ]; then
    TARBALL="$BUILD_DIR/onnxruntime/$ONNX_ASSET"
    mkdir -p "$BUILD_DIR/onnxruntime"
    if [ ! -f "$TARBALL" ]; then
      curl -sL -o "$TARBALL" \
        "https://github.com/microsoft/onnxruntime/releases/download/v$ONNX_VERSION/$ONNX_ASSET"
    fi
    echo "$ONNX_SHA  $TARBALL" | shasum -a 256 -c - >/dev/null || {
      echo "error: ONNX Runtime tarball checksum mismatch" >&2
      exit 1
    }
    tar xzf "$TARBALL" -C "$BUILD_DIR/onnxruntime"
  fi
  echo "onnxruntime $ONNX_VERSION at $ONNX_DIR"
fi

echo "== 1. building the static musicpack backend ($HOST) =="
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DMPC_BUILD_SHARED=OFF \
  -DMPC_BUILD_TESTS=OFF \
  -DMPC_BUILD_MPCGAIN=OFF \
  -DMPC_BUILD_MPCCHAP=OFF \
  -DSONIC_ONNXRUNTIME_DIR="$ONNX_DIR" \
  -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" -j --target musicpack_cmd musicpack_sonic_cmd mpcenc

BACKEND="$BUILD_DIR/musicpack/musicpack"
SONIC_BIN="$BUILD_DIR/sonic/musicpack-sonic"
MPCENC_BIN="$BUILD_DIR/mpcenc/mpcenc"
[ -x "$BACKEND" ] || { echo "error: backend not produced at $BACKEND" >&2; exit 1; }
[ -x "$SONIC_BIN" ] || { echo "error: sonic analyzer not produced at $SONIC_BIN" >&2; exit 1; }
[ -x "$MPCENC_BIN" ] || { echo "error: mpcenc not produced at $MPCENC_BIN" >&2; exit 1; }

echo "== 2. verifying runtime dependencies =="
"$ROOT/scripts/verify-backend-dylibs.sh" "$BACKEND"
# The analyzer may only depend on the ONNX Runtime dylib plus system libs.
if otool -L "$SONIC_BIN" | grep -Eq '/opt/homebrew|/usr/local'; then
  echo "error: musicpack-sonic references a Homebrew/local path" >&2
  exit 1
fi
# mpcenc (the encoder sidecar) must be fully static like the backend.
"$ROOT/scripts/verify-backend-dylibs.sh" "$MPCENC_BIN"

echo "== 3. staging the Tauri sidecars =="
SIDECAR_DIR="$AUTHOR_DIR/src-tauri/binaries"
mkdir -p "$SIDECAR_DIR"
cp "$BACKEND" "$SIDECAR_DIR/musicpack-$TRIPLE"
chmod +x "$SIDECAR_DIR/musicpack-$TRIPLE"
cp "$MPCENC_BIN" "$SIDECAR_DIR/mpcenc-$TRIPLE"
chmod +x "$SIDECAR_DIR/mpcenc-$TRIPLE"

# Stage the analyzer and rewrite it to be relocatable: remove the dev build's
# absolute rpath and point @rpath at the bundle Frameworks directory. The
# ONNX Runtime load path is already @rpath (the dylib install name).
STAGED_SONIC="$SIDECAR_DIR/musicpack-sonic-$TRIPLE"
cp "$SONIC_BIN" "$STAGED_SONIC"
chmod +x "$STAGED_SONIC"
install_name_tool -delete_rpath "$ONNX_DIR/lib" "$STAGED_SONIC"
install_name_tool -add_rpath "@loader_path/../Frameworks" "$STAGED_SONIC"
# Re-seal the ad-hoc signature invalidated by install_name_tool.
if command -v codesign >/dev/null; then
  codesign -s - -f "$STAGED_SONIC"
fi

echo "== 4. building the Tauri application =="
(
  cd "$AUTHOR_DIR"
  npm ci
  npm run tauri build
)

APP="$AUTHOR_DIR/src-tauri/target/release/bundle/macos/MusicPack Author.app"
[ -d "$APP" ] || { echo "error: bundle not produced at $APP" >&2; exit 1; }

echo "== 5. bundling ONNX Runtime =="
# The real dylib, named after its install name so @rpath resolves inside the
# bundle (Frameworks/libonnxruntime*.dylib). cp follows the release symlink.
ONNX_DYLIB="$ONNX_DIR/lib/libonnxruntime.dylib"
ID="$(otool -D "$ONNX_DYLIB" | tail -1)"
case "$ID" in
  @rpath/*) : ;;
  *) echo "error: ONNX Runtime install name is not relocatable: $ID" >&2; exit 1 ;;
esac
mkdir -p "$APP/Contents/Frameworks"
cp "$ONNX_DYLIB" "$APP/Contents/Frameworks/$(basename "$ID")"
if command -v codesign >/dev/null; then
  codesign -s - -f "$APP/Contents/Frameworks/$(basename "$ID")"
  # Re-seal the whole bundle after modifying its contents.
  codesign -s - -f "$APP"
fi

echo "== 6. packaging audit =="
"$ROOT/scripts/audit-author-macos.sh" "$APP"

echo "== 7. smoke test =="
"$ROOT/scripts/smoke-author-macos.sh" "$APP" "$ROOT"

echo
echo "Built: $APP"
