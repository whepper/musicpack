#!/usr/bin/env python3
"""Run generated MusicPack v1 conformance cases with explicit expectations."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile


def invoke(command, verb, package):
    return subprocess.run([command, verb, package], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("musicpack")
    parser.add_argument("generator")
    args = parser.parse_args()
    root = tempfile.mkdtemp(prefix="mpack-conformance-")
    try:
        subprocess.run([sys.executable, args.generator, root], check=True)
        with open(os.path.join(root, "cases.json"), encoding="utf-8") as f:
            cases = json.load(f)
        failures = []
        for name in cases["valid"]:
            package = os.path.join(root, name + ".mpack")
            if invoke(args.musicpack, "info", package) != 0:
                failures.append("valid %s: info failed" % name)
            if invoke(args.musicpack, "verify", package) != 0:
                failures.append("valid %s: verify failed" % name)
        for name in cases["invalid_manifest"]:
            package = os.path.join(root, name + ".mpack")
            if invoke(args.musicpack, "info", package) == 0:
                failures.append("invalid manifest %s: info succeeded" % name)
            if invoke(args.musicpack, "verify", package) == 0:
                failures.append("invalid manifest %s: verify succeeded" % name)
        for name in cases["invalid_verify"]:
            package = os.path.join(root, name + ".mpack")
            if invoke(args.musicpack, "verify", package) == 0:
                failures.append("invalid asset %s: verify succeeded" % name)
        if failures:
            print("\n".join("FAIL " + item for item in failures), file=sys.stderr)
            return 1
        print("mpack conformance: %d valid, %d invalid manifests, %d invalid assets" %
              (len(cases["valid"]), len(cases["invalid_manifest"]), len(cases["invalid_verify"])))
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
