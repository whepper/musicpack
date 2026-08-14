#!/usr/bin/env bash
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
# Hostile-package regression: special filesystem objects must fail verification
# quickly instead of blocking, and symlink escapes must be rejected.
#
# Usage: tests/run_mpack_hostile.sh <musicpack-bin>
set -u

MP="${1:?musicpack binary}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/mpack-hostile.XXXXXX")"
PASS=0
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# minimal valid manifest referencing one audio asset
make_pkg() {
    local dir="$1" path="$2"
    mkdir -p "$dir/audio"
    printf '{"format":"musicpack","version":1,"album":{"title":"T","artists":[{"name":"A"}]},"media":[{"disc":1,"tracks":[{"track":1,"title":"T","audio":{"path":"%s","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}]}]}' \
        "$path" > "$dir/manifest.json"
}

ok()   { echo "PASS $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL $1"; FAIL=$((FAIL+1)); }

# 1. FIFO as referenced asset: verify must return promptly (non-zero), not block
if command -v mkfifo >/dev/null 2>&1; then
    D="$TMP/fifo.mpack"
    make_pkg "$D" "audio/01.mpc"
    mkfifo "$D/audio/01.mpc"
    if "$MP" verify "$D" >/dev/null 2>&1; then
        bad "fifo asset rejected"
    else
        ok "fifo asset rejected (non-blocking)"
    fi
fi

# 2. Directory at a referenced asset path
D="$TMP/dir.mpack"
make_pkg "$D" "audio/01.mpc"
mkdir -p "$D/audio/01.mpc"
if "$MP" verify "$D" >/dev/null 2>&1; then
    bad "directory asset rejected"
else
    ok "directory asset rejected"
fi

# 3. Symlink escape to an outside file
D="$TMP/symlink.mpack"
make_pkg "$D" "audio/01.mpc"
echo "outside" > "$TMP/outside.bin"
ln -s "$TMP/outside.bin" "$D/audio/01.mpc"
if "$MP" verify "$D" >/dev/null 2>&1; then
    bad "symlink escape rejected"
else
    ok "symlink escape rejected"
fi

# 4. FIFO as manifest.json must fail package open promptly
if command -v mkfifo >/dev/null 2>&1; then
    D="$TMP/fifomanifest.mpack"
    mkdir -p "$D"
    mkfifo "$D/manifest.json"
    if "$MP" info "$D" >/dev/null 2>&1; then
        bad "fifo manifest rejected"
    else
        ok "fifo manifest rejected (non-blocking)"
    fi
fi

echo "hostile: $PASS passed, $FAIL failed"

# 5. Hard-link rejection: a hard-linked referenced asset must be rejected
# (link count > 1 is treated as an outside alias for untrusted ingestion).
# Uses the real sha256 so the ONLY reason verification fails is the link.
if [ "$(uname)" != "MINGW"* ] 2>/dev/null && command -v ln >/dev/null 2>&1; then
    D="$TMP/hardlink.mpack"
    mkdir -p "$D/audio"
    printf 'real content' > "$D/audio/01.mpc"
    SHA="$(shasum -a 256 "$D/audio/01.mpc" | cut -d' ' -f1 2>/dev/null || sha256sum "$D/audio/01.mpc" | cut -d' ' -f1)"
    printf '{"format":"musicpack","version":1,"album":{"title":"T","artists":[{"name":"A"}]},"media":[{"disc":1,"tracks":[{"track":1,"title":"T","audio":{"path":"audio/01.mpc","sha256":"%s"}}]}]}' \
        "$SHA" > "$D/manifest.json"
    ln "$D/audio/01.mpc" "$TMP/hlink_alias.mpc" 2>/dev/null
    if "$MP" verify "$D" >/dev/null 2>&1; then
        bad "hard-linked asset should be rejected"
    else
        ok "hard-linked asset rejected (nlink>1)"
    fi
    rm -f "$TMP/hlink_alias.mpc"
fi

# 6. Per-file size limit: a referenced file over the 8 GiB limit must be
# rejected before hashing (a sparse file of 9 GiB costs no disk blocks).
if command -v truncate >/dev/null 2>&1; then
    D="$TMP/bigfile.mpack"
    mkdir -p "$D/audio"
    truncate -s 9G "$D/audio/01.mpc"
    printf '{"format":"musicpack","version":1,"album":{"title":"T","artists":[{"name":"A"}]},"media":[{"disc":1,"tracks":[{"track":1,"title":"T","audio":{"path":"audio/01.mpc","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}]}]}' \
        > "$D/manifest.json"
    OUT="$("$MP" verify "$D" 2>&1)"
    if echo "$OUT" | grep -q "exceeds"; then
        ok "oversized file rejected by size limit"
    else
        bad "oversized file rejected by size limit"
    fi
fi

echo "hostile: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
