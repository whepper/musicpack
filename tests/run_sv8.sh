#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# SV8 format regression: the MusicPack encoder must always produce Musepack
# SV8 streams and nothing else.
#
# Encodes a deterministic WAV input with the built mpcenc and inspects the
# ACTUAL produced file (not source constants):
#   - the stream begins with the SV8 magic "MPCK" (not the SV7 "MP+");
#   - the SH (stream header) block reports stream version 8;
#   - the EI (encoder info) block reports the expected encoder version;
#   - the stream ends with an SE (end of stream) block;
#   - mpcdec decodes it, reports stream version 8, and the decoded WAV has
#     the same sample count / channels / rate as the input (gapless SV8).
#
# The block-structure checks are done by tests/sv8_check.py (a tiny bounded
# parser for the SV8 key/size/payload packet framing).
#
# A future refactor that changes the stream version or magic must be an
# intentional format-design decision; this test fails loudly if it happens
# accidentally.
#
# Usage: tests/run_sv8.sh <mpcenc> <mpcdec> <input.wav>
# Exit status: 0 if the produced stream is Musepack SV8, 1 otherwise.

set -u

MPCENC="${1:?mpcenc path}"
MPCDEC="${2:?mpcdec path}"
INPUT="${3:?input .wav path}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECK="$ROOT/tests/sv8_check.py"
PYTHON="${PYTHON:-python3}"

EXPECT_EI="1.32.0"   # must match mpcenc/config.h
QUALITIES="5 6"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/mpc-sv8.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

FAILED=0
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS  $1"; }

# 1. The encoder banner must report the MusicPack-maintained version and the
#    historical stable classification (even minor).
banner="$("$MPCENC" 2>&1 | head -1)"
if [[ "$banner" == *"MPC Encoder $EXPECT_EI --stable--"* ]]; then
    pass "mpcenc banner reports 'MPC Encoder $EXPECT_EI --stable--'"
else
    fail "mpcenc banner '$banner' does not report 'MPC Encoder $EXPECT_EI --stable--'"
fi

for q in $QUALITIES; do
    MPC="$TMP/out-q$q.mpc"
    if ! "$MPCENC" --silent --overwrite --quality "$q" "$INPUT" "$MPC" >/dev/null 2>&1; then
        fail "q$q: mpcenc encode failed"
        continue
    fi
    if [ ! -s "$MPC" ]; then
        fail "q$q: mpcenc produced no output"
        continue
    fi

    # 2. Parse the produced stream: MPCK magic, SH stream version 8, SE block,
    #    and the expected EI encoder version.
    if out="$("$PYTHON" "$CHECK" --assert "$MPC" --expect-ei "$EXPECT_EI" 2>&1)"; then
        pass "q$q: produced stream is SV8 ($out)"
    else
        fail "q$q: $out"
        continue
    fi

    # 3. Decode through the modern libmusepack decoder path and confirm the
    #    stream version it reports plus gapless sample-count sanity.
    WAV="$TMP/out-q$q.wav"
    if ! "$MPCDEC" -i "$MPC" >"$TMP/info-q$q.txt" 2>&1; then
        fail "q$q: mpcdec -i could not read the SV8 stream"
        continue
    fi
    if grep -q "stream version 8" "$TMP/info-q$q.txt"; then
        pass "q$q: mpcdec reports stream version 8"
    else
        fail "q$q: mpcdec does not report stream version 8"
        continue
    fi
    if ! "$MPCDEC" "$MPC" "$WAV" >/dev/null 2>&1; then
        fail "q$q: mpcdec could not decode the SV8 stream to WAV"
        continue
    fi
    if python3 - "$INPUT" "$WAV" "$TMP" <<'EOF'
import sys, wave
inp, out, tmp = sys.argv[1], sys.argv[2], sys.argv[3]
w1 = wave.open(inp, "rb")
w2 = wave.open(out, "rb")
ok = (w1.getnframes() == w2.getnframes()
      and w1.getnchannels() == w2.getnchannels()
      and w1.getframerate() == w2.getframerate())
print("input %d frames/%dch/%dHz, decoded %d frames/%dch/%dHz" % (
    w1.getnframes(), w1.getnchannels(), w1.getframerate(),
    w2.getnframes(), w2.getnchannels(), w2.getframerate()))
sys.exit(0 if ok else 1)
EOF
    then
        pass "q$q: decoded sample count / channels / rate match the input"
    else
        fail "q$q: decoded WAV properties do not match the input"
    fi
done

echo
if [ "$FAILED" -eq 0 ]; then
    echo "== SV8 format regression passed =="
    exit 0
fi
echo "== $FAILED SV8 format check(s) failed =="
exit 1
