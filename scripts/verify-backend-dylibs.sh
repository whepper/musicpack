#!/usr/bin/env bash
# Verify that a binary references only macOS system dynamic libraries.
#
# MusicPack Author ships its authoring backend fully static so the .app needs
# nothing but the OS. This gate fails the build if the backend ever picks up
# a Homebrew / local / third-party dependency (e.g. @rpath/libmusicpack.dylib).
#
# Usage: scripts/verify-backend-dylibs.sh <binary>

set -euo pipefail

BIN="${1:?binary path}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
OTOOL="$(command -v otool || echo /usr/bin/otool)"

deps="$("$OTOOL" -L "$BIN" | sed '1d' | awk '{print $1}')"

# A fully static Mach-O may have no dynamic dependencies at all (then we are
# done). Realistically the CLI links libSystem, which is fine.
if [ -z "$deps" ]; then
  echo "ok: $(basename "$BIN") has no dynamic library dependencies"
  exit 0
fi

FAIL=0
while IFS= read -r dep; do
  case "$dep" in
    /usr/lib/* | /System/Library/*) ;;
    *)
      echo "error: unexpected non-system dependency: $dep" >&2
      FAIL=1
      ;;
  esac
done <<< "$deps"

if [ "$FAIL" -ne 0 ]; then
  echo "error: $(basename "$BIN") depends on non-system libraries:" >&2
  "$OTOOL" -L "$BIN" >&2
  exit 1
fi

# Belt and braces: the two paths a Homebrew/local toolchain would leak.
if "$OTOOL" -L "$BIN" | grep -Eq '/opt/homebrew|/usr/local'; then
  echo "error: $(basename "$BIN") references a Homebrew/local path" >&2
  exit 1
fi

echo "ok: $(basename "$BIN") references only macOS system libraries"
