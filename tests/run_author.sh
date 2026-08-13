#!/usr/bin/env bash
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

# 1c. uppercase disc subdirectories ("CD1"/"CD2") are recognized: real
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

# 7. author-API capability handshake is machine-readable and versioned
if "$MUSICPACK" author-api-version --json 2>/dev/null > "$TMP/api.json" \
   && $PY - "$TMP/api.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["authorApi"] == 2, "author API version"
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
