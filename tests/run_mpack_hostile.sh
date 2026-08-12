#!/usr/bin/env bash
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
[ "$FAIL" -eq 0 ]
