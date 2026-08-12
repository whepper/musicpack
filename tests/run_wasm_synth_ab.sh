#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?wasm build dir}"
FIXTURE="${2:-$ROOT/tests/fixtures/sine44-q5-48s.mpc}"

node "$ROOT/wasm/synth_ab.mjs" "$BUILD/wasm/musepack.js" "$FIXTURE"
