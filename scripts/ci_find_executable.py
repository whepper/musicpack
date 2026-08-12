#!/usr/bin/env python3
"""Locate exactly one expected executable under a build tree."""
import argparse
import os
import pathlib
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("name")
    args = parser.parse_args()
    matches = sorted(path for path in pathlib.Path(args.root).rglob(args.name)
                     if path.is_file() and os.access(path, os.X_OK))
    if len(matches) != 1:
        print(f"expected one {args.name} below {args.root}, found {len(matches)}", file=sys.stderr)
        return 1
    print(matches[0].resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
