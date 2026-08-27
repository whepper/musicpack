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

# Top-level domain mapping. For nested paths, NESTED_RULES takes precedence
# (checked first) so that e.g. codec/libmpcdec routes to {codec, wasm, web}
# while codec/libmpcenc routes to {codec} only.
COMPONENT_RULES = {
    "codec": {"codec", "wasm", "web"},
    "core": {"core", "server", "author", "web"},
    "server": {"server", "web"},
    "author": {"author"},
    "wasm": {"wasm", "web"},
    "web": {"web"},
    "research": {"research"},
    "platform": {"codec"},
}

NESTED_RULES = {
    "codec/libmpcdec": {"codec", "wasm", "web"},
    "codec/libmpcenc": {"codec"},
    "codec/libmpcpsy": {"codec"},
    "codec/libwavformat": {"codec"},
    "codec/common": {"codec", "wasm", "web"},
    "codec/include": {"codec", "wasm", "web"},
    "codec/mpcdec": {"codec"},
    "codec/mpcenc": {"codec"},
    "codec/mpc2sv8": {"codec"},
    "codec/mpccut": {"codec"},
    "codec/wavcmp": {"codec"},
    "codec/mpcgain": {"codec"},
    "codec/mpcchap": {"codec"},
    "core/libmusicpack": {"core", "server", "author", "web"},
    "core/musicpack": {"core", "server", "author", "web"},
    "core/sonic": {"core"},
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

    # Check nested (two-component) prefixes first for precise routing.
    two = path.split("/", 2)[0] + "/" + path.split("/", 2)[1] if "/" in path else None
    if two and two in NESTED_RULES:
        return set(NESTED_RULES[two])

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
