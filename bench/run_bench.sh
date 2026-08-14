#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Repeatable native decoder benchmark.
#
# Runs bench/decode_bench over the corpus with an optional --impl filter,
# records full environment metadata and independent repetitions in
# bench/results/<date>.tsv.
#
# Usage:
#   run_bench.sh <corpus-dir> [--impl scalar|simd] [--iterations N] [--runs N]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
BIN="${MPC_BENCH:-$ROOT/build/bench/decode_bench}"
CORPUS="${1:?corpus dir}"

IMPL=""
ITERATIONS="3"
RUNS="3"
shift
while [ "$#" -gt 0 ]; do
  case "$1" in
    --impl|--iterations|--runs)
      [ "$#" -ge 2 ] || { echo "missing value for $1"; exit 2; }
      case "$1" in
        --impl) IMPL="$2" ;;
        --iterations) ITERATIONS="$2" ;;
        --runs) RUNS="$2" ;;
      esac
      shift 2 ;;
    --impl=*) IMPL="${1#--impl=}"; shift ;;
    --iterations=*) ITERATIONS="${1#--iterations=}"; shift ;;
    --runs=*) RUNS="${1#--runs=}"; shift ;;
    *) echo "unknown option: $1"; exit 2 ;;
  esac
done

[[ "$ITERATIONS" =~ ^[1-9][0-9]*$ ]] || { echo "invalid --iterations: $ITERATIONS"; exit 2; }
[[ "$RUNS" =~ ^[1-9][0-9]*$ ]] || { echo "invalid --runs: $RUNS"; exit 2; }
if [ -n "$IMPL" ] && [ "$IMPL" != scalar ] && [ "$IMPL" != simd ]; then
  echo "invalid --impl: $IMPL"
  exit 2
fi

[ -x "$BIN" ] || { echo "decode_bench not found at $BIN (build with -DMPC_BUILD_TESTS=ON)"; exit 1; }

RESULTS_DIR="$BENCH/results"
mkdir -p "$RESULTS_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULTS="$RESULTS_DIR/$STAMP.tsv"
SUMMARY="$RESULTS_DIR/$STAMP-summary.tsv"
CORPUS_HASH="$(python3 - "$CORPUS" <<'PY'
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve()
h = hashlib.sha256()
for path in sorted(root.rglob("*.mpc"), key=lambda p: p.relative_to(root).as_posix()):
    rel = path.relative_to(root).as_posix().encode()
    h.update(len(rel).to_bytes(4, "big")); h.update(rel)
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
print(h.hexdigest())
PY
)"
# ---- metadata record ------------------------------------------------------
{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  python3 "$ROOT/scripts/ci_config.py" --build "${MPC_BENCH_BUILD:-$ROOT/build}" \
    --role benchmark-native --selected-config Release --executable "$BIN" --corpus "$CORPUS" --format metadata
  echo "# impl: ${IMPL:-auto}"
  echo "# iterations: $ITERATIONS"
  echo "# runs: $RUNS"
  echo "# binary_sha256: $(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$BIN")"
  echo "# corpus_mpc_sha256: $CORPUS_HASH"
  if [ -n "${MPC_BENCH_BUILD:-}" ] && [ -f "$MPC_BENCH_BUILD/CMakeCache.txt" ]; then
    grep -E '^(CMAKE_BUILD_TYPE|CMAKE_C_COMPILER|CMAKE_C_COMPILER_ID|CMAKE_C_COMPILER_VERSION|CMAKE_C_FLAGS|CMAKE_C_FLAGS_RELEASE|MPC_ENABLE_SIMD|MPC_ENABLE_NATIVE_TUNING):' "$MPC_BENCH_BUILD/CMakeCache.txt" | sed 's/^/# cmake: /'
  fi
  echo "# files: $(find "$CORPUS" -name '*.mpc' | wc -l | tr -d ' ')"
  echo "# columns: file sample_rate channels audio_s wall_ms cpu_ms realtime_x impl run"
} | tee "$RESULTS"

# ---- run ------------------------------------------------------------------
FILES=( "$CORPUS"/corpus/*.mpc "$CORPUS"/fixtures/*.mpc )
ARGS=(--iterations "$ITERATIONS")
if [ -n "$IMPL" ]; then ARGS+=(--impl "$IMPL"); fi

for ((run = 1; run <= RUNS; run++)); do
  echo "# --- run $run ---" | tee -a "$RESULTS"
  if [ -n "$IMPL" ]; then
    "$BIN" "${ARGS[@]}" "${FILES[@]}" | awk -v r="$run" 'BEGIN{OFS="\t"}{print $0,r}' | tee -a "$RESULTS"
  elif ((run % 2)); then
    "$BIN" "${ARGS[@]}" --impl scalar "${FILES[@]}" | awk -v r="$run" 'BEGIN{OFS="\t"}{print $0,r}' | tee -a "$RESULTS"
    "$BIN" "${ARGS[@]}" --impl simd "${FILES[@]}" | awk -v r="$run" 'BEGIN{OFS="\t"}{print $0,r}' | tee -a "$RESULTS"
  else
    "$BIN" "${ARGS[@]}" --impl simd "${FILES[@]}" | awk -v r="$run" 'BEGIN{OFS="\t"}{print $0,r}' | tee -a "$RESULTS"
    "$BIN" "${ARGS[@]}" --impl scalar "${FILES[@]}" | awk -v r="$run" 'BEGIN{OFS="\t"}{print $0,r}' | tee -a "$RESULTS"
  fi
done

python3 "$BENCH/summarize_bench.py" --mode native --expected-runs "$RUNS" \
  "$RESULTS" "$SUMMARY"
echo "raw results: $RESULTS"
echo "summary: $SUMMARY"
