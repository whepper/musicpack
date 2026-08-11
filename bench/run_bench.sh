#!/usr/bin/env bash
# Repeatable native decoder benchmark.
#
# Runs bench/decode_bench over the corpus with an optional --impl filter,
# records full environment metadata (commit, compiler+version, arch, CPU,
# flags), and appends the results to bench/results/<date>.tsv.
#
# Usage:
#   run_bench.sh <corpus-dir> [--impl scalar|simd] [--iterations N]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
BIN="${MPC_BENCH:-$ROOT/build/bench/decode_bench}"
CORPUS="${1:?corpus dir}"

IMPL=""
ITERATIONS="3"
if [ "${2:-}" = "--impl" ]; then IMPL="$3"; ITERATIONS="${4:-$ITERATIONS}"; fi

[ -x "$BIN" ] || { echo "decode_bench not found at $BIN (build with -DMPC_BUILD_TESTS=ON)"; exit 1; }

RESULTS_DIR="$BENCH/results"
mkdir -p "$RESULTS_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULTS="$RESULTS_DIR/$STAMP.tsv"

# ---- metadata record ------------------------------------------------------
{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "# compiler: $(cc --version 2>/dev/null | head -1 || clang --version | head -1)"
  if [ "$(uname -s)" = "Darwin" ]; then
    echo "# cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
  else
    echo "# cpu: $(grep -m1 "model name" /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ //' || echo unknown)"
  fi
  echo "# arch: $(uname -m)"
  echo "# impl: ${IMPL:-auto}"
  echo "# iterations: $ITERATIONS"
  echo "# files: $(find "$CORPUS" -name '*.mpc' | wc -l | tr -d ' ')"
  echo "# columns: file sample_rate channels audio_s wall_ms cpu_ms realtime_x impl"
} | tee "$RESULTS"

# ---- run ------------------------------------------------------------------
FILES=( "$CORPUS"/corpus/*.mpc "$CORPUS"/fixtures/*.mpc )
ARGS=(--iterations "$ITERATIONS")
if [ -n "$IMPL" ]; then ARGS+=(--impl "$IMPL"); fi

if [ -n "$IMPL" ]; then
  "$BIN" "${ARGS[@]}" "${FILES[@]}" | tee -a "$RESULTS"
else
  echo "# --- scalar ---" | tee -a "$RESULTS"
  "$BIN" "${ARGS[@]}" --impl scalar "${FILES[@]}" | tee -a "$RESULTS"
  echo "# --- simd ---" | tee -a "$RESULTS"
  "$BIN" "${ARGS[@]}" --impl simd "${FILES[@]}" | tee -a "$RESULTS"
fi

echo "results appended to $RESULTS"
