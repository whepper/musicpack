#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Repeatable Phase 3 psychoacoustic encoder benchmark.
#
# Times the mpcenc CLI (wall clock) for each corpus WAV x quality, for both
# the scalar and SIMD psychoacoustic implementations (--psy-impl), and
# reports the realtime multiplier (audio_seconds / encode_wall_seconds) and
# output size. Records full metadata (commit, compiler/version, arch, CPU,
# flags) and appends a TSV to bench/results/.
#
# Usage:
#   encode_bench.sh <corpus-wav-dir> <mpcenc> [--qualities 5,6,7] [--runs N]
#
# Output rows: file quality psy_impl audio_s median_wall_ms realtime_x bytes
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
CORPUS="${1:?corpus wav dir}"
MPCENC="${2:?mpcenc path}"
QUALITIES="${3:-5,6,7}"
RUNS="${4:-3}"

[ -x "$MPCENC" ] || { echo "mpcenc not found at $MPCENC"; exit 1; }
MPCENC="$(cd "$(dirname "$MPCENC")" && pwd)/$(basename "$MPCENC")"

RESULTS_DIR="$BENCH/results"
mkdir -p "$RESULTS_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULTS="$RESULTS_DIR/enc-$STAMP.tsv"
BUILD="${MPC_BENCH_BUILD:-$ROOT/build}"

{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  python3 "$ROOT/scripts/ci_config.py" --build "$BUILD" --role benchmark-encoder \
    --selected-config Release --executable "$MPCENC" --corpus "$CORPUS" --format metadata
  echo "# qualities: $QUALITIES"
  echo "# runs: $RUNS"
  echo "# comparison: analyser=auto, psychoacoustics=scalar|simd"
  echo "# statistic: median of runs"
  echo "# columns: file quality psy_impl audio_s median_wall_ms realtime_x bytes speedup_vs_scalar_pct"
} | tee "$RESULTS"

# One python driver: reads the WAV duration, times each encode (best of RUNS),
# and emits the TSV rows. Avoids per-call interpreter startup.
QUALITIES="$QUALITIES" RUNS="$RUNS" MPCENC="$MPCENC" python3 - "$CORPUS" "$RESULTS" <<'PY'
import hashlib, os, statistics, subprocess, sys, tempfile, time, wave

corpus, results = sys.argv[1], sys.argv[2]
mpcenc = os.environ["MPCENC"]
quals = [int(q) for q in os.environ["QUALITIES"].split(",")]
runs = int(os.environ["RUNS"])

def dur(w):
    with wave.open(w, "rb") as f:
        return f.getnframes() / float(f.getframerate())

rows = []
for wav in sorted(os.listdir(corpus)):
    if not wav.endswith(".wav"):
        continue
    w = os.path.join(corpus, wav)
    d = dur(w)
    for q in quals:
        measurements = {}
        hashes = {}
        sizes = {}
        for impl in ("scalar", "simd"):
            timings = []
            for run in range(runs):
                out = os.path.join(tempfile.gettempdir(),
                                   "encbench-%d-%d-%s-%d.mpc" % (os.getpid(), q, impl, run))
                try:
                    os.unlink(out)
                except FileNotFoundError:
                    pass
                t0 = time.perf_counter()
                result = subprocess.run([mpcenc, "--silent", "--overwrite", "--psy-impl", impl,
                                         "--quality", str(q), w, out],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
                ms = (time.perf_counter() - t0) * 1000
                if result.returncode != 0:
                    raise SystemExit("encode failed (%s q%d %s): %s" %
                                     (wav, q, impl, result.stderr.strip()))
                if not os.path.isfile(out) or os.path.getsize(out) == 0:
                    raise SystemExit("encode produced no output (%s q%d %s)" % (wav, q, impl))
                with open(out, "rb") as f:
                    data = f.read()
                digest = hashlib.sha256(data).hexdigest()
                if impl in hashes and digest != hashes[impl]:
                    raise SystemExit("non-deterministic output (%s q%d %s)" % (wav, q, impl))
                hashes[impl] = digest
                sizes[impl] = len(data)
                timings.append(ms)
                os.unlink(out)
            measurements[impl] = statistics.median(timings)
        if hashes["scalar"] != hashes["simd"]:
            raise SystemExit("psy scalar/SIMD output differs (%s q%d)" % (wav, q))
        scalar_ms = measurements["scalar"]
        for impl in ("scalar", "simd"):
            median_ms = measurements[impl]
            x = d * 1000 / median_ms if median_ms > 0 else 0
            speedup = 0.0 if impl == "scalar" else (scalar_ms / median_ms - 1.0) * 100.0
            row = "%s\t%d\t%s\t%.3f\t%.1f\t%.1f\t%d\t%.2f" % (wav, q, impl, d, median_ms, x, sizes[impl], speedup)
            rows.append(row)
            print(row, flush=True)

with open(results, "a") as f:
    f.write("\n".join(rows) + "\n")
PY
echo "results appended to $RESULTS"
