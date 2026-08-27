#!/usr/bin/env python3
"""Regression matrix for scripts/ci_router.py."""
from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ci_router", ROOT / "scripts/ci_router.py")
assert SPEC and SPEC.loader
router = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(router)


def check(name: str, expected: set[str], *paths: str) -> None:
    actual, unknown = router.route(list(paths))
    assert not unknown, f"{name}: unexpected unknown paths: {unknown}"
    assert actual == expected, f"{name}: expected {sorted(expected)}, got {sorted(actual)}"
    print(f"PASS {name}: {sorted(actual)}")


check("docs", set(), "docs/ci.md")
check("root-readme", set(), "README.md")
check("web", {"web"}, "web/src/App.ts")
check("player-core", {"web"}, "web/player-core/src/player.ts")
check("encoder", {"codec"}, "codec/libmpcenc/src/encoder.c")
check("decoder", {"codec", "wasm", "web"}, "codec/libmpcdec/src/decoder.c")
check("decoder-cmake", {"codec", "wasm", "web"}, "codec/libmpcdec/CMakeLists.txt")
check("musicpack-core", {"core", "server", "author", "web"}, "core/libmusicpack/src/package.c")
check("musicpack-cmake", {"core", "server", "author", "web"}, "core/libmusicpack/CMakeLists.txt")
check("server", {"server", "web"}, "server/src/api.c")
check("server-cmake", {"server", "web"}, "server/CMakeLists.txt")
check("author", {"author"}, "author/app/src/main.ts")
check("research", {"research"}, "research/test_sonic.py")
check("sonic", {"core"}, "core/sonic/frontend.c")
check("sonic-cmake", {"core"}, "core/sonic/CMakeLists.txt")
check("root-cmake", set(router.ALL), "CMakeLists.txt")
check("tests-cmake", set(router.ALL), "tests/CMakeLists.txt")
check("ci-script", set(router.ALL), "scripts/ci_config.py")
check("workflow", set(router.ALL), ".github/workflows/web.yml")
check("bench", {"codec"}, "bench/run_bench.sh")
check("decoder-plus-web", {"codec", "wasm", "web"}, "codec/libmpcdec/src/decoder.c", "web/src/App.ts")
check("core-plus-research", {"core", "server", "author", "web", "research"}, "core/libmusicpack/src/package.c", "research/test_sonic.py")
check("unknown-top-level", set(router.ALL), "new-component/foo.c")
check("unknown-root-build-file", set(router.ALL), "toolchain.toml")

print("All CI router regression tests passed.")
