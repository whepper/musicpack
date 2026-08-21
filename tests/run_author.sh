#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Integration tests for the MusicPack Author backend: the `musicpack` CLI
# draft commands (inspect / validate-draft / build-draft / identify-draft)
# that power the MusicPack Author desktop GUI.
#
# Exercises:
#   - inspect scans a tagged .mpc album into a draft (metadata, discs,
#     tracks, codec/duration, artwork), creating no package
#   - release vs source vs identity semantics survive the round-trip
#   - validate-draft reports errors and warnings separately
#   - build-draft produces a package that passes `musicpack verify`
#   - failed verification / invalid drafts are surfaced as failures
#   - path handling stays safe (traversal rejected)
#   - identify-draft applies an offline MusicBrainz release exactly
#
# Usage: tests/run_author.sh <musicpack-cmd>

set -u

MUSICPACK="${1:?musicpack cmd}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
META="$ROOT/tests/reference/meta"
MPC_REF="$ROOT/tests/reference/test-musicpack-album.mpack"
PY=python3

FAILED=0
PASSED=0
fail() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }
pass() { echo "PASS  $1"; PASSED=$((PASSED + 1)); }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/author-integration.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

ALBUM="$TMP/album"
mkdir -p "$ALBUM"
cp "$META/album-ape.mpc" "$ALBUM/01 - Big in Japan.mpc"
cp "$MPC_REF/audio/01 - Alphaville - Big in Japan.mpc" "$ALBUM/02 - The Van.mpc"
printf 'fake-cover' > "$ALBUM/cover.jpg"
printf 'notes' > "$ALBUM/notes.txt"

# 1. inspect produces a structured draft and creates no package
if "$MUSICPACK" inspect "$ALBUM" --json 2>/dev/null > "$TMP/draft.json" \
   && $PY - "$TMP/draft.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["schema"] == "musicpack-draft", "draft schema"
assert d["sourceRoot"].endswith("/album"), "sourceRoot"
assert d["album"]["title"], "album title"
assert d["source"]["store"] == "Deezer", "source tag mapped"
assert len(d["media"]) >= 1 and len(d["media"][0]["tracks"]) >= 1, "tracks"
t = d["media"][0]["tracks"][0]
assert t.get("audioPath"), "audioPath"
assert t.get("codec"), "codec probe"
assert "duration" in t, "duration probe"
assert any(a.get("path") == "cover.jpg" for a in d["artwork"]), "file artwork"
assert d["extras"] == [{"path": "notes.txt"}], "extras classified"
assert "format" not in d.get("release", {}), "release separate from source"
print("ok")
EOF
then
    pass "inspect builds a draft (metadata, discs, probes, artwork)"
else
    fail "inspect builds a draft (metadata, discs, probes, artwork)"
fi

if [ ! -e "$TMP/draft.json" ] || [ -e "$TMP/album/manifest.json" ]; then
    fail "inspect creates no package"
else
    pass "inspect creates no package"
fi

# 1b. embedded APE cover art is listed when no cover file shadows it
EMBED="$TMP/embed"
mkdir -p "$EMBED"
cp "$META/album-ape.mpc" "$EMBED/01 - Big in Japan.mpc"
if "$MUSICPACK" inspect "$EMBED" --json 2>/dev/null > "$TMP/embed.json" \
   && $PY - "$TMP/embed.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert any(a.get("embedded") and a.get("sourceAudio") for a in d["artwork"]), \
    "embedded artwork listed"
print("ok")
EOF
then
    pass "inspect lists embedded artwork without a cover file"
else
    fail "inspect lists embedded artwork without a cover file"
fi

# 1c. A malformed binary APE cover with no NUL filename separator must stay
# bounded. Both import's direct extraction and build-draft's embedded path use
# image magic and produce a stable extension instead of scanning past the tag.
MALFORMED_APE="$TMP/malformed-ape"
mkdir -p "$MALFORMED_APE"
$PY - "$META/album-ape.mpc" "$MALFORMED_APE/01 - No Separator.mpc" <<'EOF'
import sys
data = bytearray(open(sys.argv[1], "rb").read())
key = b"Cover Art (Front)\0"
key_at = data.index(key)
value_len = int.from_bytes(data[key_at - 8:key_at - 4], "little")
value_at = key_at + len(key)
assert value_len >= 3
data[value_at:value_at + value_len] = b"\xff\xd8\xff" + b"\xff" * (value_len - 3)
assert b"\0" not in data[value_at:value_at + value_len]
open(sys.argv[2], "wb").write(data)
EOF
if "$MUSICPACK" import -L -a Alphaville -o "$TMP/malformed-import.mpack" \
        "$MALFORMED_APE" >/dev/null 2>&1 \
   && [ -f "$TMP/malformed-import.mpack/artwork/front.jpg" ] \
   && "$MUSICPACK" inspect "$MALFORMED_APE" --json 2>/dev/null > "$TMP/malformed.json" \
   && $PY - "$TMP/malformed.json" "$TMP/malformed-ready.json" <<'EOF' &&
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "Alphaville", "role": "main"}]
d["identity"] = {"source": "local", "confidence": "none"}
d["waveformAnalysis"] = {"status": "disabled", "intervalMs": 100,
                        "encoding": "peak-rms-u8", "floorDb": -60, "tracks": []}
json.dump(d, open(sys.argv[2], "w"))
EOF
   "$MUSICPACK" build-draft --draft "$TMP/malformed-ready.json" \
        -o "$TMP/malformed-build.mpack" --no-loudness >/dev/null 2>&1 \
   && [ -f "$TMP/malformed-build.mpack/artwork/front.jpg" ]; then
    pass "APE artwork without a NUL separator is extracted safely by both paths"
else
    fail "APE artwork without a NUL separator is extracted safely by both paths"
fi

# 1d. uppercase disc subdirectories ("CD1"/"CD2") are recognized: real
# collections (e.g. Deezer Deemix converts) use CD1/CD2, and tracks under
# them must not be silently skipped as non-disc subdirectories.
CASE="$TMP/cdcase"
mkdir -p "$CASE/CD1" "$CASE/CD2"
cp "$ROOT/tests/reference/author-fixture/Neon Skyline/disc-1/"*.flac "$CASE/CD1/"
cp "$ROOT/tests/reference/author-fixture/Neon Skyline/disc-2/"*.flac "$CASE/CD2/"
if "$MUSICPACK" inspect "$CASE" --json 2>/dev/null > "$TMP/cdcase.json" \
   && $PY - "$TMP/cdcase.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
media = d["media"]
assert len(media) == 2, f"two discs, got {len(media)}"
assert [m["disc"] for m in media] == [1, 2], "disc numbers 1 and 2"
assert sum(len(m["tracks"]) for m in media) == 5, "all 5 tracks discovered"
assert media[0]["tracks"][0]["audioPath"].startswith("CD1/"), "track path keeps CD1"
print("ok")
EOF
then
    pass "inspect recognizes uppercase CD1/CD2 disc directories"
else
    fail "inspect recognizes uppercase CD1/CD2 disc directories"
fi

# 2. the draft round-trips through validate-draft cleanly
if "$MUSICPACK" validate-draft --draft "$TMP/draft.json" --json 2>/dev/null \
   | grep -q '"ok":[[:space:]]*false'; then
    pass "initial draft validation surfaces missing artist"
else
    fail "initial draft validation surfaces missing artist"
fi

# 3. complete the draft (simulating GUI edits) and validate it green
$PY - "$TMP/draft.json" "$TMP/ready.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["artists"] = [{"name": "Alphaville", "role": "main"}]
d["release"] = {"releaseDate": "1984-06-01", "edition": "1984 7-inch",
                "country": "DE", "label": "WEA", "catalogueNumber": "249 102-7"}
d["identifiers"] = {"musicbrainzReleaseId": "11111111-2222-3333-4444-555555555555",
                    "barcode": "198704979941"}
d["identity"] = {"source": "local", "confidence": "none"}
# Waveform envelopes are default-on in Author. This test uses 4-byte
# placeholder .mpc files that libmusicpack_audio_* cannot decode, so
# we explicitly opt out via the disabled status (per the waveform spec).
d["waveformAnalysis"] = {
    "status": "disabled",
    "intervalMs": 100,
    "encoding": "peak-rms-u8",
    "floorDb": -60,
    "tracks": [],
}
json.dump(d, open(sys.argv[2], "w"))
EOF
OUT="$("$MUSICPACK" validate-draft --draft "$TMP/ready.json" --json 2>/dev/null)"
if echo "$OUT" | grep -q '"ok":[[:space:]]*true' \
   && echo "$OUT" | grep -q '"errors":[[:space:]]*\[\]'; then
    pass "complete draft validates green"
else
    fail "complete draft validates green"
fi

# 3b. release / source / identity stay separate in the manifest
OUT2="$("$MUSICPACK" build-draft --draft "$TMP/ready.json" -o "$TMP/out.mpack" --json 2>/dev/null)"
if echo "$OUT2" | grep -q '"ok":[[:space:]]*true'; then
    pass "build-draft creates a package"
else
    fail "build-draft creates a package"
fi
if $PY - "$TMP/out.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["album"]["title"], "title"
assert m["release"]["edition"] == "1984 7-inch", "edition in release"
assert m["source"]["type"] == "digital-download", "source not merged into release"
assert m["identity"]["confidence"] == "none", "identity separate"
assert m["loudness"]["algorithm"] == "ITU-R BS.1770-5", "canonical loudness"
assert any(a["role"] == "front" for a in m["artwork"]), "embedded artwork extracted"
print("ok")
EOF
then
    pass "built manifest preserves release/source/identity semantics"
else
    fail "built manifest preserves release/source/identity semantics"
fi

# 3c. the built package passes full verification
if "$MUSICPACK" verify "$TMP/out.mpack" --json 2>/dev/null | grep -q '"ok":[[:space:]]*true'; then
    pass "built package passes verify"
else
    fail "built package passes verify"
fi

# 3d. an existing .mpack re-opens as an editable draft (Author's open path)
PKG="$TMP/edit.mpack"
cp -R "$TMP/out.mpack" "$PKG"
if "$MUSICPACK" inspect "$PKG" --json 2>/dev/null > "$TMP/reopened.json" \
   && $PY - "$TMP/reopened.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("openedFrom", "").endswith("edit.mpack"), "openedFrom records the package"
t0 = d["media"][0]["tracks"][0]
assert t0["audioPath"].startswith("audio/"), "in-package audio path: " + t0["audioPath"]
assert d["artwork"] and d["artwork"][0].get("path", "").startswith("artwork/"), \
    "artwork carried from the manifest"
assert d["album"]["title"], "album metadata from manifest"
print("ok")
EOF
then
    pass "inspect reopens a built .mpack as an editable draft"
else
    fail "inspect reopens a built .mpack as an editable draft"
fi

# 3e. --replace refuses to create; it only overwrites an existing package
if "$MUSICPACK" build-draft --draft "$TMP/ready.json" -o "$TMP/absent.mpack" \
     --replace --json 2>/dev/null | grep -q 'destination_missing'; then
    pass "--replace refuses a missing target"
else
    fail "--replace refuses a missing target"
fi

# 3f. edit a title and save back in place (--replace --sync-tags)
$PY - "$TMP/reopened.json" "$TMP/edit.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["media"][0]["tracks"][0]["title"] = "Edited Title"
d["waveformAnalysis"] = {"status": "disabled", "intervalMs": 100,
                         "encoding": "peak-rms-u8", "floorDb": -60, "tracks": []}
json.dump(d, open(sys.argv[2], "w"))
EOF
if OUT="$("$MUSICPACK" build-draft --draft "$TMP/edit.json" -o "$PKG" \
           --replace --sync-tags --json 2>/dev/null)" \
   && echo "$OUT" | grep -q '"ok":[[:space:]]*true' \
   && echo "$OUT" | grep -q '"replaced":[[:space:]]*true'; then
    pass "build-draft --replace saves a package in place"
else
    fail "build-draft --replace saves a package in place"
fi
if $PY - "$PKG/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["media"][0]["tracks"][0]["title"] == "Edited Title", "title updated"
assert m["media"][0]["tracks"][0]["loudness"], "measured loudness preserved"
print("ok")
EOF
then
    pass "in-place save updates the manifest and keeps loudness"
else
    fail "in-place save updates the manifest and keeps loudness"
fi
if [ ! -d "$PKG.old-"* ] 2>/dev/null && "$MUSICPACK" verify "$PKG" --json 2>/dev/null \
     | grep -q '"ok":[[:space:]]*true'; then
    pass "no backup leftovers and the saved package verifies"
else
    fail "no backup leftovers and the saved package verifies"
fi

# 3g. a second identical save must not churn any audio bytes (sync is
# idempotent; untouched tracks keep their bytes)
AUDIO_MID="$(find "$PKG/audio" -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256)"
"$MUSICPACK" inspect "$PKG" --json 2>/dev/null > "$TMP/reopened2.json"
"$MUSICPACK" build-draft --draft "$TMP/reopened2.json" -o "$PKG" \
    --replace --sync-tags --json >/dev/null 2>&1
AUDIO_AFTER="$(find "$PKG/audio" -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256)"
if [ "$AUDIO_MID" = "$AUDIO_AFTER" ]; then
    pass "second save leaves every audio file byte-identical"
else
    fail "second save leaves every audio file byte-identical"
fi

# 4. validation errors propagate as errors, warnings separately
$PY - "$TMP/ready.json" "$TMP/bad.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["album"]["title"] = ""
d["album"]["releaseType"] = "bogus"
d["media"][0]["format"] = "NotAFormat"
dup = json.loads(json.dumps(d["media"][0]["tracks"][0]))
dup["track"] = 3
d["media"][0]["tracks"].append(dup)
d["media"][0]["tracks"][1]["audioPath"] = "../../etc/passwd"
json.dump(d, open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" validate-draft --draft "$TMP/bad.json" --json 2>/dev/null \
   | $PY -c '
import json, sys
d = json.load(sys.stdin)
assert d["ok"] is False
msgs = "|".join(d["errors"])
for want in ("missing required album title", "unsupported release type",
             "invalid medium format", "duplicate track number 3 on disc 1",
             "invalid path"):
    assert want in msgs, f"missing error: {want}"
print("ok")
'; then
    pass "validation errors propagate (title/type/format/duplicates/path)"
else
    fail "validation errors propagate (title/type/format/duplicates/path)"
fi

# 4b. build-draft refuses an invalid draft and never reports success
if "$MUSICPACK" build-draft --draft "$TMP/bad.json" -o "$TMP/nope.mpack" --json 2>/dev/null \
   | grep -q '"ok":[[:space:]]*false'; then
    pass "build-draft refuses an invalid draft"
else
    fail "build-draft refuses an invalid draft"
fi
if [ -e "$TMP/nope.mpack/manifest.json" ]; then
    fail "no package created for an invalid draft"
else
    pass "no package created for an invalid draft"
fi

# 5. path handling stays safe: traversal in a track path is rejected
$PY - "$TMP/ready.json" "$TMP/trav.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["media"][0]["tracks"][0]["audioPath"] = "../../etc/passwd"
json.dump(d, open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" validate-draft --draft "$TMP/trav.json" --json 2>/dev/null \
   | grep -q "invalid path"; then
    pass "traversal path rejected"
else
    fail "traversal path rejected"
fi

# 6. identify-draft applies an offline MusicBrainz release exactly
if "$MUSICPACK" identify-draft --draft "$TMP/ready.json" \
   --mb-json "$META/mb-release.json" --json 2>/dev/null > "$TMP/ident.json" \
   && $PY - "$TMP/ident.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["applied"] is True
assert d["confidence"] == "exact", "exact match via release id"
dr = d["draft"]
assert dr["identity"]["source"] == "musicbrainz", "identity source set"
assert dr["identity"]["confidence"] == "exact", "identity confidence set"
assert dr["album"]["releaseType"] == "compilation", "releaseType enriched"
assert dr["identifiers"]["musicbrainzReleaseGroupId"], "release group enriched"
ti = dr["media"][0]["tracks"][1]["identifiers"]
assert ti["isrc"] == "GBK3W2503556", "track isrc enriched (Big in Japan)"
assert dr["media"][0]["tracks"][0]["audioPath"], "audioPath preserved"
assert dr["source"]["store"] == "Deezer", "source preserved"
print("ok")
EOF
then
    pass "identify-draft applies an MB release (exact) preserving draft fields"
else
    fail "identify-draft applies an MB release (exact) preserving draft fields"
fi

# 6a. A captured release delivered through the new local assertion path must
# produce the identical draft that the former MBID lookup flow produced.
if "$MUSICPACK" identify-draft --draft "$TMP/ready.json" \
    --mb-json "$META/mb-release.json" \
    --mbid "11111111-2222-3333-4444-555555555555" --json 2>/dev/null > "$TMP/ident-assert.json" \
   && $PY - "$TMP/ident.json" "$TMP/ident-assert.json" <<'EOF'
import json, sys
previous = json.load(open(sys.argv[1]))
asserted = json.load(open(sys.argv[2]))
assert asserted == previous, "captured release must preserve applied-draft semantics"
print("ok")
EOF
then
    pass "captured MB release keeps prior applied-draft semantics through --mbid assertion"
else
    fail "captured MB release keeps prior applied-draft semantics through --mbid assertion"
fi

# 6b. a build from the identified draft carries the exact identity
$PY - "$TMP/ident.json" "$TMP/ident-draft.json" <<'EOF'
import json, sys
json.dump(json.load(open(sys.argv[1]))["draft"], open(sys.argv[2], "w"))
EOF
if "$MUSICPACK" build-draft --draft "$TMP/ident-draft.json" -o "$TMP/ident.mpack" --json 2>/dev/null \
   | grep -q '"ok":[[:space:]]*true' \
   && $PY - "$TMP/ident.mpack/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
assert m["identity"]["source"] == "musicbrainz"
assert m["identity"]["confidence"] == "exact"
print("ok")
EOF
then
    pass "identified draft builds with exact identity"
else
    fail "identified draft builds with exact identity"
fi

# 6c. identify-draft handles the live MusicBrainz /release/{id} response
# shape: the live API returns "label-info" (with a nested "label" object)
# rather than the legacy "labels" array, and returns the release-group as a
# sub-object. Both must be applied or editions never group under one album.
if "$MUSICPACK" identify-draft --draft "$TMP/draft.json" \
   --mb-json "$META/mb-release-live-shape.json" --json 2>/dev/null > "$TMP/ident-live.json" \
   && $PY - "$TMP/ident-live.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["applied"] is True
dr = d["draft"]
assert dr["identifiers"]["musicbrainzReleaseGroupId"] == \
    "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "release group applied"
assert dr["release"]["label"] == "Example Records", "label read from label-info"
assert dr["release"]["catalogueNumber"] == "ERCD 001", \
    "catalogue read from label-info"
assert dr["identity"]["source"] == "musicbrainz"
print("ok")
EOF
then
    pass "identify-draft applies the live MB release shape (label-info + release-group)"
else
    fail "identify-draft applies the live MB release shape (label-info + release-group)"
fi

# 6d. identify-draft is local-only. --mbid is an exact-release assertion for
# an already-fetched --mb-json document, never a network lookup.
if "$MUSICPACK" identify-draft --draft "$TMP/ready.json" \
    --mbid "not-a-uuid" >/dev/null 2>&1; then
    fail "identify-draft rejects a non-UUID MBID"
else
    pass "identify-draft rejects a non-UUID MBID"
fi
if "$MUSICPACK" identify-draft --draft "$TMP/ready.json" \
    --mbid "11111111-2222-3333-4444-555555555555" >/dev/null 2>&1; then
    fail "identify-draft rejects a standalone MBID lookup"
else
    pass "identify-draft requires --mb-json with --mbid"
fi
if env PATH=/nonexistent "$MUSICPACK" identify-draft --draft "$TMP/ready.json" \
    --mb-json "$META/mb-release.json" --json >/dev/null 2>&1; then
    pass "identify-draft offline release apply needs no curl on PATH"
else
    fail "identify-draft offline release apply needs no curl on PATH"
fi
# Construct a captured barcode-search envelope from the existing captured
# release document, then verify candidate extraction remains C-owned.
$PY - "$META/mb-release-live-shape.json" "$TMP/mb-search.json" <<'EOF'
import json, sys
json.dump({"releases": [json.load(open(sys.argv[1]))]}, open(sys.argv[2], "w"))
EOF
if env PATH=/nonexistent "$MUSICPACK" identify-draft --draft "$TMP/draft.json" \
    --mb-search-json "$TMP/mb-search.json" --json 2>/dev/null > "$TMP/candidates.json" \
   && $PY - "$TMP/candidates.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert len(d["candidates"]) == 1
assert d["candidates"][0]["releaseId"] == "11111111-2222-3333-4444-555555555555"
print("ok")
EOF
then
    pass "identify-draft extracts candidates from captured MB search JSON without curl"
else
    fail "identify-draft extracts candidates from captured MB search JSON without curl"
fi

# 7. author-API capability handshake is machine-readable and versioned
if "$MUSICPACK" author-api-version --json 2>/dev/null > "$TMP/api.json" \
   && $PY - "$TMP/api.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["authorApi"] == 6, "author API version"
assert d["musicpackVersion"], "musicpack version present"
print("ok")
EOF
then
    pass "author-api-version reports the author API handshake"
else
    fail "author-api-version reports the author API handshake"
fi

echo
echo "== $PASSED passed, $FAILED failed =="
[ "$FAILED" -eq 0 ]
