#!/usr/bin/env bash
set -euo pipefail

# Regression tests for the path-to-domain routing contract in .github/workflows/ci.yml.
# The router is intentionally shell/regex based; keep this matrix aligned with the
# repository's architectural boundaries. These tests exercise the routing expressions
# without needing GitHub Actions.

codec=false; core=false; server=false; author=false; wasm=false; web=false; research=false
files=''

match() { printf '%s\n' "$files" | grep -Eq "$1"; }
route() {
  codec=false; core=false; server=false; author=false; wasm=false; web=false; research=false
  if match '^(libmpcdec|libmpcenc|libmpcpsy|libwavformat|mpcdec|mpcenc|mpc2sv8|mpccut|wavcmp|mpcgain|mpcchap)/' || match '^common/' || match '^include/(mpc|musepack)/' || match '^tests/(codec|fixtures|fuzz|patch_reference\.py)'; then codec=true; fi
  if match '^(libmusicpack|musicpack)/' || match '^tests/(mpack|integration|mpack_)'; then core=true; fi
  if match '^libmusicpack/' || match '^server/' || match '^tests/server_'; then server=true; fi
  if match '^libmusicpack/' || match '^author/' || match '^tests/(author_|mpack_integration)'; then author=true; fi
  if match '^libmpcdec/' || match '^common/' || match '^include/(mpc|musepack)/' || match '^wasm/' || match '^tests/wasm/'; then wasm=true; fi
  if match '^web/' || match '^tests/fixtures/' || match '^(libmpcdec|common|wasm|server|libmusicpack)/'; then web=true; fi
  if match '^research/'; then research=true; fi
  if match '^CMakeLists\.txt$' || match '^scripts/ci_[^/]*$' || match '^\.github/workflows/'; then
    codec=true; core=true; server=true; author=true; wasm=true; web=true; research=true
  fi
}

assert_routes() {
  local name="$1" expected="$2"; shift 2
  files="$(printf '%s\n' "$@")"
  route
  actual=""
  for d in codec core server author wasm web research; do
    if [[ "${!d}" == true ]]; then actual+=" $d"; fi
  done
  actual="${actual# }"
  [[ "$actual" == "$expected" ]] || {
    echo "FAIL: $name"
    echo "  files: $files"
    echo "  expected: '$expected'"
    echo "  actual:   '$actual'"
    exit 1
  }
  echo "PASS: $name"
}

assert_routes "web" "web" web/src/App.ts
assert_routes "player-core" "web" web/player-core/src/player.ts
assert_routes "encoder" "codec" libmpcenc/src/encoder.c
assert_routes "decoder" "codec wasm web" libmpcdec/src/decoder.c
assert_routes "musicpack-core" "core server author web" libmusicpack/src/package.c
assert_routes "server" "server web" server/src/api.c
assert_routes "author" "author" author/app/src/main.ts
assert_routes "research" "research" research/test_sonic.py
assert_routes "sonic" "" sonic/frontend.c
assert_routes "sonic-cmake" "" sonic/CMakeLists.txt
assert_routes "root-cmake" "author codec core research server wasm web" CMakeLists.txt
assert_routes "tests-cmake" "" tests/CMakeLists.txt
assert_routes "ci-script" "author codec core research server wasm web" scripts/ci_config.py
assert_routes "workflow" "author codec core research server wasm web" .github/workflows/web.yml

# Multi-file dependency propagation.
assert_routes "decoder + web" "codec wasm web" libmpcdec/src/decoder.c web/src/App.ts
assert_routes "core + research" "core research" libmusicpack/src/package.c research/test_sonic.py

echo "All router regression tests passed."
