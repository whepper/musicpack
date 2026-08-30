#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# MPAK HTTP Range transport smoke test: exercises demo/mpakrange.js (the
# browser/WASM range transport) against a deterministic local HTTP server
# with real fetch() — no Emscripten or browser required.
#
# Usage: tests/run_mpakrange_smoke.sh <musicpack-cli> [fixture.mpc]
# Requires node (18+).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${1:?usage: run_mpakrange_smoke.sh <musicpack-cli> [fixture.mpc]}"
FIXTURE="${2:-$ROOT/tests/fixtures/sine44-q5.mpc}"

[ -x "$CLI" ] || { echo "FAIL  musicpack CLI not executable: $CLI"; exit 1; }
[ -f "$FIXTURE" ] || { echo "FAIL  fixture not found: $FIXTURE"; exit 1; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/mpak-range.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# A container larger than the 256 KiB discovery window exercises the
# 64 KiB block-fetch path (discover -> aligned blocks -> final partial):
# pad the audio member so the packed container spans several blocks.
mkdir -p "$TMP/pkg/audio"
cp "$FIXTURE" "$TMP/pkg/audio/01.mpc"
head -c 300000 /dev/zero >> "$TMP/pkg/audio/01.mpc"
cat > "$TMP/pkg/manifest.json" <<EOF
{
  "album": { "artists": [ { "name": "T" } ], "title": "M" },
  "format": "musicpack",
  "media": [ { "disc": 1, "tracks": [ { "audio": { "path": "audio/01.mpc",
    "sha256": "$(shasum -a 256 "$TMP/pkg/audio/01.mpc" | cut -d' ' -f1)" },
    "title": "t", "track": 1 } ] } ],
  "version": 1
}
EOF
"$CLI" pack "$TMP/pkg" "$TMP/test.mpak" >/dev/null || {
    echo "FAIL  packing the test container"; exit 1; }
if [ "$(stat -f%z "$TMP/test.mpak" 2>/dev/null || stat -c%s "$TMP/test.mpak")" \
        -le 262144 ]; then
    echo "SKIP  container does not exceed the discovery window"
    exit 77
fi

if ! node "$ROOT/wasm/smoke_mpakrange.js" "$TMP/test.mpak"; then
    echo "FAIL  mpakrange transport smoke"
    exit 1
fi
echo "PASS  mpakrange transport smoke"
