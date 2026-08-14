#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${MPC_PSY_FFT_BENCH:-$ROOT/build/bench/psy_fft_bench}"
ITERATIONS="${1:-1000}"
RESULTS_DIR="$ROOT/bench/results"
RESULTS="$RESULTS_DIR/psy-fft-$(date +%Y%m%d-%H%M%S).tsv"
BUILD="${MPC_BENCH_BUILD:-$ROOT/build}"

[ -x "$BIN" ] || { echo "psy_fft_bench not found at $BIN" >&2; exit 1; }
mkdir -p "$RESULTS_DIR"
{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  python3 "$ROOT/scripts/ci_config.py" --build "$BUILD" --role benchmark-psy-fft \
    --selected-config Release --executable "$BIN" --format metadata
  echo "# iterations: $ITERATIONS"
  echo "# workload: deterministic q5/q6/q7 scalar|simd kernel mix"
  for quality in 5 6 7; do
    for impl in scalar simd; do
      "$BIN" --quality "$quality" --impl "$impl" --iterations "$ITERATIONS"
    done
  done
} | tee "$RESULTS"
