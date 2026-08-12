#!/usr/bin/env python3
"""Run required CTest names individually so missing gates cannot be masked."""
import argparse
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--config")
    parser.add_argument("tests", nargs="+")
    args = parser.parse_args()
    for test in args.tests:
        command = ["ctest", "--test-dir", args.build, "--no-tests=error",
                   "-R", f"^{test}$", "--output-on-failure"]
        if args.config:
            command[3:3] = ["-C", args.config]
        print(f"CI_TEST required={test}", flush=True)
        if subprocess.run(command).returncode:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
