#!/usr/bin/env bash
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
