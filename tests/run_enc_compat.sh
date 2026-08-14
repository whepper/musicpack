#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Phase 2 bit-exact encoder regression: the built encoder must produce
# byte-identical MPC output for the extended encoder corpus (q5/q6/q7).
#
# Two modes:
#   1. Live comparison (CI): a reference mpcenc is provided (2nd argument or
#      REF_MPCENC env) and the expected hashes are produced by encoding the
#      corpus with it on the spot. This is toolchain-agnostic and is how the
#      test runs on all CI platforms.
#   2. Manifest comparison (local fallback / cross-platform ground truth):
#      with no reference binary, compare against
#      tests/encoder_reference_manifest.txt. That manifest is pinned to a
#      pristine -O0 reference build (see generate_encoder_corpus.py), so it
#      is only meaningful at matching optimization.
#
# Usage:
#   tests/run_enc_compat.sh <mpcenc> [ref_mpcenc] [corpus_dir]
#
# Exit status: 0 if all outputs match, 1 otherwise.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="$ROOT/tests/encoder_reference_manifest.txt"
MPCENC="${1:-}"
REF_MPCENC="${2:-${REF_MPCENC:-}}"
CORPUS_DIR="${3:-}"

if [ -z "$MPCENC" ] || [ ! -x "$MPCENC" ]; then
    echo "ERROR: usage: tests/run_enc_compat.sh <path-to-mpcenc> [ref_mpcenc] [corpus_dir]" >&2
    exit 1
fi

PY=""
for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done
if [ -z "$PY" ]; then
    echo "ERROR: python3/python not found on PATH" >&2
    exit 1
fi

sha256() { "$PY" -c 'import sys,hashlib; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/mpc-enc-compat.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

if [ -z "$CORPUS_DIR" ]; then
    CORPUS_DIR="$TMP/corpus"
    "$PY" "$ROOT/tests/generate_encoder_corpus.py" "$CORPUS_DIR" >/dev/null
fi

QUALITIES="5 6 7"

# Produce the expected-hash manifest: <name> <quality> <sha256> per line.
if [ -n "$REF_MPCENC" ]; then
    if [ ! -x "$REF_MPCENC" ]; then
        echo "ERROR: reference mpcenc not executable: '$REF_MPCENC'" >&2
        exit 1
    fi
    echo "== comparing against reference encoder: $REF_MPCENC =="
    EXPECTED="$TMP/expected.txt"
    : > "$EXPECTED"
    for wav in "$CORPUS_DIR"/*.wav; do
        [ -e "$wav" ] || continue
        name="$(basename "$wav")"
        for q in $QUALITIES; do
            rm -f "$TMP/ref.mpc"
            if ! "$REF_MPCENC" --silent --overwrite --quality "$q" "$wav" "$TMP/ref.mpc" >/dev/null 2>&1; then
                echo "ERROR: reference encode failed for $name q=$q" >&2
                exit 1
            fi
            echo "$name $q $(sha256 "$TMP/ref.mpc")" >> "$EXPECTED"
        done
    done
else
    echo "== comparing against committed manifest: $MANIFEST =="
    EXPECTED="$MANIFEST"
fi

FAILED=0
TOTAL=0
for wav in "$CORPUS_DIR"/*.wav; do
    [ -e "$wav" ] || continue
    name="$(basename "$wav")"
    for q in $QUALITIES; do
        TOTAL=$((TOTAL + 1))
        expected="$(awk -v n="$name" -v qq="$q" '$1==n && $2==qq {print $3; exit}' "$EXPECTED")"
        if [ -z "$expected" ]; then
            echo "FAIL  $name q=$q (missing reference hash)"
            FAILED=$((FAILED + 1))
            continue
        fi
        "$MPCENC" --silent --overwrite --quality "$q" "$wav" "$TMP/out.mpc" >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            echo "FAIL  $name q=$q (encode returned non-zero)"
            FAILED=$((FAILED + 1))
            continue
        fi
        got="$(sha256 "$TMP/out.mpc")"
        if [ "$got" = "$expected" ]; then
            echo "PASS  $name q=$q"
        else
            echo "FAIL  $name q=$q (expected $expected, got $got)"
            FAILED=$((FAILED + 1))
        fi
    done
done

echo
echo "== $((TOTAL - FAILED))/$TOTAL outputs match the reference encoder =="
[ "$TOTAL" -gt 0 ] && [ "$FAILED" -eq 0 ]
