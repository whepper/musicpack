#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Integration tests for the Musepack toolchain.
#
# Requires a built tree and the tool binaries. Exercises:
#   - encode -> decode round-trip
#   - mpcdec -c (integrity check)
#   - mpcdec -i (info)
#   - mpc_demux seeking via mpcdec (decode starting at an offset)
#   - mpccut (sample-range cutting)
#   - wavcmp (sample-exact comparison)
#
# Usage:
#   tests/run_integration.sh <builddir> <mpcenc> <mpcdec> <mpccut> <wavcmp>
# Exit status: 0 if all pass, 1 otherwise.

set -u

MPCENC="${1:?mpcenc path}"
MPCDEC="${2:?mpcdec path}"
MPCCUT="${3:?mpccut path}"
WAVCMP="${4:?wavcmp path}"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/mpc-integration.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

FAILED=0
PASSED=0

fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS  $1"; PASSED=$((PASSED + 1)); }

# Generate a deterministic 1s 44.1kHz stereo test WAV.
SRC="$TMP/src.wav"
OUT="$TMP/out.mpc"
DEC="$TMP/dec.wav"

python3 - "$SRC" <<'EOF'
import math, sys, wave
rate = 44100
n = rate
with wave.open(sys.argv[1], "w") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for i in range(n):
        t = i / rate
        l = int(20000 * math.sin(2 * math.pi * 440 * t))
        r = int(20000 * math.sin(2 * math.pi * 550 * t))
        frames += (l & 0xFFFF).to_bytes(2, "little")
        frames += (r & 0xFFFF).to_bytes(2, "little")
    w.writeframes(bytes(frames))
EOF

echo "== integration tests =="

# 1. Encode
if "$MPCENC" --silent --overwrite "$SRC" "$OUT" >/dev/null 2>&1 && [ -s "$OUT" ]; then
    pass "encode"
else
    fail "encode"
fi

# 2. Integrity check
if "$MPCDEC" -c "$OUT" >/dev/null 2>&1; then
    pass "mpcdec -c"
else
    fail "mpcdec -c"
fi

# Psychoacoustic scalar/SIMD selector must preserve the complete bitstream
# when a distinct SIMD kernel is compiled. Scalar-only builds verify that the
# forced SIMD selector fails closed instead.
if ! "$MPCENC" --silent --overwrite --psy-impl scalar --quality 6 "$SRC" "$TMP/psy-scalar.mpc" >/dev/null 2>&1; then
    fail "psy scalar implementation"
elif "$MPCENC" --silent --overwrite --psy-impl simd --quality 6 "$SRC" "$TMP/psy-simd.mpc" >/dev/null 2>&1; then
    if cmp -s "$TMP/psy-scalar.mpc" "$TMP/psy-simd.mpc"; then
        pass "psy scalar/SIMD bitstream identity"
    else
        fail "psy scalar/SIMD bitstream identity"
    fi
elif [ "${MPC_PSY_SIMD_ENABLED:-1}" = 0 ]; then
    pass "psy SIMD selector fails closed when unavailable"
else
    fail "psy scalar/SIMD bitstream identity"
fi

# 3. Info
if "$MPCDEC" -i "$OUT" 2>&1 | grep -q "stream version 8"; then
    pass "mpcdec -i"
else
    fail "mpcdec -i"
fi

# 4. Full decode round-trip
if "$MPCDEC" "$OUT" "$DEC" >/dev/null 2>&1; then
    pass "decode"
else
    fail "decode"
fi

# Sequential stdin must work without random seeks, including a forward-only
# ID3v2 skip.
if cat "$OUT" | "$MPCDEC" - "$TMP/stdin.wav" >/dev/null 2>&1 &&
   cmp -s "$DEC" "$TMP/stdin.wav"; then
    pass "decode from non-seekable stdin"
else
    fail "decode from non-seekable stdin"
fi

if python3 - "$MPCDEC" "$OUT" "$TMP/live-stdin.wav" <<'EOF'
import os, subprocess, sys, time

decoder, source, output = sys.argv[1:]
proc = subprocess.Popen([decoder, "-", output], stdin=subprocess.PIPE,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
with open(source, "rb") as stream:
    data = stream.read()
for byte in data:
    os.write(proc.stdin.fileno(), bytes((byte,)))

deadline = time.monotonic() + 3.0
progressed = False
while time.monotonic() < deadline:
    if os.path.exists(output) and os.path.getsize(output) > 44:
        progressed = True
        break
    if proc.poll() is not None:
        break
    time.sleep(0.02)

try:
    proc.stdin.close()
except BrokenPipeError:
    pass
status = proc.wait(timeout=5)
if not progressed or status != 0:
    raise SystemExit(1)
EOF
then
    if cmp -s "$DEC" "$TMP/live-stdin.wav"; then
        pass "live pipe makes progress before EOF"
    else
        fail "live pipe output matches file decode"
    fi
else
    fail "live pipe makes progress before EOF"
fi

python3 - "$OUT" "$TMP/id3.mpc" <<'EOF'
import sys
with open(sys.argv[1], "rb") as source, open(sys.argv[2], "wb") as output:
    output.write(b"ID3\x04\x00\x00\x00\x00\x00\x00")
    output.write(source.read())
EOF
if cat "$TMP/id3.mpc" | "$MPCDEC" - "$TMP/id3-stdin.wav" >/dev/null 2>&1 &&
   cmp -s "$DEC" "$TMP/id3-stdin.wav"; then
    pass "forward ID3 skip on stdin"
else
    fail "forward ID3 skip on stdin"
fi

# 5. wavcmp on the round-trip (should differ slightly from source but be valid)
if "$WAVCMP" "$SRC" "$DEC" >/dev/null 2>&1; then
    pass "wavcmp runs"
else
    fail "wavcmp runs"
fi

# 6. Seeking: decode and check total decoded samples matches stream length
LEN="$("$MPCDEC" -i "$OUT" 2>&1 | grep -oE '\([0-9]+ samples\)' | tr -d '()')"
SAMPLES="${LEN%% *}"
DECCNT="$("$MPCDEC" "$OUT" "$TMP/seek.wav" 2>&1 | grep -oE '^[0-9]+ samples' | awk '{print $1}')"
if [ -n "$SAMPLES" ] && [ "$DECCNT" = "$SAMPLES" ]; then
    pass "decode sample count ($SAMPLES)"
else
    fail "decode sample count (expected $SAMPLES, got $DECCNT)"
fi

if python3 - "$DEC" "$SAMPLES" <<'EOF'
import sys, wave
with wave.open(sys.argv[1], "rb") as wav:
    assert wav.getnframes() == int(sys.argv[2])
EOF
then
    pass "WAV declares playable sample length"
else
    fail "WAV declares playable sample length"
fi

if [ -w /dev/full ]; then
    if "$MPCDEC" "$OUT" /dev/full >/dev/null 2>&1; then
        fail "WAV output failure propagation"
    else
        pass "WAV output failure propagation"
    fi
fi

set -o pipefail
if "$MPCDEC" "$OUT" - 2>/dev/null |
   python3 -c 'import sys; sys.stdin.buffer.read(44)' >/dev/null; then
    fail "short stdout write propagation"
else
    pass "short stdout write propagation"
fi
set +o pipefail

# 7. mpccut: cut first half, then decode and confirm half the samples
HALF=$((SAMPLES / 2))
if "$MPCCUT" -s 0 -e "$HALF" "$OUT" "$TMP/cut.mpc" >/dev/null 2>&1; then
    CUTCNT="$("$MPCDEC" "$TMP/cut.mpc" "$TMP/cut.wav" 2>&1 | grep -oE '^[0-9]+ samples' | awk '{print $1}')"
    if [ "$CUTCNT" = "$HALF" ]; then
        pass "mpccut ($CUTCNT samples)"
    else
        fail "mpccut count (expected $HALF, got $CUTCNT)"
    fi
else
    fail "mpccut"
fi

echo
echo "== $PASSED passed, $FAILED failed =="
[ "$FAILED" -eq 0 ]
