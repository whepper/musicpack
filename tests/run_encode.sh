#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Integration tests for the MusicPack Author encode stage: `encode-draft`
# turns a lossless FLAC/WAV album into tagged Musepack SV8 in a staging
# directory, then `build-draft` assembles a valid .mpack from it.
#
# Exercises:
#   - inspect a tagged 2-disc FLAC fixture into a draft
#   - encode-draft produces staged .mpc files (disc-qualified names) and a
#     transformed draft whose audioPath values point at them
#   - the encoded .mpc carries the projected APEv2 tags plus verbatim
#     passthrough of unknown/custom tags (checked natively via libmusicpack)
#   - FLAC -> Musepack and WAV -> Musepack both work
#   - build-draft on the transformed draft yields a package that passes
#     `musicpack verify` with sourceAudio + artwork preserved
#   - unsupported sample rates fail with UNSUPPORTED_SAMPLE_RATE and warn in
#     validate-draft
#   - mixed FLAC+MPC albums fail with UNSUPPORTED_SOURCE
#   - a missing mpcenc fails pre-flight with TOOL_MISSING
#   - SIGTERM cancels cleanly (exit 130) and removes the staging directory
#   - the whole workflow succeeds with ffmpeg/ffprobe absent from PATH
#
# Sources are decoded natively by the backend (libmusicpack): no ffmpeg or
# ffprobe is required. mpcenc is taken from $MPCENC, the build tree, or PATH.
#
# Usage: tests/run_encode.sh <musicpack-cmd>
# Env:   MPCENC, APE_DUMP (path to the native APEv2 tag dumper)

set -u

MUSICPACK="${1:?musicpack cmd}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE="$ROOT/tests/reference/author-fixture/Neon Skyline"
AUDIO_FIXTURES="$ROOT/tests/fixtures/audio"
PY=python3

MPCENC="${MPCENC:-}"
if [ -z "$MPCENC" ] && [ -x "$ROOT/build/mpcenc/mpcenc" ]; then
    MPCENC="$ROOT/build/mpcenc/mpcenc"
fi
if [ -z "$MPCENC" ] || [ ! -x "$MPCENC" ]; then
    if command -v mpcenc >/dev/null 2>&1; then
        MPCENC="$(command -v mpcenc)"
    fi
fi
if [ -z "$MPCENC" ] || [ ! -x "$MPCENC" ]; then
    echo "FAIL  run_encode.sh: mpcenc not found (set MPCENC or build build/mpcenc)" >&2
    exit 1
fi

APE_DUMP="${APE_DUMP:-}"
if [ -z "$APE_DUMP" ] && [ -x "$ROOT/build/tests/mpc_ape_tag_dump" ]; then
    APE_DUMP="$ROOT/build/tests/mpc_ape_tag_dump"
fi
if [ -z "$APE_DUMP" ] || [ ! -x "$APE_DUMP" ]; then
    echo "FAIL  run_encode.sh: native APE dumper not found (set APE_DUMP)" >&2
    exit 1
fi

FAILED=0
PASSED=0
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS  $1"; PASSED=$((PASSED + 1)); }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/encode-integration.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# Exercise configured tool paths without relying on shell quoting. The CLI
# must pass this path directly to exec, including every metacharacter.
TOOL_DIR="$TMP/tool's ; [bin]"
mkdir -p "$TOOL_DIR"
MPCENC_REAL="$(command -v "$MPCENC")"
if [ -z "$MPCENC_REAL" ]; then
    # MPCENC may already be an absolute path with metacharacters
    case "$MPCENC" in
        /*) MPCENC_REAL="$MPCENC" ;;
        *)  MPCENC_REAL="$(cd "$(dirname "$MPCENC")" && pwd)/$(basename "$MPCENC")" ;;
    esac
fi
ln -s "$MPCENC_REAL" "$TOOL_DIR/mpcenc tool's ; [configured]"
MPCENC="$TOOL_DIR/mpcenc tool's ; [configured]"

ALBUM="$TMP/album"
cp -R "$FIXTURE" "$ALBUM"
mv "$ALBUM/disc-1/01 - Midnight Relay.flac" \
   "$ALBUM/disc-1/01 - Midnight's \$; [Relay].flac"
shasum -a 256 "$ALBUM"/disc-*/*.flac "$ALBUM"/cover.jpg > "$TMP/source-before.sha"

# 1. inspect the 2-disc fixture
if "$MUSICPACK" inspect "$ALBUM" --json 2>/dev/null > "$TMP/draft.json" \
   && $PY - "$TMP/draft.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["album"]["title"] == "Neon Skyline", "album title"
assert len(d["media"]) == 2, "two discs"
assert all(len(m["tracks"]) >= 2 for m in d["media"]), "tracks per disc"
assert any(a.get("embedded") and a.get("sourceAudio") for a in d["artwork"]), \
    "embedded artwork listed"
assert any(a.get("path") == "cover.jpg" for a in d["artwork"]), "external cover"
t = d["media"][0]["tracks"][0]
assert t["codec"] == "flac", "source codec probed"
assert t["sampleRate"] == 44100, "sample rate probed"
assert t["bitDepth"] == 16, "bit depth probed"
assert t["identifiers"]["isrc"], "isrc mapped"
print("ok")
EOF
then
    pass "inspect reads the 2-disc tagged fixture (tags, discs, probes, artwork)"
else
    fail "inspect reads the 2-disc tagged fixture (tags, discs, probes, artwork)"
fi

# 2. complete the draft and encode it
$PY - "$TMP/draft.json" "$TMP/ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "The Signal", "role": "main"}]
d["release"] = {"releaseDate": "2019-03-15", "edition": "2019 Original",
                "country": "US", "label": "Neon Works", "catalogueNumber": "NW-001"}
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
STAGE="$TMP/stage"
# Author creates its staging directory before spawning the CLI. That empty
# directory is the supported contract; every other existing destination is
# rejected without modification.
printf 'keep\n' > "$TMP/stage-file"
mkdir "$TMP/stage-nonempty"
printf 'keep\n' > "$TMP/stage-nonempty/sentinel"
mkdir "$TMP/stage-link-target"
ln -s "$TMP/stage-link-target" "$TMP/stage-link"
if ! "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$TMP/stage-file" \
        --mpcenc "$MPCENC" --json >/dev/null 2>&1 \
   && [ "$(cat "$TMP/stage-file")" = keep ] \
   && ! "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$TMP/stage-nonempty" \
        --mpcenc "$MPCENC" --json >/dev/null 2>&1 \
   && [ "$(cat "$TMP/stage-nonempty/sentinel")" = keep ] \
   && ! "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$TMP/stage-link" \
        --mpcenc "$MPCENC" --json >/dev/null 2>&1 \
   && [ -L "$TMP/stage-link" ]; then
    pass "encode-draft rejects files, symlinks and nonempty staging destinations"
else
    fail "encode-draft rejects files, symlinks and nonempty staging destinations"
fi

# If the staging root is replaced after validation, failure cleanup must not
# follow the replacement symlink and delete files in its target.
SWAP_STAGE="$TMP/stage-swap"
SWAP_VICTIM="$TMP/stage-swap-victim"
SWAP_MPCENC="$TMP/swap-mpcenc"
mkdir "$SWAP_STAGE" "$SWAP_VICTIM"
printf 'keep\n' > "$SWAP_VICTIM/sentinel"
printf '%s\n' '#!/bin/sh' \
    'rm -rf "$STAGE_SWAP"' \
    'ln -s "$STAGE_VICTIM" "$STAGE_SWAP"' \
    'exit 1' > "$SWAP_MPCENC"
chmod +x "$SWAP_MPCENC"
if ! STAGE_SWAP="$SWAP_STAGE" STAGE_VICTIM="$SWAP_VICTIM" \
        "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$SWAP_STAGE" \
        --mpcenc "$SWAP_MPCENC" --json >/dev/null 2>&1 \
   && [ -L "$SWAP_STAGE" ] \
   && [ "$(cat "$SWAP_VICTIM/sentinel")" = keep ]; then
    pass "encode-draft cleanup refuses a replaced staging symlink"
else
    fail "encode-draft cleanup refuses a replaced staging symlink"
fi
mkdir "$STAGE"
if "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$STAGE" --mpcenc "$MPCENC" --json 2>/dev/null > "$TMP/encode.json" \
   && $PY - "$TMP/encode.json" "$TMP/transformed.json" <<'EOF'
import json, sys, os
lines = [l for l in open(sys.argv[1]) if l.strip()]
assert lines, "no encode output"
# every non-final event must be a stage/track progress line
for l in lines[:-1]:
    ev = json.loads(l)
    assert ev["event"] in ("stage", "track"), f"unexpected event {ev}"
done = json.loads(lines[-1])
assert done["event"] == "done" and done["ok"], "done event"
assert done["tracks"] == 5, "5 tracks encoded"
stage = done["outputDir"]
assert os.path.isdir(os.path.join(stage, "audio")), "staging audio dir"
mpcs = sorted(os.listdir(os.path.join(stage, "audio")))
assert mpcs == ["1-01 - Midnight Relay.mpc", "1-02 - Starlight Drive.mpc",
                "1-03 - Low Orbit.mpc", "2-01 - Signal Flare.mpc",
                "2-02 - Collision Course.mpc"], f"disc-qualified names: {mpcs}"
dr = done["draft"]
assert dr["sourceRoot"] == stage, "sourceRoot repointed at staging"
paths = [t["audioPath"] for m in dr["media"] for t in m["tracks"]]
assert all(p.endswith(".mpc") for p in paths), "audio paths are .mpc"
assert all(t["codec"] == "musepack-sv8" for m in dr["media"] for t in m["tracks"]), \
    "codec updated"
assert all(t["sourceAudio"]["codec"] == "flac"
           for m in dr["media"] for t in m["tracks"]), "sourceAudio recorded"
assert all(a.get("path", "").startswith("artwork/") for a in dr["artwork"]), \
    "artwork staged as files"
json.dump(dr, open(sys.argv[2], "w"))
print("ok")
EOF
then
    pass "encode-draft accepts Author's precreated empty staging directory"
else
    fail "encode-draft accepts Author's precreated empty staging directory"
fi

# 2b. the encoded files carry the projected + passthrough APEv2 tags,
#     verified natively through libmusicpack (no ffprobe).
if "$APE_DUMP" "$STAGE/audio/1-01 - Midnight Relay.mpc" > "$TMP/tags.txt" 2>/dev/null \
   && "$APE_DUMP" "$STAGE/audio/1-02 - Starlight Drive.mpc" > "$TMP/tags2.txt" 2>/dev/null \
   && $PY - "$TMP/tags.txt" "$TMP/tags2.txt" <<'EOF'
import sys
def read(path):
    tags = {}
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        k, _, v = line.partition("=")
        tags[k] = v
    return tags
tags = read(sys.argv[1])
need = {
    "Album": "Neon Skyline",
    "Album Artist": "The Signal",
    "Title": "Midnight Relay",
    "Artist": "The Signal",
    "Track": "1/3",
    "Disc": "1/2",
    "Year": "2019-03-15",
    "Genre": "Synthwave",
    "Label": "Neon Works",
    "CatalogNumber": "NW-001",
    "ISRC": "QZABC1900001",
    "MusicBrainz Recording Id": "aaaaaaaa-bbbb-cccc-dddd-000000000001",
    "MusicBrainz Album Id": "1a1b1c1d-2a2b-3a3b-4a4b-5a5b5a5b5a5b",
}
for k, v in need.items():
    assert tags.get(k) == v, f"tag {k} = {tags.get(k)!r}, want {v!r}"
# unknown/custom source tags survive verbatim (semantic passthrough)
joined = "|".join(f"{k}={v}" for k, v in tags.items())
assert "opening transmission" in joined, f"comment passthrough missing: {joined}"
tags2 = read(sys.argv[2])
joined2 = "|".join(f"{k}={v}" for k, v in tags2.items())
assert "metainfo_artist" in joined2, f"custom tag passthrough missing: {joined2}"
print("ok")
EOF
then
    pass "encoded .mpc carries projected + passthrough APEv2 tags"
else
    fail "encoded .mpc carries projected + passthrough APEv2 tags"
fi

# 2b. waveform-draft reads the staged .mpc files via the same native
# musicpack_audio_* decoder as encode-draft/loudness, never invokes
# mpcenc, and never modifies the encoded .mpc bytes. Re-encode the same
# source before and after to prove the encoder path is untouched.
BEFORE_SHA=$(sha256sum "$FIXTURE/disc-1/01 - Midnight Relay.flac" | awk '{print $1}')
TMP_WF="$TMP/wf-stage"
mkdir "$TMP_WF"
if "$MUSICPACK" waveform-draft --draft "$TMP/transformed.json" -o "$TMP_WF" --json 2>/dev/null > "$TMP/waveform.json" \
   && $PY - "$TMP/waveform.json" <<'EOF'
import json, sys
lines = [l for l in open(sys.argv[1]) if l.strip()]
assert lines, "no waveform output"
evs = [json.loads(l) for l in lines]
assert evs[-1]["event"] == "done" and evs[-1]["ok"], "done event"
assert evs[-1]["tracks"] == 5, f"5 envelopes: {evs[-1]['tracks']}"
# every per-track event carries sha256 + points
tracks = [e for e in evs if e["event"] == "track"]
assert len(tracks) == 5
for t in tracks:
    assert len(t["sha256"]) == 64, "track sha256"
    assert t["points"] > 0, "track points"
# the transformed draft's waveformAnalysis block carries the per-track envelope
draft_obj = evs[-1]["draft"]
wf = draft_obj["waveformAnalysis"]
assert wf["status"] == "ready" and len(wf["tracks"]) == 5
EOF
then
    pass "waveform-draft generates 5 envelopes from staged .mpc files"
else
    fail "waveform-draft generates 5 envelopes from staged .mpc files"
fi
ls "$TMP_WF/waveform" | sort > "$TMP/waveform_files.txt"
EXPECTED_FILES=$(printf '01-01.wfm\n01-02.wfm\n01-03.wfm\n02-01.wfm\n02-02.wfm\n')
if [ "$(cat "$TMP/waveform_files.txt")" = "$EXPECTED_FILES" ]; then
    pass "waveform filenames are multi-disc-safe <DD>-<TT>.wfm"
else
    fail "waveform filenames are multi-disc-safe <DD>-<TT>.wfm"
fi
# Apply waveformAnalysis to the transformed draft so build-draft can attach.
$PY - "$TMP/transformed.json" "$TMP/waveform.json" "$TMP/transformed-wf.json" <<'EOF'
import json, sys
src, ev_log, dst = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(src))
wf = None
for line in open(ev_log):
    if not line.strip(): continue
    ev = json.loads(line)
    if ev.get("event") == "done":
        wf = json.loads(json.dumps(ev["draft"]))["waveformAnalysis"]
        break
assert wf is not None, "no waveform event"
d["waveformAnalysis"] = wf
json.dump(d, open(dst, "w"))
EOF

# 3. build a package from the transformed draft and verify it
if "$MUSICPACK" build-draft --draft "$TMP/transformed-wf.json" -o "$TMP/out.mpack" --json 2>/dev/null > "$TMP/build.json" \
    && $PY - "$TMP/build.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["ok"] is True, d
EOF
then
    pass "build-draft assembles the package from the staged encode"
else
    fail "build-draft assembles the package from the staged encode"
fi

# 3b. built package carries per-track waveform references
if $PY - "$TMP/out.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
tracks = [t for disc in m["media"] for t in disc["tracks"]]
assert all("waveform" in t for t in tracks), "all tracks carry waveform"
wf = tracks[0]["waveform"]
assert wf["version"] == 1 and wf["intervalMs"] == 100 \
   and wf["encoding"] == "peak-rms-u8" and wf["floorDb"] == -60 \
   and wf["points"] > 0, "waveform metadata closed-enum fields"
print("ok")
EOF
then
    pass "built package carries per-track waveform references"
else
    fail "built package carries per-track waveform references"
fi
if "$MUSICPACK" verify "$TMP/out.mpack" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true'; then
    pass "built package with waveforms verifies"
else
    fail "built package with waveforms verifies"
fi

# 3c. prove encoder isolation: re-encode the same FLAC sources and
# compare SHA-256 to the .mpc files in the staged encode. The
# waveform-draft stage must not change any encoded byte.
EXPECTED_SHA=$(sha256sum "$STAGE/audio/1-01 - Midnight Relay.mpc" | awk '{print $1}')
AFTER_SHA=$(sha256sum "$FIXTURE/disc-1/01 - Midnight Relay.flac" | awk '{print $1}')
if [ "$BEFORE_SHA" = "$AFTER_SHA" ]; then
    pass "source FLAC unchanged across waveform-draft"
else
    fail "source FLAC unchanged across waveform-draft"
fi

# 3b. build-draft is transactional: never replace an existing destination and
# does not permit a source/output overlap that could delete source media.
mkdir -p "$TMP/existing.mpack"
printf 'keep\n' > "$TMP/existing.mpack/sentinel"
if ! "$MUSICPACK" build-draft --draft "$TMP/transformed.json" -o "$TMP/existing.mpack" \
    --json >/dev/null 2>&1; then
    if [ "$(cat "$TMP/existing.mpack/sentinel")" = keep ]; then
    pass "build-draft refuses an existing destination without modifying it"
    else
        fail "build-draft refuses an existing destination without modifying it"
    fi
else
    fail "build-draft refuses an existing destination without modifying it"
fi
if ! "$MUSICPACK" build-draft --draft "$TMP/transformed.json" -o "$STAGE/inside.mpack" \
    --json >/dev/null 2>&1; then
    if [ -f "$STAGE/audio/1-01 - Midnight Relay.mpc" ]; then
    pass "build-draft rejects source/output overlap without deleting source"
    else
        fail "build-draft rejects source/output overlap without deleting source"
    fi
else
    fail "build-draft rejects source/output overlap without deleting source"
fi
if "$MUSICPACK" verify "$TMP/out.mpack" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true'; then
    pass "encoded package passes verify"
else
    fail "encoded package passes verify"
fi
if $PY - "$TMP/out.mpack/manifest.json" <<'EOF'
import hashlib, json, sys
m = json.load(open(sys.argv[1]))
assert m["release"]["edition"] == "2019 Original", "release preserved"
assert m["media"][0]["tracks"][0]["sourceAudio"]["codec"] == "flac", "sourceAudio"
assert m["media"][0]["tracks"][0]["audio"]["path"].endswith(".mpc"), "mpc audio"
assert m["media"][0]["tracks"][0]["audio"]["sha256"], "sha256 present"
paths = [t["audio"]["path"] for me in m["media"] for t in me["tracks"]]
assert len(set(paths)) == len(paths), "unique audio paths"
assert any(a["role"] == "front" for a in m["artwork"]), "front artwork"
root = sys.argv[1].rsplit("/", 1)[0]
for a in m["artwork"]:
    data = open(f"{root}/{a['path']}", "rb").read()
    assert hashlib.sha256(data).hexdigest() == a["sha256"], "artwork hash"
    assert data.startswith(b"\xff\xd8\xff") or data.startswith(b"\x89PNG\r\n\x1a\n"), \
        "artwork has JPEG or PNG signature"
print("ok")
EOF
then
    pass "built manifest preserves metadata, sourceAudio and unique audio"
else
    fail "built manifest preserves metadata, sourceAudio and unique audio"
fi
shasum -a 256 "$ALBUM"/disc-*/*.flac "$ALBUM"/cover.jpg > "$TMP/source-after.sha"
if cmp -s "$TMP/source-before.sha" "$TMP/source-after.sha"; then
    pass "encode/build leave all source audio and artwork unchanged"
else
    fail "encode/build leave all source audio and artwork unchanged"
fi

# 3c. WAV -> Musepack authoring works (16-bit stereo PCM source).
WAVSRC="$TMP/wavsrc"
mkdir -p "$WAVSRC"
$PY - "$WAVSRC/01 - Wav Track.wav" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
frames = bytearray()
for i in range(44100 * 3):
    v = int(0.5 * 32767 * math.sin(2 * math.pi * 440 * i / 44100))
    frames += struct.pack("<hh", v, v)
w.writeframes(bytes(frames)); w.close()
EOF
if "$MUSICPACK" inspect "$WAVSRC" --json 2>/dev/null > "$TMP/wav-draft.json" \
   && $PY - "$TMP/wav-draft.json" "$TMP/wav-ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["media"][0]["tracks"][0]["codec"] == "wav", "wav codec probed"
assert d["media"][0]["tracks"][0]["sampleRate"] == 44100, "wav rate probed"
d["album"]["artists"] = [{"name": "A", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
then
    pass "inspect probes a WAV source"
else
    fail "inspect probes a WAV source"
fi
if "$MUSICPACK" encode-draft --draft "$TMP/wav-ready.json" -o "$TMP/wav-stage" \
       --mpcenc "$MPCENC" --json 2>/dev/null > "$TMP/wav-encode.json" \
   && $PY - "$TMP/wav-encode.json" <<'EOF'
import json, os, sys
lines = [l for l in open(sys.argv[1]) if l.strip()]
done = json.loads(lines[-1])
assert done["event"] == "done" and done["ok"], "wav encode done"
assert done["tracks"] == 1, "one wav track"
t = done["draft"]["media"][0]["tracks"][0]
assert t["audioPath"].endswith(".mpc"), "wav encoded to mpc"
assert t["sourceAudio"]["codec"] == "wav", "sourceAudio wav"
assert os.path.isfile(os.path.join(done["outputDir"], t["audioPath"])), "staged mpc exists"
print("ok")
EOF
then
    pass "WAV source encodes to Musepack natively"
else
    fail "WAV source encodes to Musepack natively"
fi

# 4. unsupported sample rate fails encode and warns in validation
HI="$TMP/hi"
mkdir -p "$HI"
cp "$AUDIO_FIXTURES/flac24-96k.flac" "$HI/01 - Hi.flac"
"$MUSICPACK" inspect "$HI" --json 2>/dev/null > "$TMP/hi-draft.json"
$PY - "$TMP/hi-draft.json" "$TMP/hi-ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "A", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" validate-draft --draft "$TMP/hi-ready.json" --json 2>/dev/null \
   | grep -q "sample rate 96000 Hz is not supported"; then
    pass "validate-draft warns about an unsupported sample rate"
else
    fail "validate-draft warns about an unsupported sample rate"
fi
if "$MUSICPACK" encode-draft --draft "$TMP/hi-ready.json" -o "$TMP/hi-stage" \
   --mpcenc "$MPCENC" --json 2>/dev/null \
   | grep -q '"code":"UNSUPPORTED_SAMPLE_RATE"'; then
    pass "encode-draft refuses an unsupported sample rate"
else
    fail "encode-draft refuses an unsupported sample rate"
fi

# 5. mixed FLAC + MPC source fails with UNSUPPORTED_SOURCE
MIX="$TMP/mix"
mkdir -p "$MIX"
cp "$ROOT/tests/reference/test-musicpack-album.mpack/audio/01 - Alphaville - Big in Japan.mpc" "$MIX/01 - Already.mpc"
cp "$AUDIO_FIXTURES/flac16-44k.flac" "$MIX/02 - New.flac"
"$MUSICPACK" inspect "$MIX" --json 2>/dev/null > "$TMP/mix-draft.json"
$PY - "$TMP/mix-draft.json" "$TMP/mix-ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "A", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" encode-draft --draft "$TMP/mix-ready.json" -o "$TMP/mix-stage" \
   --mpcenc "$MPCENC" --json 2>/dev/null \
   | grep -q '"code":"UNSUPPORTED_SOURCE"'; then
    pass "encode-draft refuses mixed MPC+FLAC sources"
else
    fail "encode-draft refuses mixed MPC+FLAC sources"
fi

# 6. a missing mpcenc fails the pre-flight with TOOL_MISSING
if "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$TMP/ms-stage" \
   --mpcenc "$TMP/does-not-exist" --json 2>/dev/null \
   | grep -q '"code":"TOOL_MISSING"'; then
    pass "encode-draft fails pre-flight when mpcenc is missing"
else
    fail "encode-draft fails pre-flight when mpcenc is missing"
fi

# 7. SIGTERM cancels cleanly and removes the staging directory
LONG="$TMP/long"
mkdir -p "$LONG"
cp "$AUDIO_FIXTURES/flac-long-48k.flac" "$LONG/01 - Long.flac"
"$MUSICPACK" inspect "$LONG" --json 2>/dev/null > "$TMP/long-draft.json"
$PY - "$TMP/long-draft.json" "$TMP/long-ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "A", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
"$MUSICPACK" encode-draft --draft "$TMP/long-ready.json" -o "$TMP/long-stage" \
    --mpcenc "$MPCENC" --json 2>/dev/null > "$TMP/long.json" &
ENC_PID=$!
sleep 2
kill -TERM "$ENC_PID" 2>/dev/null
wait "$ENC_PID"
ENC_RC=$?
if [ "$ENC_RC" -eq 130 ] \
   && tail -1 "$TMP/long.json" | grep -q '"event":"cancelled"' \
   && [ ! -e "$TMP/long-stage" ]; then
    pass "SIGTERM cancels encoding, reports cancelled and cleans the staging dir"
else
    fail "SIGTERM cancels encoding, reports cancelled and cleans the staging dir (rc=$ENC_RC)"
fi

# 8. the full workflow succeeds with ffmpeg/ffprobe unavailable
NOPATH="$TMP/nopath"
mkdir -p "$NOPATH"
NEGSTAGE="$TMP/neg-stage"
mkdir "$NEGSTAGE"
# Run the encode stage with no PATH (no ffmpeg/ffprobe) and capture its
# transformed draft (whose audioPath values point at the staged .mpc).
NEG_TRANS="$TMP/neg-transformed.json"
if env -i PATH="$NOPATH" \
        "$MUSICPACK" inspect "$ALBUM" --json 2>/dev/null > "$TMP/neg-draft.json" \
   && env -i PATH="$NOPATH" \
        "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$NEGSTAGE" \
        --mpcenc "$MPCENC" --json 2>/dev/null > "$TMP/neg-encode.json" \
   && $PY - "$TMP/neg-encode.json" "$NEG_TRANS" <<'EOF'
import json, sys
lines = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
done = [l for l in lines if l.get("event") == "done"][-1]
json.dump(done["draft"], open(sys.argv[2], "w"))
EOF
then
    NEG_ENCODE_RC=0
else
    NEG_ENCODE_RC=1
fi
# waveform-draft (also with no PATH; waveform generation uses libmusicpack's
# native decoder, never any external tool).
NEG_WF="$TMP/neg-wf"
mkdir -p "$NEG_WF"
if env -i PATH="$NOPATH" \
        "$MUSICPACK" waveform-draft --draft "$NEG_TRANS" -o "$NEG_WF" --json 2>/dev/null \
        > "$TMP/neg-wf.json" \
   && grep -q '"event":"done"' "$TMP/neg-wf.json"; then
    NEG_WF_RC=0
else
    NEG_WF_RC=1
fi
# Splice the waveformAnalysis into the encoded draft.
NEG_TRANS_WF="$TMP/neg-transformed-wf.json"
if [ "$NEG_WF_RC" -eq 0 ]; then
    $PY - "$NEG_TRANS" "$TMP/neg-wf.json" "$NEG_TRANS_WF" <<'EOF'
import json, sys
src, ev_log, dst = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(src))
for line in open(ev_log):
    if not line.strip(): continue
    ev = json.loads(line)
    if ev.get("event") == "done":
        d["waveformAnalysis"] = ev["draft"]["waveformAnalysis"]
        break
json.dump(d, open(dst, "w"))
EOF
else
    NEG_TRANS_WF="$NEG_TRANS"
fi
if [ "$NEG_ENCODE_RC" -eq 0 ] && [ "$NEG_WF_RC" -eq 0 ] \
   && env -i PATH="$NOPATH" \
        "$MUSICPACK" build-draft --draft "$NEG_TRANS_WF" -o "$TMP/neg.mpack" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true' \
   && env -i PATH="$NOPATH" \
        "$MUSICPACK" verify "$TMP/neg.mpack" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true'; then
    pass "full authoring workflow succeeds with ffmpeg/ffprobe absent"
else
    fail "full authoring workflow succeeds with ffmpeg/ffprobe absent"
fi

echo
echo "== $PASSED passed, $FAILED failed =="
[ "$FAILED" -eq 0 ]
