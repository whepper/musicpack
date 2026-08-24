#!/usr/bin/env python3
"""Classify changed paths into MusicPack CI domains.

The classifier is deliberately conservative: known architectural boundaries get
narrow routing, while unknown code/build paths fail safe to the full validation
set. Documentation-only and other explicitly non-build paths remain cheap.
"""
from __future__ import annotations

import argparse
from pathlib import PurePosixPath

DOMAINS = ("codec", "core", "server", "author", "wasm", "web", "research")
ALL = set(DOMAINS)

# Explicitly non-executable repository areas. Add new documentation/content areas
# here rather than accidentally making them trigger the full matrix.
IGNORED_PREFIXES = (
    ".git/",
    "docs/",
    "doc/",
    "wiki/",
)
IGNORED_ROOT_FILES = {
    "README.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "LICENSE",
    "LICENSE.txt",
    "LICENSE.md",
}

KNOWN_TOP_LEVEL = {
    ".github",
    "author",
    "bench",
    "common",
    "include",
    "libmpcdec",
    "libmpcenc",
    "libmpcpsy",
    "libwavformat",
    "libmusicpack",
    "musicpack",
    "mpcdec",
    "mpcenc",
    "mpc2sv8",
    "mpccut",
    "mpcgain",
    "mpcchap",
    "wavcmp",
    "server",
    "sonic",
    "tests",
    "wasm",
    "web",
    "research",
    "scripts",
}

GLOBAL_ROOT_FILES = {
    "CMakeLists.txt",
    "CMakePresets.json",
    "Makefile",
}


def classify(path: str) -> set[str]:
    path = path.strip().lstrip("./")
    if not path:
        return set()
    if path in IGNORED_ROOT_FILES or any(path.startswith(p) for p in IGNORED_PREFIXES):
        return set()
    if path in GLOBAL_ROOT_FILES:
        return set(ALL)
    if path.startswith(".github/workflows/") or (
        path.startswith("scripts/") and path.rsplit("/", 1)[-1].startswith("ci_")
    ):
        return set(ALL)

    top = path.split("/", 1)[0]

    # Test build registration is shared by native, WASM and web harnesses. A
    # change here can alter which tests exist in several domain workflows.
    if top == "tests" and path == "tests/CMakeLists.txt":
        return set(ALL)

    # Component-specific build files are routed by the component they describe.
    # They must not be treated as global CMake changes.
    component_rules = {
        "libmpcdec": {"codec", "wasm", "web"},
        "libmpcenc": {"codec"},
        "libmpcpsy": {"codec"},
        "libwavformat": {"codec"},
        "libmusicpack": {"core", "server", "author", "web"},
        "musicpack": {"core", "server", "author", "web"},
        "server": {"server", "web"},
        "sonic": {"core"},
        "author": {"author"},
        "wasm": {"wasm", "web"},
        "web": {"web"},
        "research": {"research"},
        "common": {"codec", "wasm", "web"},
        "include": {"codec", "wasm", "web"},
        "mpcdec": {"codec"},
        "mpcenc": {"codec"},
        "mpc2sv8": {"codec"},
        "mpccut": {"codec"},
        "wavcmp": {"codec"},
        "mpcgain": {"codec"},
        "mpcchap": {"codec"},
    }
    if top in component_rules:
        domains = set(component_rules[top])
        # The codec test fixtures are shared by browser integration tests.
        if top == "web":
            domains.add("web")
        return domains

    if top == "bench":
        return {"codec"}

    if top == "tests":
        # Existing test files are routed by their test family. Unknown test files
        # are deliberately fail-safe because CTest registration can be indirect.
        if path.startswith(("tests/wasm/", "tests/node/")):
            return {"wasm", "web"}
        if path.startswith("tests/server_"):
            return {"server", "web"}
        if path.startswith(("tests/author_", "tests/mpack_integration")):
            return {"author", "core"}
        if path.startswith(("tests/mpack", "tests/integration")):
            return {"core", "server", "author", "web"}
        if path.startswith(("tests/fixtures/", "tests/fuzz/")):
            return {"codec", "core", "wasm", "web"}
        return set(ALL)

    # Root scripts that are not explicitly CI scripts may affect build/test
    # generation. Fail safe rather than silently omitting validation.
    if top == "scripts":
        return set(ALL)

    # Unknown top-level content: documentation was handled above. Everything else
    # is assumed potentially executable/build-relevant and triggers full validation.
    if top not in KNOWN_TOP_LEVEL:
        return set(ALL)

    # Defensive fallback for future paths under a known component that were not
    # matched by a more specific rule.
    return set(ALL)


def route(paths: list[str]) -> tuple[set[str], list[str]]:
    domains: set[str] = set()
    unknown: list[str] = []
    for path in paths:
        before = domains.copy()
        result = classify(path)
        domains.update(result)
        if not result and path.strip() not in IGNORED_ROOT_FILES and not any(
            path.strip().startswith(p) for p in IGNORED_PREFIXES
        ):
            # Explicitly known non-build files are fine; a path with no route is
            # otherwise suspicious and should fail safe.
            top = path.strip().lstrip("./").split("/", 1)[0]
            if top not in {"docs", "doc", "wiki"}:
                unknown.append(path)
    if unknown:
        domains = set(ALL)
    return domains, unknown


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--file", action="append", dest="files")
    args = parser.parse_args()
    paths = list(args.paths)
    if args.files:
        paths.extend(args.files)
    domains, unknown = route(paths)
    for domain in DOMAINS:
        print(f"{domain}={'true' if domain in domains else 'false'}")
    if unknown:
        print("unknown=true")
        for path in unknown:
            print(f"unknown_path={path}")
    else:
        print("unknown=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
