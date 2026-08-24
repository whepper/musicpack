#!/usr/bin/env python3
"""Classify changed paths into MusicPack CI domains.

The classifier is deliberately conservative: known architectural boundaries get
narrow routing, while unknown code/build paths fail safe to the full validation
set. Documentation-only and other explicitly non-build paths remain cheap.
"""
from __future__ import annotations

import argparse
import sys

DOMAINS = ("codec", "core", "server", "author", "wasm", "web", "research")
ALL = set(DOMAINS)

IGNORED_PREFIXES = (".git/", "docs/", "doc/", "wiki/")
IGNORED_ROOT_FILES = {
    "README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md",
    "LICENSE", "LICENSE.txt", "LICENSE.md",
}

GLOBAL_ROOT_FILES = {"CMakeLists.txt", "CMakePresets.json", "Makefile"}

COMPONENT_RULES = {
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

    if top == "tests" and path == "tests/CMakeLists.txt":
        return set(ALL)

    if top in COMPONENT_RULES:
        return set(COMPONENT_RULES[top])

    if top == "bench":
        return {"codec"}

    if top == "tests":
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

    if top == "scripts":
        return set(ALL)

    # Unknown top-level code/build paths fail safe. This is intentional: a new
    # component must never silently bypass CI until its dependency mapping is added.
    return set(ALL)


def route(paths: list[str]) -> tuple[set[str], list[str]]:
    domains: set[str] = set()
    unknown: list[str] = []
    for path in paths:
        clean = path.strip().lstrip("./")
        result = classify(clean)
        domains.update(result)
        if not result and clean not in IGNORED_ROOT_FILES and not any(
            clean.startswith(p) for p in IGNORED_PREFIXES
        ):
            unknown.append(clean)
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
    if not paths and not sys.stdin.isatty():
        paths.extend(line.rstrip("\n") for line in sys.stdin if line.strip())

    domains, unknown = route(paths)
    for domain in DOMAINS:
        print(f"{domain}={'true' if domain in domains else 'false'}")
    print(f"unknown={'true' if unknown else 'false'}")
    for path in unknown:
        print(f"unknown_path={path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
