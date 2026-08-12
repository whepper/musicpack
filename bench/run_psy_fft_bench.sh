#!/usr/bin/env bash
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${MPC_PSY_FFT_BENCH:-$ROOT/build/bench/psy_fft_bench}"
ITERATIONS="${1:-1000}"
RESULTS_DIR="$ROOT/bench/results"
RESULTS="$RESULTS_DIR/psy-fft-$(date +%Y%m%d-%H%M%S).tsv"

[ -x "$BIN" ] || { echo "psy_fft_bench not found at $BIN" >&2; exit 1; }
mkdir -p "$RESULTS_DIR"
{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# commit: $(git -C "$ROOT" rev-parse --short HEAD)"
  echo "# arch: $(uname -m)"
  echo "# iterations: $ITERATIONS"
  for quality in 5 6 7; do
    for impl in scalar simd; do
      "$BIN" --quality "$quality" --impl "$impl" --iterations "$ITERATIONS"
    done
  done
} | tee "$RESULTS"
