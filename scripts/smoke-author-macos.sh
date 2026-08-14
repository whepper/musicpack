#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Smoke test for the standalone MusicPack Author application bundle.
#
# Verifies:
#   - the .app bundle exists,
#   - the bundled backend exists and is executable,
#   - the backend runs from its bundled location and reports the expected
#     author API version,
#   - the backend references only macOS system libraries (no Homebrew/local),
#   - a harmless structured backend operation succeeds using the bundled CLI.
#   - external FFmpeg discovery can execute from an absolute common location
#     with Finder's minimal PATH (when FFmpeg is installed there).
#
# Usage: scripts/smoke-author-macos.sh <MusicPack Author.app> <repo-root>

set -euo pipefail

APP="${1:?path to MusicPack Author.app}"
ROOT="${2:?repository root}"

[ -d "$APP" ] || { echo "fail: $APP does not exist" >&2; exit 1; }
echo "ok: .app exists at $APP"

MACOS="$APP/Contents/MacOS"
BACKEND="$MACOS/musicpack"
[ -f "$BACKEND" ] || { echo "fail: bundled backend missing at $BACKEND" >&2; exit 1; }
[ -x "$BACKEND" ] || { echo "fail: bundled backend not executable" >&2; exit 1; }
echo "ok: bundled backend present and executable"

echo "== backend capability handshake =="
API="$("$BACKEND" author-api-version --json 2>/dev/null)"
echo "$API"
echo "$API" | grep -q '"authorApi":[[:space:]]*2' \
  || { echo "fail: backend does not report author API 2" >&2; exit 1; }
echo "$API" | grep -q '"musicpackVersion"' \
  || { echo "fail: backend does not report a musicpack version" >&2; exit 1; }
echo "ok: author-API handshake matches"

echo "== runtime dependency check =="
"$ROOT/scripts/verify-backend-dylibs.sh" "$BACKEND"

MPCENC="$MACOS/mpcenc"
[ -f "$MPCENC" ] || { echo "fail: bundled mpcenc missing at $MPCENC" >&2; exit 1; }
[ -x "$MPCENC" ] || { echo "fail: bundled mpcenc not executable" >&2; exit 1; }
"$ROOT/scripts/verify-backend-dylibs.sh" "$MPCENC"
echo "ok: bundled mpcenc present, executable and static"

echo "== FFmpeg minimal-PATH discovery =="
FFMPEG=""
for candidate in /opt/homebrew/bin/ffmpeg /usr/local/bin/ffmpeg /opt/local/bin/ffmpeg; do
  if [ -x "$candidate" ]; then
    FFMPEG="$candidate"
    break
  fi
done
if [ -n "$FFMPEG" ]; then
  env -i PATH=/usr/bin:/bin "$FFMPEG" -version >/dev/null
  echo "ok: $FFMPEG runs with Finder-like PATH"
else
  echo "skip: no common-location ffmpeg installed; configure MUSICPACK_FFMPEG to encode"
fi

SONIC="$MACOS/musicpack-sonic"
[ -f "$SONIC" ] || { echo "fail: bundled sonic analyzer missing at $SONIC" >&2; exit 1; }
[ -x "$SONIC" ] || { echo "fail: bundled sonic analyzer not executable" >&2; exit 1; }
echo "ok: bundled sonic analyzer present and executable"

echo "== sonic analyzer runs and reports typed model state =="
JOB="$(mktemp)"
trap 'rm -f "$JOB"' EXIT
cat > "$JOB" <<EOF
{"profile":"musicpack-sonic-openl3-v1","modelDir":"/nonexistent","cacheDir":"/tmp","outPath":"/tmp/sonic.json","tracks":[{"disc":1,"track":1,"path":"/tmp/does-not-exist.wav"}]}
EOF
OUT="$("$SONIC" "$JOB" 2>/dev/null || true)"
echo "$OUT"
echo "$OUT" | grep -q '"code":"MODEL_MISSING"' \
  || { echo "fail: analyzer did not report MODEL_MISSING" >&2; exit 1; }
echo "ok: analyzer binary loads and reports MODEL_MISSING (no model installed)"

echo "== harmless structured operation (verify a reference package) =="
PKG="$ROOT/tests/reference/test-musicpack-album.mpack"
[ -d "$PKG" ] || PKG="$(find "$ROOT/tests/reference" -maxdepth 1 -name '*.mpack' -type d | head -1)"
if [ -z "$PKG" ] || [ ! -d "$PKG" ]; then
  echo "skip: no reference package to verify"
else
  "$BACKEND" verify "$PKG" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true' \
    || { echo "fail: bundled backend could not verify the reference package" >&2; exit 1; }
  echo "ok: bundled backend verified the reference package"
fi

echo
echo "MusicPack Author.app smoke test passed"
