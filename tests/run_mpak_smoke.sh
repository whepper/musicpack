#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# MPAK-over-HTTP WASM smoke test: serves the committed deterministic
# container fixture over HTTP Range, acquires it with the browser transport
# (demo/mpakrange.js) and runs musicpack_package_open_range()/verify/
# decode/seek inside the Emscripten module. Only meaningful in an
# Emscripten build.
#
# Usage: tests/run_mpak_smoke.sh <wasm-build-dir> [container.mpak]
# Requires node. The default fixture (tests/fixtures/mpak-range-test.mpak,
# ~321 KiB — larger than the 256 KiB discovery window so the 64 KiB
# block-fetch path is exercised) is deterministic: regenerate with
#   musicpack pack <padded single-track album dir> tests/fixtures/mpak-range-test.mpak
# (the fixture is the byte-exact packed form of sine44-q5.mpc padded with
# 300000 zero bytes; verified with `musicpack verify`).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WASM_BUILD="${1:?wasm build dir}"
CONTAINER="${2:-$ROOT/tests/fixtures/mpak-range-test.mpak}"

MODULE_JS="$WASM_BUILD/wasm/musepack.js"
[ -f "$MODULE_JS" ] || { echo "FAIL  wasm module not found at $MODULE_JS"; exit 1; }
[ -f "$CONTAINER" ] || { echo "FAIL  container fixture not found: $CONTAINER"; exit 1; }

if ! node "$ROOT/wasm/smoke_mpak.js" "$MODULE_JS" "$CONTAINER"; then
    echo "FAIL  mpak wasm smoke"
    exit 1
fi
echo "PASS  mpak wasm smoke"
