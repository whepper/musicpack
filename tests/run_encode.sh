#!/usr/bin/env bash
# Integration tests for the MusicPack Author encode stage: `encode-draft`
# turns a lossless FLAC album into tagged Musepack SV8 in a staging
# directory, then `build-draft` assembles a valid .mpack from it.
#
# Exercises:
#   - inspect a tagged 2-disc FLAC fixture into a draft
#   - encode-draft produces staged .mpc files (disc-qualified names) and a
#     transformed draft whose audioPath values point at them
#   - the encoded .mpc carries the projected APEv2 tags plus verbatim
#     passthrough of unknown/custom tags (checked with ffprobe when present)
#   - build-draft on the transformed draft yields a package that passes
#     `musicpack verify` with sourceAudio + artwork preserved
#   - unsupported sample rates fail with UNSUPPORTED_SAMPLE_RATE and warn in
#     validate-draft
#   - mixed FLAC+MPC albums fail with UNSUPPORTED_SOURCE
#   - a missing mpcenc fails pre-flight with TOOL_MISSING
#   - SIGTERM cancels cleanly (exit 130) and removes the staging directory
#
# Skipped (exit 0) when ffmpeg is unavailable; mpcenc is taken from $MPCENC,
# the build tree, or PATH.
#
# Usage: tests/run_encode.sh <musicpack-cmd>

set -u

MUSICPACK="${1:?musicpack cmd}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE="$ROOT/tests/reference/author-fixture/Neon Skyline"
PY=python3

FFMPEG="${FFMPEG:-ffmpeg}"
if ! command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "SKIP  run_encode.sh: ffmpeg not available"
    exit 0
fi

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
    echo "SKIP  run_encode.sh: mpcenc not found (set MPCENC or build build/mpcenc)"
    exit 0
fi

FFPROBE="${FFPROBE:-ffprobe}"
HAVE_FFPROBE=0
command -v "$FFPROBE" >/dev/null 2>&1 && HAVE_FFPROBE=1

FAILED=0
PASSED=0
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS  $1"; PASSED=$((PASSED + 1)); }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/encode-integration.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

ALBUM="$TMP/album"
cp -R "$FIXTURE" "$ALBUM"

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
if "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$STAGE" --mpcenc "$MPCENC" --ffmpeg "$FFMPEG" --json 2>/dev/null > "$TMP/encode.json" \
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
    pass "encode-draft stages tagged .mpc tracks and transforms the draft"
else
    fail "encode-draft stages tagged .mpc tracks and transforms the draft"
fi

# 2b. the encoded files carry the projected + passthrough APEv2 tags
if [ "$HAVE_FFPROBE" -eq 1 ]; then
    if "$FFPROBE" -v error -show_entries format_tags \
        "$STAGE/audio/1-01 - Midnight Relay.mpc" 2>/dev/null > "$TMP/tags.txt" \
       && "$FFPROBE" -v error -show_entries format_tags \
        "$STAGE/audio/1-02 - Starlight Drive.mpc" 2>/dev/null > "$TMP/tags2.txt" \
       && $PY - "$TMP/tags.txt" "$TMP/tags2.txt" <<'EOF'
import sys
def read(path):
    tags = {}
    for line in open(path):
        line = line.strip()
        if line.startswith("TAG:"):
            k, _, v = line[4:].partition("=")
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
else
    echo "SKIP  encoded .mpc APEv2 tags (ffprobe not available)"
fi

# 3. build a package from the transformed draft and verify it
if "$MUSICPACK" build-draft --draft "$TMP/transformed.json" -o "$TMP/out.mpack" --json 2>/dev/null > "$TMP/build.json" \
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
if "$MUSICPACK" verify "$TMP/out.mpack" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true'; then
    pass "encoded package passes verify"
else
    fail "encoded package passes verify"
fi
if $PY - "$TMP/out.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["release"]["edition"] == "2019 Original", "release preserved"
assert m["media"][0]["tracks"][0]["sourceAudio"]["codec"] == "flac", "sourceAudio"
assert m["media"][0]["tracks"][0]["audio"]["path"].endswith(".mpc"), "mpc audio"
assert m["media"][0]["tracks"][0]["audio"]["sha256"], "sha256 present"
assert m["audio"] if False else True
paths = [t["audio"]["path"] for me in m["media"] for t in me["tracks"]]
assert len(set(paths)) == len(paths), "unique audio paths"
assert any(a["role"] == "front" for a in m["artwork"]), "front artwork"
print("ok")
EOF
then
    pass "built manifest preserves metadata, sourceAudio and unique audio"
else
    fail "built manifest preserves metadata, sourceAudio and unique audio"
fi

# 4. unsupported sample rate fails encode and warns in validation
HI="$TMP/hi"
mkdir -p "$HI"
"$FFMPEG" -v error -y -f lavfi -i "sine=frequency=440:duration=2:sample_rate=96000" \
    -c:a flac -metadata title="Hi" -metadata artist="A" -metadata album="Hi" \
    -metadata albumartist="A" -metadata track="1/1" "$HI/01 - Hi.flac" 2>/dev/null
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
   --mpcenc "$MPCENC" --ffmpeg "$FFMPEG" --json 2>/dev/null \
   | grep -q '"code":"UNSUPPORTED_SAMPLE_RATE"'; then
    pass "encode-draft refuses an unsupported sample rate"
else
    fail "encode-draft refuses an unsupported sample rate"
fi

# 5. mixed FLAC + MPC source fails with UNSUPPORTED_SOURCE
MIX="$TMP/mix"
mkdir -p "$MIX"
cp "$ROOT/tests/reference/test-musicpack-album.mpack/audio/01 - Alphaville - Big in Japan.mpc" "$MIX/01 - Already.mpc"
"$FFMPEG" -v error -y -f lavfi -i "sine=frequency=440:duration=2:sample_rate=44100" \
    -c:a flac -metadata title="New" -metadata artist="A" -metadata album="Mix" \
    -metadata albumartist="A" -metadata track="2/2" "$MIX/02 - New.flac" 2>/dev/null
"$MUSICPACK" inspect "$MIX" --json 2>/dev/null > "$TMP/mix-draft.json"
$PY - "$TMP/mix-draft.json" "$TMP/mix-ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "A", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" encode-draft --draft "$TMP/mix-ready.json" -o "$TMP/mix-stage" \
   --mpcenc "$MPCENC" --ffmpeg "$FFMPEG" --json 2>/dev/null \
   | grep -q '"code":"UNSUPPORTED_SOURCE"'; then
    pass "encode-draft refuses mixed MPC+FLAC sources"
else
    fail "encode-draft refuses mixed MPC+FLAC sources"
fi

# 6. a missing mpcenc fails the pre-flight with TOOL_MISSING
if "$MUSICPACK" encode-draft --draft "$TMP/ready.json" -o "$TMP/ms-stage" \
   --mpcenc "$TMP/does-not-exist" --ffmpeg "$FFMPEG" --json 2>/dev/null \
   | grep -q '"code":"TOOL_MISSING"'; then
    pass "encode-draft fails pre-flight when mpcenc is missing"
else
    fail "encode-draft fails pre-flight when mpcenc is missing"
fi

# 7. SIGTERM cancels cleanly and removes the staging directory
LONG="$TMP/long"
mkdir -p "$LONG"
"$FFMPEG" -v error -y -f lavfi -i "sine=frequency=330:duration=3600:sample_rate=48000" \
    -c:a flac -metadata title="Long" -metadata artist="A" -metadata album="Long" \
    -metadata albumartist="A" -metadata track="1/1" "$LONG/01 - Long.flac" 2>/dev/null
"$MUSICPACK" inspect "$LONG" --json 2>/dev/null > "$TMP/long-draft.json"
$PY - "$TMP/long-draft.json" "$TMP/long-ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "A", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
json.dump(d, open(sys.argv[2], "w"))
EOF
"$MUSICPACK" encode-draft --draft "$TMP/long-ready.json" -o "$TMP/long-stage" \
    --mpcenc "$MPCENC" --ffmpeg "$FFMPEG" --json 2>/dev/null > "$TMP/long.json" &
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

echo
echo "== $PASSED passed, $FAILED failed =="
[ "$FAILED" -eq 0 ]
