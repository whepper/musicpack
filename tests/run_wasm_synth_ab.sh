#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:?wasm build dir}"
FIXTURE="${2:-$ROOT/tests/fixtures/sine44-q5-48s.mpc}"

node "$ROOT/wasm/synth_ab.mjs" "$BUILD/wasm/musepack.js" "$FIXTURE"
