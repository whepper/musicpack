#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Audit the standalone MusicPack Author .app for the distribution contract:
#
#   MusicPack Author.app/
#     Contents/MacOS/musicpack          (static backend)
#     Contents/MacOS/mpcenc             (static Musepack encoder)
#     Contents/MacOS/musicpack-sonic    (Sonic analyzer)
#     Contents/Frameworks/libonnxruntime*.dylib  (relocatable ONNX Runtime)
#
# Fails the build when:
#   - any of the required pieces is missing,
#   - a dependency resolves to Homebrew / /usr/local / another external path,
#   - the analyzer carries an absolute rpath or an absolute ONNX Runtime
#     load path (must be @rpath/@loader_path-relocatable),
#   - an architecture does not match the app host build.
#   - FFmpeg is accidentally included: releases intentionally use a configured
#     or common-location external installation, not a bundled dependency tree.
#
# Usage: scripts/audit-author-macos.sh <MusicPack Author.app>

set -euo pipefail

APP="${1:?path to MusicPack Author.app}"
[ -d "$APP" ] || { echo "fail: $APP does not exist" >&2; exit 1; }
OTOOL="$(command -v otool || echo /usr/bin/otool)"

MACOS="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"
BACKEND="$MACOS/musicpack"
MPCENC="$MACOS/mpcenc"
SONIC="$MACOS/musicpack-sonic"

echo "== required pieces =="
for f in "$BACKEND" "$MPCENC" "$SONIC"; do
  [ -f "$f" ] || { echo "fail: missing $f" >&2; exit 1; }
  [ -x "$f" ] || { echo "fail: not executable $f" >&2; exit 1; }
  echo "ok: $(basename "$f")"
done

echo "== FFmpeg distribution policy =="
if find "$APP/Contents" -type f -name 'ffmpeg*' -print -quit | grep -q .; then
  echo "fail: FFmpeg must not be bundled (the approved strategy is external deterministic discovery)" >&2
  exit 1
fi
echo "ok: no FFmpeg binary is bundled"

# The analyzer must have a bundled ONNX Runtime dylib.
ONNX_DYLIB="$(ls "$FRAMEWORKS"/libonnxruntime*.dylib 2>/dev/null || true)"
if [ -z "$ONNX_DYLIB" ]; then
  echo "fail: no ONNX Runtime dylib in $FRAMEWORKS" >&2
  exit 1
fi
echo "ok: $(basename "$ONNX_DYLIB")"

# Expected host architecture comes from the app's main executable.
ARCH="$(file "$MACOS/musicpack-author" 2>/dev/null | grep -oE 'x86_64|arm64' | head -1 || true)"
[ -n "$ARCH" ] || ARCH="$(uname -m)"
echo "host architecture: $ARCH"

echo "== architecture consistency =="
for f in "$BACKEND" "$MPCENC" "$SONIC" "$ONNX_DYLIB"; do
  a="$(file "$f" | grep -oE 'x86_64|arm64' | head -1 || true)"
  if [ "$a" != "$ARCH" ]; then
    echo "fail: $(basename "$f") architecture '$a' does not match host '$ARCH'" >&2
    exit 1
  fi
  echo "ok: $(basename "$f") is $a"
done

echo "== dependency audit (otool -L) =="
FAIL=0
check_deps() {
  local name="$1" file="$2"
  local deps
  deps="$("$OTOOL" -L "$file" | sed '1d' | awk '{print $1}')"
  while IFS= read -r dep; do
    case "$dep" in
      /usr/lib/* | /System/Library/*) : ;;
      @rpath/* | @loader_path/* | @executable_path/*)
        # @rpath/@loader_path deps must be satisfied inside the bundle; the
        # analyzer's ONNX Runtime reference is the expected case.
        ;;
      *)
        echo "error: $name depends on external path: $dep" >&2
        FAIL=1
        ;;
    esac
    if printf '%s' "$dep" | grep -Eq '/opt/homebrew|/usr/local'; then
      echo "error: $name references a Homebrew/local path: $dep" >&2
      FAIL=1
    fi
  done <<< "$deps"
}

check_deps musicpack "$BACKEND"
check_deps mpcenc "$MPCENC"
check_deps musicpack-sonic "$SONIC"
check_deps onnxruntime "$ONNX_DYLIB"
if [ "$FAIL" -ne 0 ]; then
  echo "fail: external dependency references found" >&2
  exit 1
fi
echo "ok: all dependencies are system or bundle-contained"

echo "== analyzer must depend only on ONNX Runtime via @rpath =="
RPATH_DEPS="$("$OTOOL" -L "$SONIC" | sed '1d' | awk '{print $1}' | grep '^@rpath/' || true)"
RPATH_COUNT="$(printf '%s\n' "$RPATH_DEPS" | grep -c '^@rpath/' || true)"
if [ "$RPATH_COUNT" -ne 1 ]; then
  echo "fail: expected exactly one @rpath dependency in musicpack-sonic:" >&2
  echo "$RPATH_DEPS" >&2
  exit 1
fi
case "$RPATH_DEPS" in
  @rpath/libonnxruntime*) echo "ok: analyzer @rpath dependency is $RPATH_DEPS" ;;
  *) echo "fail: unexpected @rpath dependency: $RPATH_DEPS" >&2; exit 1 ;;
esac

echo "== analyzer load path + rpath audit (otool -l) =="
# The ONNX Runtime load path must be relocatable (@rpath/...).
LOAD="$("$OTOOL" -L "$SONIC" | sed '1d' | grep -E 'onnxruntime' | awk '{print $1}' || true)"
case "$LOAD" in
  @rpath/*) echo "ok: analyzer loads ONNX Runtime via $LOAD" ;;
  *)
    echo "fail: analyzer ONNX Runtime load path is not relocatable: $LOAD" >&2
    exit 1
    ;;
esac

# No absolute rpaths.
if "$OTOOL" -l "$SONIC" | awk '/LC_RPATH/{getline; getline; print $2}' | grep -E '^/' >/dev/null; then
  echo "fail: analyzer carries an absolute rpath:" >&2
  "$OTOOL" -l "$SONIC" | grep -A2 LC_RPATH >&2
  exit 1
fi
echo "ok: analyzer rpaths are relocatable"

# The dylib's own install name must be relocatable and present by that name.
ID="$("$OTOOL" -D "$ONNX_DYLIB" | tail -1)"
case "$ID" in
  @rpath/*) : ;;
  *) echo "fail: ONNX Runtime install name is not relocatable: $ID" >&2; exit 1 ;;
esac
BASE="${ID#@rpath/}"
if [ ! -f "$FRAMEWORKS/$BASE" ]; then
  echo "fail: ONNX Runtime install name '$ID' does not resolve inside the bundle" >&2
  exit 1
fi
echo "ok: ONNX Runtime install name $ID resolves in Frameworks"

echo
echo "MusicPack Author.app packaging audit passed"
