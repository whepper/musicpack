#!/usr/bin/env bash
# Repeatable encoder benchmark (Phase 2).
#
# Times the mpcenc CLI (wall clock) for each corpus WAV x quality, for both
# the scalar and SIMD analyser implementations (--impl scalar|simd), and
# reports the realtime multiplier (audio_seconds / encode_wall_seconds) and
# output size. Records full metadata (commit, compiler/version, arch, CPU,
# flags) and appends a TSV to bench/results/.
#
# Usage:
#   encode_bench.sh <corpus-wav-dir> <mpcenc> [--qualities 5,6,7] [--runs N]
#
# Output rows: file quality impl audio_s wall_ms realtime_x bytes
set -u

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

{
  echo "# date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "# compiler: $(cc --version 2>/dev/null | head -1 || clang --version | head -1)"
  if [ "$(uname -s)" = "Darwin" ]; then
    echo "# cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
  else
    echo "# cpu: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ //' || echo unknown)"
  fi
  echo "# arch: $(uname -m)"
  echo "# qualities: $QUALITIES"
  echo "# runs: $RUNS"
  echo "# columns: file quality impl audio_s wall_ms realtime_x bytes"
} | tee "$RESULTS"

# One python driver: reads the WAV duration, times each encode (best of RUNS),
# and emits the TSV rows. Avoids per-call interpreter startup.
QUALITIES="$QUALITIES" RUNS="$RUNS" MPCENC="$MPCENC" python3 - "$CORPUS" "$RESULTS" <<'PY'
import os, subprocess, sys, time, wave

corpus, results = sys.argv[1], sys.argv[2]
mpcenc = os.environ["MPCENC"]
quals = [int(q) for q in os.environ["QUALITIES"].split(",")]
runs = int(os.environ["RUNS"])
outbase = "/tmp/encbench-%d.mpc"

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
        for impl in ("scalar", "simd"):
            best = None
            for run in range(runs):
                out = outbase % run
                t0 = time.perf_counter()
                subprocess.run([mpcenc, "--silent", "--overwrite", "--impl", impl,
                                "--quality", str(q), w, out],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                ms = (time.perf_counter() - t0) * 1000
                if best is None or ms < best:
                    best = ms
            with open(outbase % 0, "rb") as f:
                f.seek(0, 2)
                sz = f.tell()
            x = d * 1000 / best if best > 0 else 0
            row = "%s\t%d\t%s\t%.3f\t%.1f\t%.1f\t%d" % (wav, q, impl, d, best, x, sz)
            rows.append(row)
            print(row, flush=True)

with open(results, "a") as f:
    f.write("\n".join(rows) + "\n")
for run in range(runs):
    try:
        os.unlink(outbase % run)
    except OSError:
        pass
PY
echo "results appended to $RESULTS"
