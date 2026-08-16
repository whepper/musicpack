#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Integration tests for the .mpack package model.
#
# Uses the reference packages under tests/reference/, the musicpack CLI and
# a libmusicpack helper. Exercises:
#   - musicpack info / verify on both reference packages
#   - JSON Schema validation (when python3 jsonschema is available)
#   - negative cases: modified audio, missing audio, stray file, traversal
#   - Musepack handoff (track -> libmusepack decode) via mpack_tests
#
# Usage: tests/run_mpack.sh <musicpack-cmd> <mpack-tests-bin>

set -u

MUSICPACK="${1:?musicpack cmd}"
MPACK_TESTS="${2:?mpack_tests binary}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="$ROOT/tests/reference"
MPC_REF="$REF/test-musicpack-album.mpack"
FLAC_REF="$REF/test-flac-album.mpack"
SCHEMA="$ROOT/specs/musicpack-v1.schema.json"

FAILED=0
PASSED=0
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS  $1"; PASSED=$((PASSED + 1)); }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/mpack-integration.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# 1. info + verify on both reference packages
if "$MUSICPACK" info "$MPC_REF" >/dev/null 2>&1; then
    pass "info mpc reference"
else
    fail "info mpc reference"
fi
if "$MUSICPACK" info "$FLAC_REF" >/dev/null 2>&1; then
    pass "info flac reference"
else
    fail "info flac reference"
fi
if "$MUSICPACK" verify "$MPC_REF" >/dev/null 2>&1; then
    pass "verify mpc reference"
else
    fail "verify mpc reference"
fi
if "$MUSICPACK" verify "$FLAC_REF" >/dev/null 2>&1; then
    pass "verify flac reference"
else
    fail "verify flac reference"
fi

# 2. JSON Schema validation (optional dependency)
if python3 -c "import jsonschema" 2>/dev/null; then
    if python3 - "$SCHEMA" "$MPC_REF/manifest.json" "$FLAC_REF/manifest.json" <<'EOF'
import json, sys
from jsonschema import Draft202012Validator
schema = json.load(open(sys.argv[1]))
for p in sys.argv[2:]:
    Draft202012Validator(schema).validate(json.load(open(p)))
EOF
    then
        pass "json schema validates reference manifests"
    else
        fail "json schema validates reference manifests"
    fi
else
    echo "SKIP  json schema (python3 jsonschema not available)"
fi

# 3. libmusicpack C tests (parse/validate/handoff/security/meter)
if "$MPACK_TESTS" "$MPC_REF" "$FLAC_REF"; then
    pass "mpack C tests"
else
    fail "mpack C tests"
fi

# 4. negative cases on a copy of the reference package
cp -R "$MPC_REF" "$TMP/mut.mpack"

# 4a. modified audio -> verify fails with a checksum error
AUDIO="$TMP/mut.mpack/audio/01 - Alphaville - Big in Japan.mpc"
printf 'X' >> "$AUDIO"
if "$MUSICPACK" verify "$TMP/mut.mpack" 2>&1 | grep -q "checksum mismatch"; then
    pass "modified audio detected"
else
    fail "modified audio detected"
fi

# 4b. missing audio -> verify fails
rm -f "$TMP/mut.mpack/audio/02 - Bleachers - The Van.mpc"
if "$MUSICPACK" verify "$TMP/mut.mpack" 2>&1 | grep -q "missing file"; then
    pass "missing audio detected"
else
    fail "missing audio detected"
fi

# 4c. stray file -> warning only (fresh copy, no other corruption)
cp -R "$MPC_REF" "$TMP/stray.mpack"
printf 'junk' > "$TMP/stray.mpack/stray.txt"
OUT="$("$MUSICPACK" verify "$TMP/stray.mpack" 2>&1)"
if echo "$OUT" | grep -q "unreferenced file"; then
    pass "stray file warning"
else
    fail "stray file warning"
fi
if echo "$OUT" | grep -q "^error:"; then
    fail "no errors for stray file"
else
    pass "no errors for stray file"
fi

# 4d. create requires an artist and does not publish a partial destination.
NOART="$TMP/noartist.mpack"
if ! "$MUSICPACK" create -o "$NOART" -t "No Artist" \
    -T "$MPC_REF/audio/01 - Alphaville - Big in Japan.mpc" >/dev/null 2>&1 \
   && [ ! -e "$NOART" ]; then
    pass "create requires artist without publishing output"
else
    fail "create requires artist without publishing output"
fi

# 5. traversal manifest rejected
TRAV="$TMP/trav.mpack"
mkdir -p "$TRAV"
python3 - "$MPC_REF/manifest.json" "$TRAV/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
m["media"][0]["tracks"][0]["audio"]["path"] = "../../etc/passwd"
json.dump(m, open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" info "$TRAV" >/dev/null 2>&1; then
    fail "traversal manifest rejected"
else
    pass "traversal manifest rejected"
fi

# 6. collector metadata: create a package with release/edition flags
CREAT="$TMP/collector.mpack"
A1="$MPC_REF/audio/01 - Alphaville - Big in Japan.mpc"
A2="$MPC_REF/audio/02 - Bleachers - The Van.mpc"
if "$MUSICPACK" create -o "$CREAT" -t "Collector Album" -a "Test Artist" \
    -R album -O 1986-06-16 -d 2016-09-23 -e "2016 Remaster" \
    -l "Example Records" -c "ABC 123" -C GB -m CD \
    -T "$A1" -n "Track One" -T "$A2" -n "Track Two" >/dev/null 2>&1; then
    pass "create with release flags"
else
    fail "create with release flags"
fi

if python3 - "$CREAT/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
rel = m["release"]
assert m["album"]["releaseType"] == "album"
assert m["album"]["originalReleaseDate"] == "1986-06-16"
assert rel["releaseDate"] == "2016-09-23"
assert rel["edition"] == "2016 Remaster"
assert rel["label"] == "Example Records"
assert rel["catalogueNumber"] == "ABC 123"
assert rel["country"] == "GB"
assert m["media"][0]["format"] == "CD"
assert m["loudness"]["algorithm"] == "ITU-R BS.1770-5"
print("ok")
EOF
then
    pass "release metadata in created manifest"
else
    fail "release metadata in created manifest"
fi

# 6b. album loudness must be a program measurement: feed a loud and a quiet
# track into one package; albumLUFS must NOT be the mean of track LUFS (the
# quiet track is relative-gated out of the album program) and album true peak
# must equal the max of track true peaks. WAV sources are measured natively
# (no ffmpeg); the concatenation-semantics proof also lives in the C test
# test_album_loudness_aggregation.
LOUD_WAV="$TMP/loud's \$; [input].wav"
QUIET_WAV="$TMP/quiet space; [input].wav"
LOUD_NAME="Loud's \$; [mix]"
QUIET_NAME="Quiet space; [mix]"
python3 - "$LOUD_WAV" "$QUIET_WAV" <<'EOF'
import math, os, struct, sys, wave
def wav(path, amp, rate=44100):
    w = wave.open(path, "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for i in range(rate * 3):
        v = int(amp * 32767 * math.sin(2 * math.pi * 1000 * i / rate))
        frames += struct.pack("<hh", v, v)
    w.writeframes(bytes(frames)); w.close()
wav(sys.argv[1], 0.95)
wav(sys.argv[2], 0.05)
EOF
WAVPACK="$TMP/lq.mpack"
if "$MUSICPACK" create -o "$WAVPACK" -t "Loud Quiet" -a "A" -R album -m Digital \
    -T "$LOUD_WAV" -n "$LOUD_NAME" -T "$QUIET_WAV" -n "$QUIET_NAME" >/dev/null 2>&1; then
    pass "create measures filenames with apostrophes, spaces and metacharacters"
else
    fail "create measures filenames with apostrophes, spaces and metacharacters"
fi
if python3 - "$WAVPACK/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
tl = [t.get("loudness") for d in m["media"] for t in d["tracks"]]
assert all(x is not None for x in tl), "special-character WAV paths must be measured natively"
assert "loudness" in m
assert m["loudness"]["algorithm"] == "ITU-R BS.1770-5"
al = m["loudness"]["albumLUFS"]
tp = m["loudness"]["albumTruePeakDbTP"]
lufs = [x["trackLUFS"] for x in tl]
peaks = [x["truePeakDbTP"] for x in tl]
mean = sum(lufs) / len(lufs)
assert abs(al - mean) > 1.0, "albumLUFS must not be the arithmetic mean of track LUFS"
assert abs(al - max(lufs)) < 1.0, "loud track dominates the album measurement"
assert abs(tp - max(peaks)) < 0.01, "album true peak must be the max of track true peaks"
print("ok")
EOF
then
    pass "album loudness is a program measurement"
else
    fail "album loudness is a program measurement"
fi

# 6c. `info` shows collector metadata first-class
OUT="$("$MUSICPACK" info "$CREAT" 2>&1)"
if echo "$OUT" | grep -q "^Type: album" \
   && echo "$OUT" | grep -q "^Edition: 2016 Remaster" \
   && echo "$OUT" | grep -q "^Release date: 2016-09-23" \
   && echo "$OUT" | grep -q "^Original release: 1986-06-16" \
   && echo "$OUT" | grep -q "^Label: Example Records" \
   && echo "$OUT" | grep -q "^Catalogue: ABC 123" \
   && echo "$OUT" | grep -q "^Country: GB" \
   && echo "$OUT" | grep -q "^Medium: CD"; then
    pass "info shows collector metadata"
else
    fail "info shows collector metadata"
fi

# 6d. same album represented by two distinct releases (1987 CD vs 2016 Digital)
E1="$TMP/ed1987.mpack"; E2="$TMP/ed2016.mpack"
"$MUSICPACK" create -o "$E1" -t "Two Faces" -a "X" -R album -O 1987-03-01 \
    -d 1987-03-01 -e "1987 CD" -m CD -T "$A1" >/dev/null 2>&1
"$MUSICPACK" create -o "$E2" -t "Two Faces" -a "X" -R album -O 1987-03-01 \
    -d 2016-09-23 -e "2016 Digital Remaster" -m Digital -T "$A2" >/dev/null 2>&1
if python3 - "$E1/manifest.json" "$E2/manifest.json" <<'EOF'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
assert a["album"]["title"] == b["album"]["title"]
assert a["album"]["originalReleaseDate"] == b["album"]["originalReleaseDate"]
assert a["release"]["edition"] != b["release"]["edition"]
assert a["release"]["releaseDate"] != b["release"]["releaseDate"]
assert a["media"][0]["format"] == "CD" and b["media"][0]["format"] == "Digital"
print("ok")
EOF
then
    pass "same album, two distinct editions"
else
    fail "same album, two distinct editions"
fi

# 6e. loudness regression: measuring the committed stereo/44.1 kHz FLAC
# reference album natively (no ffmpeg resample/downmix) must reproduce the
# committed manifest loudness. The source audio is identical, so native
# decode is byte-identical to the old ffmpeg path and the values must match.
LDR="$TMP/loudreg"
mkdir -p "$LDR"
cp "$FLAC_REF"/audio/*.flac "$LDR/"
LREGPKG="$TMP/loudreg.mpack"
if "$MUSICPACK" import -o "$LREGPKG" -t "Loudness Regression" -a "A" "$LDR" >/dev/null 2>&1 \
   && python3 - "$LREGPKG/manifest.json" "$FLAC_REF/manifest.json" <<'EOF'
import json, sys
new = json.load(open(sys.argv[1]))
old = json.load(open(sys.argv[2]))
nl = new["loudness"]; ol = old["loudness"]
assert abs(nl["albumLUFS"] - ol["albumLUFS"]) < 0.05, "albumLUFS regressed"
assert abs(nl["albumTruePeakDbTP"] - ol["albumTruePeakDbTP"]) < 0.05, "albumTP regressed"
for nm, om in zip(new["media"], old["media"]):
    for nt, ot in zip(nm["tracks"], om["tracks"]):
        assert abs(nt["loudness"]["trackLUFS"] - ot["loudness"]["trackLUFS"]) < 0.05, "trackLUFS regressed"
        assert abs(nt["loudness"]["truePeakDbTP"] - ot["loudness"]["truePeakDbTP"]) < 0.05, "trackTP regressed"
print("ok")
EOF
then
    pass "native loudness regression matches the committed reference album"
else
    fail "native loudness regression matches the committed reference album"
fi

# 7. real metadata import: embedded tags drive the manifest, flags override
META="$REF/meta"
TAGSRC="$TMP/tagsrc"
mkdir -p "$TAGSRC"
cp "$META/album-vorbis.flac" "$TAGSRC/01 - Big in Japan.flac"
TAGIMP="$TMP/tagimport.mpack"
if "$MUSICPACK" import -L -o "$TAGIMP" "$TAGSRC" >/dev/null 2>&1; then
    pass "import reads embedded tags"
else
    fail "import reads embedded tags"
fi

# 7d. import preserves sparse disc identifiers, distinguishes multi-disc
# audio paths, and retains equal-basename assets with collision-safe names.
MULTISRC="$TMP/multisrc"
mkdir -p "$MULTISRC/disc-2" "$MULTISRC/disc-4"
cp "$A1" "$MULTISRC/disc-2/01 - Same.mpc"
cp "$A2" "$MULTISRC/disc-4/01 - Same.mpc"
printf 'disc two lyrics' > "$MULTISRC/disc-2/notes.lrc"
printf 'disc four lyrics' > "$MULTISRC/disc-4/notes.lrc"
printf 'disc two extra' > "$MULTISRC/disc-2/readme.txt"
printf 'disc four extra' > "$MULTISRC/disc-4/readme.txt"
MULTI="$TMP/multidisc.mpack"
if "$MUSICPACK" import -L -o "$MULTI" -t "Multi" -a "Artist" "$MULTISRC" >/dev/null 2>&1 \
   && "$MUSICPACK" verify "$MULTI" >/dev/null 2>&1 \
   && python3 - "$MULTI/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert [d["disc"] for d in m["media"]] == [2, 4]
paths = [t["audio"]["path"] for d in m["media"] for t in d["tracks"]]
assert len(paths) == len(set(paths)) == 2
assert all(p.startswith("audio/") for p in paths)
assert len(m["lyrics"]) == len({x["path"] for x in m["lyrics"]}) == 2
assert len(m["extras"]) == len({x["path"] for x in m["extras"]}) == 2
EOF
then
    pass "import sparse discs and colliding assets"
else
    fail "import sparse discs and colliding assets"
fi

# 7e. a failed import removes its fresh sibling staging directory and leaves
# the requested destination absent.
FAILSRC="$TMP/failsrc"
mkdir -p "$FAILSRC"
cp "$MPC_REF/audio/01 - Alphaville - Big in Japan.mpc" "$FAILSRC/01 - bad.mpc"
FAILOUT="$TMP/failimport.mpack"
if ! "$MUSICPACK" import -L -o "$FAILOUT" -t "Failure" "$FAILSRC" >/dev/null 2>&1 \
   && [ ! -e "$FAILOUT" ] && ! compgen -G "$FAILOUT.import-*" >/dev/null; then
    pass "failed import cleans staging"
else
    fail "failed import cleans staging"
fi
if python3 - "$TAGIMP/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["album"]["title"] == "Synthetic Test Album", "album title from tag"
assert m["album"]["artists"][0]["name"] == "Alphaville", "album artist from tag"
assert m["album"]["originalReleaseDate"] == "1984-06-01"
assert m["album"]["genres"] == ["Synthpop", "New Wave"]
assert m["release"]["releaseDate"] == "2016-09-23"
assert m["release"]["label"] == "Example Records"
assert m["release"]["catalogueNumber"] == "ERCD 001"
assert m["identifiers"]["barcode"] == "198704979941"
assert m["identifiers"]["musicbrainzReleaseId"] == "11111111-2222-3333-4444-555555555555"
assert m["identifiers"]["musicbrainzReleaseGroupId"] == "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
assert m["source"]["store"] == "Deezer" and m["source"]["type"] == "digital-download"
t = m["media"][0]["tracks"][0]
assert t["title"] == "Big in Japan", "track title from tag"
assert t["track"] == 3, "track number from tag"
assert t["identifiers"]["isrc"] == "GBK3W2503556"
assert t["identifiers"]["musicbrainzRecordingId"] == "12121212-3434-5656-7878-909090909090"
assert t["identifiers"]["musicbrainzTrackId"] == "23232323-4545-6767-8989-abababababab"
assert t["source"]["store"] == "Deezer" and t["source"]["trackId"] == "3810015612"
roles = {a["role"]: a["path"] for a in m["artwork"]}
assert roles.get("front", "").endswith(".png"), "front artwork from FLAC picture"
assert roles.get("back", "").endswith(".jpg"), "back artwork from FLAC picture"
assert any(p.startswith("lyrics/") for p in [l["path"] for l in m["lyrics"]]), \
    "lyrics asset from LYRICS tag"
print("ok")
EOF
then
    pass "tag-driven manifest fields (FLAC/Vorbis)"
else
    fail "tag-driven manifest fields (FLAC/Vorbis)"
fi

# 7a. embedded artwork and lyrics are extracted, byte-preserving
if [ -f "$TAGIMP/artwork/front.png" ] && [ -f "$TAGIMP/artwork/back.jpg" ] \
   && [ -f "$TAGIMP/lyrics/03 - Big in Japan.txt" ]; then
    pass "embedded artwork + lyrics extracted"
else
    fail "embedded artwork + lyrics extracted"
fi

# 7b. APEv2 tags on an existing .mpc are read too
APESRC="$TMP/apesrc"
mkdir -p "$APESRC"
cp "$META/album-ape.mpc" "$APESRC/09 - whatever.mpc"
if "$MUSICPACK" import -L -o "$TMP/apeimp.mpack" -a "Synthetic Artist" "$APESRC" >/dev/null 2>&1 \
   && python3 - "$TMP/apeimp.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["album"]["title"] == "Synthetic Test Album", "ape album"
t = m["media"][0]["tracks"][0]
assert t["title"] == "Big in Japan", "ape track title"
assert t["track"] == 3, "ape track number"
assert t["source"]["store"] == "Deezer", "ape source store"
print("ok")
EOF
then
    pass "tag-driven manifest fields (MPC/APEv2)"
else
    fail "tag-driven manifest fields (MPC/APEv2)"
fi

# 7c. explicit flags still override tags
if "$MUSICPACK" import -L -o "$TMP/override.mpack" -t "Override Title" \
    -a "Override Artist" "$TAGSRC" >/dev/null 2>&1 \
   && python3 - "$TMP/override.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["album"]["title"] == "Override Title"
assert m["album"]["artists"][0]["name"] == "Override Artist"
print("ok")
EOF
then
    pass "flags override tags"
else
    fail "flags override tags"
fi

# 8. identify enriches a package from a MusicBrainz release (offline --mb-json)
if "$MUSICPACK" identify "$TAGIMP" --mb-json "$META/mb-release.json" >/dev/null 2>&1 \
   && python3 - "$TAGIMP/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m.get("identity", {}).get("source") == "musicbrainz", "identity source"
assert m["identity"]["confidence"] == "exact", "identity exact via release id"
assert m["album"]["releaseType"] == "compilation", "releaseType enriched from MB"
assert m["identifiers"]["musicbrainzReleaseId"] == "11111111-2222-3333-4444-555555555555"
assert m["identifiers"]["barcode"] == "198704979941", "barcode preserved"
print("ok")
EOF
then
    pass "identify enriches from MB release (offline)"
else
    fail "identify enriches from MB release (offline)"
fi

# 8b. identify with no evidence leaves the package untouched
if "$MUSICPACK" identify "$E1" >/dev/null 2>&1 \
   && ! grep -q '"identity"' "$E1/manifest.json"; then
    pass "identify without evidence leaves identity untouched"
else
    fail "identify without evidence leaves identity untouched"
fi

# 9. update-metadata --sync-tags writes manifest metadata into .mpc tags and
# refreshes checksums; the metadata round-trips through a re-import
SYNC="$TMP/sync.mpack"
if "$MUSICPACK" create -o "$SYNC" -t "Sync Album" -a "Sync Artist" -m CD \
    -d 2020-01-01 -R album -l "Sync Label" -c "SYNC 001" \
    -T "$MPC_REF/audio/01 - Alphaville - Big in Japan.mpc" \
    -n "Sync Track" >/dev/null 2>&1 \
   && "$MUSICPACK" update-metadata "$SYNC" --sync-tags >/dev/null 2>&1; then
    pass "update-metadata --sync-tags"
else
    fail "update-metadata --sync-tags"
fi
if "$MUSICPACK" verify "$SYNC" >/dev/null 2>&1; then
    pass "synced package still verifies"
else
    fail "synced package still verifies"
fi
mkdir -p "$TMP/syncsrc"
cp "$SYNC"/audio/*.mpc "$TMP/syncsrc/"
if "$MUSICPACK" import -L -o "$TMP/syncimp.mpack" "$TMP/syncsrc" >/dev/null 2>&1 \
   && python3 - "$TMP/syncimp.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["album"]["title"] == "Sync Album", "album synced to tags"
assert m["album"]["artists"][0]["name"] == "Sync Artist", "artist synced"
assert m["album"]["releaseType"] == "album"
assert m["release"]["releaseDate"] == "2020-01-01"
assert m["release"]["label"] == "Sync Label"
assert m["release"]["catalogueNumber"] == "SYNC 001"
assert m["media"][0]["tracks"][0]["title"] == "Sync Track", "track synced"
print("ok")
EOF
then
    pass "manifest metadata round-trips through .mpc tags"
else
    fail "manifest metadata round-trips through .mpc tags"
fi

# 9b. unknown fields survive the update-metadata round-trip
python3 - "$SYNC/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
m["xFutureField"] = {"note": "survives update"}
json.dump(m, open(sys.argv[1], "w"))
EOF
"$MUSICPACK" update-metadata "$SYNC" >/dev/null 2>&1
if grep -q "xFutureField" "$SYNC/manifest.json"; then
    pass "unknown fields survive update-metadata"
else
    fail "unknown fields survive update-metadata"
fi

# 10. Sonic analysis: build with/without sonic, verify, info, invalid handling
SONIC_SRC="$TMP/sonic-src"
SONIC_OUT="$TMP/sonic-out"
SONIC_DRAFT="$TMP/sonic-draft.json"
SONIC_DOC="$TMP/sonic.json"
mkdir -p "$SONIC_SRC"
printf 'AAAA' > "$SONIC_SRC/01 - One.mpc"
printf 'BBBB' > "$SONIC_SRC/02 - Two.mpc"
python3 - "$SONIC_DOC" "$SONIC_DRAFT" "$SONIC_SRC" <<'EOF'
import base64, json, struct, sys
def unit(dims):
    v = [0.0] * dims
    v[0] = 1.0
    return base64.b64encode(struct.pack("<%df" % dims, *v)).decode()
b = unit(512)
doc = {
    "format": "musicpack-sonic", "version": 1,
    "profile": {"id": "musicpack-sonic-openl3-v1", "dimensions": 512,
                "distance": "cosine", "encoding": "base64-f32le"},
    "analyzer": {"tool": "musicpack", "toolVersion": "test"},
    "album": {"embedding": {"encoding": "base64-f32le", "dimensions": 512,
                            "data": b}, "tracksContributing": 2},
    "tracks": [
        {"disc": 1, "track": 1, "embedding": {"encoding": "base64-f32le",
                                              "dimensions": 512, "data": b}},
        {"disc": 1, "track": 2, "embedding": {"encoding": "base64-f32le",
                                              "dimensions": 512, "data": b}},
    ],
}
json.dump(doc, open(sys.argv[1], "w"))
draft = {
    "schema": "musicpack-draft", "version": 1,
    "sourceRoot": sys.argv[3],
    "album": {"title": "Sonic Test", "artists": [{"name": "Tester"}]},
    "release": {"releaseDate": "2026-01-01", "edition": "Test",
                "catalogueNumber": "SCT-1"},
    "identifiers": {}, "identity": {"source": "local", "confidence": "none"},
    "media": [{"disc": 1, "tracks": [
        {"track": 1, "title": "One", "audioPath": "01 - One.mpc"},
        {"track": 2, "title": "Two", "audioPath": "02 - Two.mpc"}]}],
    "artwork": [], "booklet": [], "lyrics": [], "extras": [],
    "sonicAnalysis": {"status": "ready",
                      "profile": "musicpack-sonic-openl3-v1",
                      "path": sys.argv[1]},
}
json.dump(draft, open(sys.argv[2], "w"))
EOF

# build with sonic: package contains analysis/sonic.json + analysis[] ref
if "$MUSICPACK" build-draft --draft "$SONIC_DRAFT" -o "$SONIC_OUT" >/dev/null 2>&1 &&
   [ -f "$SONIC_OUT/analysis/sonic.json" ]; then
    pass "build-draft includes sonic analysis"
else
    fail "build-draft includes sonic analysis"
fi
if python3 - "$SONIC_OUT/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
a = m.get("analysis", [])
assert len(a) == 1 and a[0]["type"] == "sonic"
assert a[0]["profile"] == "musicpack-sonic-openl3-v1"
assert a[0]["path"] == "analysis/sonic.json"
assert len(a[0]["sha256"]) == 64
print("ok")
EOF
then
    pass "manifest analysis[] reference written"
else
    fail "manifest analysis[] reference written"
fi
if "$MUSICPACK" verify "$SONIC_OUT" >/dev/null 2>&1; then
    pass "verify ok with sonic"
else
    fail "verify ok with sonic"
fi
if "$MUSICPACK" info "$SONIC_OUT" --json 2>/dev/null |
   python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["sonic"]["profile"]=="musicpack-sonic-openl3-v1" and d["sonic"]["tracks"]==2 and d["sonic"]["tracksWithEmbedding"]==2'; then
    pass "info --json exposes sonic"
else
    fail "info --json exposes sonic"
fi
if "$MUSICPACK" info "$SONIC_OUT" 2>/dev/null | grep -q "Sonic Analysis:"; then
    pass "info text shows Sonic Analysis"
else
    fail "info text shows Sonic Analysis"
fi

# build without sonic: valid package, no analysis
python3 -c "
import json,sys
d=json.load(open('$SONIC_DRAFT'))
del d['sonicAnalysis']
json.dump(d, open('$SONIC_DRAFT','w'))
"
rm -rf "$SONIC_OUT"
if "$MUSICPACK" build-draft --draft "$SONIC_DRAFT" -o "$SONIC_OUT" >/dev/null 2>&1 &&
   [ ! -e "$SONIC_OUT/analysis" ] && "$MUSICPACK" verify "$SONIC_OUT" >/dev/null 2>&1; then
    pass "build-draft without sonic stays valid"
else
    fail "build-draft without sonic stays valid"
fi

# invalid sonic (track mismatch) is dropped with a warning; package still valid
python3 -c "
import json,sys
d=json.load(open('$SONIC_DOC'))
d['tracks']=[{'disc':1,'track':9,'embedding':d['album']['embedding']}]
json.dump(d, open('$SONIC_DOC','w'))
d2=json.load(open('$SONIC_DRAFT'))
d2['sonicAnalysis']={'status':'ready','profile':'musicpack-sonic-openl3-v1','path':'$SONIC_DOC'}
json.dump(d2, open('$SONIC_DRAFT','w'))
"
rm -rf "$SONIC_OUT"
if "$MUSICPACK" build-draft --draft "$SONIC_DRAFT" -o "$SONIC_OUT" --json 2>/dev/null |
   python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] and d["sonic"] is False and "sonicWarning" in d'; then
    pass "invalid sonic dropped with warning"
else
    fail "invalid sonic dropped with warning"
fi
if "$MUSICPACK" verify "$SONIC_OUT" >/dev/null 2>&1; then
    pass "package without sonic verifies after drop"
else
    fail "package without sonic verifies after drop"
fi

echo
echo "== $PASSED passed, $FAILED failed =="
[ "$FAILED" -eq 0 ]
