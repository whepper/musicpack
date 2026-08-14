#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Build the decoder benchmark corpus: deterministic WAV signals (from
# tests/generate_corpus.py) encoded to SV8 at several qualities, plus the
# committed correctness fixtures.
#
# Usage: make_corpus.sh <outdir> [mpcenc]
#   outdir   directory that will contain corpus/*.mpc (created)
#   mpcenc   path to the mpcenc binary (default: <repo>/build/mpcenc/mpcenc)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?outdir}"
MPCENC="${2:-$ROOT/build/mpcenc/mpcenc}"

[ -x "$MPCENC" ] || { echo "mpcenc not found at $MPCENC"; exit 1; }

WAV="$OUT/corpus-wav"
MPC="$OUT/corpus"
mkdir -p "$WAV" "$MPC"

python3 "$ROOT/tests/generate_corpus.py" "$WAV"

# Long tracks: short corpus files make per-file timings noise-dominated by
# fixed open/header overhead. Two deterministic synthetic tracks (30s/60s)
# give the decode path a workload where the SIMD gain is actually visible.
python3 - "$WAV" <<'PY'
import math, random, struct, sys, wave
outdir = sys.argv[1]
rng = random.Random(99)
def write(name, rate, dur):
    n = rate * dur
    w = wave.open(f"{outdir}/{name}.wav", "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        # dense tonal + noise mix (deterministic)
        l = 0.5 * math.sin(2 * math.pi * 440 * t) + 0.3 * math.sin(2 * math.pi * 2000 * t) \
            + 0.08 * rng.uniform(-1, 1)
        r = 0.4 * math.sin(2 * math.pi * 660 * t) + 0.25 * math.sin(2 * math.pi * 3000 * t) \
            + 0.08 * rng.uniform(-1, 1)
        for v in (l, r):
            iv = int(round(max(-1.0, min(1.0, v)) * 32767))
            frames += struct.pack("<h", iv)
    w.writeframes(bytes(frames)); w.close()
write("long_30s", 44100, 30)
write("long_60s", 44100, 60)
PY

encoded=0
for w in "$WAV"/*.wav; do
    base="$(basename "$w" .wav)"
    for q in 5 7; do
        out="$MPC/${base}-q${q}.mpc"
        if [ ! -s "$out" ]; then
            "$MPCENC" --silent --overwrite --quality "$q" "$w" "$out"
        fi
        encoded=$((encoded + 1))
    done
done

# The committed fixtures double as short-input correctness references.
mkdir -p "$OUT/fixtures"
cp "$ROOT"/tests/fixtures/*.mpc "$OUT/fixtures/"

echo "corpus ready: $encoded encodes in $MPC, fixtures in $OUT/fixtures"
