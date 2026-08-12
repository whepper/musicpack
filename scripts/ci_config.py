#!/usr/bin/env python3
"""Emit a concise, greppable CMake configuration summary for CI."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import subprocess


def cache_values(build):
    values = {}
    cache = build / "CMakeCache.txt"
    if not cache.exists():
        return values
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def compiler_values(build, values):
    compiler = values.get("CMAKE_C_COMPILER", "")
    compiler_files = sorted(build.glob("CMakeFiles/*/CMakeCCompiler.cmake"))
    if not compiler_files:
        return compiler, "", values.get("CMAKE_C_COMPILER_VERSION", "")
    content = compiler_files[-1].read_text(encoding="utf-8", errors="replace")
    if not compiler:
        match = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)', content)
        compiler = match.group(1) if match else ""
    match = re.search(r'set\(CMAKE_C_COMPILER_ID "([^"]+)', content)
    compiler_id = match.group(1) if match else ""
    match = re.search(r'set\(CMAKE_C_COMPILER_VERSION "([^"]+)', content)
    return compiler, compiler_id, match.group(1) if match else ""


def command_version(command, compiler_id, configured_version):
    if configured_version:
        return configured_version
    if not command or compiler_id == "MSVC":
        return "unknown"
    try:
        return subprocess.check_output([command, "--version"], text=True,
                                       stderr=subprocess.STDOUT).splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "missing"


def tree_digest(path):
    root = pathlib.Path(path)
    digest_value = hashlib.sha256()
    for item in sorted((entry for entry in root.rglob("*") if entry.is_file()),
                       key=lambda entry: entry.relative_to(root).as_posix()):
        name = item.relative_to(root).as_posix().encode()
        digest_value.update(len(name).to_bytes(4, "big"))
        digest_value.update(name)
        with item.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest_value.update(chunk)
    return digest_value.hexdigest()


def git_value(*args):
    try:
        return subprocess.check_output(["git", *args], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def cpu_name():
    if platform.system() == "Darwin":
        try:
            return subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
        except (OSError, subprocess.CalledProcessError):
            return "unknown"
    try:
        for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def cmake_version():
    try:
        return subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--reference", action="store_true")
    parser.add_argument("--executable")
    parser.add_argument("--selected-config", default="")
    parser.add_argument("--expect", action="append", default=[], metavar="FIELD=VALUE")
    parser.add_argument("--compare-build")
    parser.add_argument("--compare-field", action="append", default=[])
    parser.add_argument("--format", choices=("ci", "metadata", "json"), default="ci")
    parser.add_argument("--corpus")
    args = parser.parse_args()
    build = pathlib.Path(args.build)
    values = cache_values(build)
    compiler, compiler_id, compiler_version = compiler_values(build, values)
    multi_config = bool(values.get("CMAKE_CONFIGURATION_TYPES", ""))
    selected_config = args.selected_config or ("Release" if not multi_config else "")
    if multi_config:
        effective_config = selected_config or "<not-selected>"
    else:
        effective_config = values.get("CMAKE_BUILD_TYPE", "")
    fields = {
        "role": args.role,
        "os": platform.system(),
        "arch": platform.machine(),
        "compiler": compiler or "unknown",
        "compiler_id": compiler_id or "unknown",
        "compiler_version": command_version(compiler, compiler_id, compiler_version),
        "generator_mode": "multi-config" if multi_config else "single-config",
        "cmake_build_type": values.get("CMAKE_BUILD_TYPE", "") if not multi_config else "<not-applicable>",
        "cmake_configurations": values.get("CMAKE_CONFIGURATION_TYPES", ""),
        "selected_configuration": effective_config,
        "simd_option": values.get("MPC_ENABLE_SIMD", ""),
        "decoder_simd_enabled": values.get("MPC_DECODER_SIMD_ENABLED", ""),
        "decoder_simd_backend": values.get("MPC_DECODER_SIMD_BACKEND", ""),
        "wasm_simd": values.get("MPC_WASM_SIMD", ""),
        "wasm_test_hooks": values.get("MPC_WASM_TEST_HOOKS", ""),
        "native_tuning": values.get("MPC_ENABLE_NATIVE_TUNING", ""),
        "psy_profile": values.get("MPC_ENABLE_PSY_PROFILE", ""),
        "tests": values.get("MPC_BUILD_TESTS", ""),
    }
    if args.reference:
        fields["reference"] = "05d97a5"
        patch = pathlib.Path("tests/patch_reference.py")
        fields["reference_patch_sha256"] = digest(patch)
    if args.executable:
        executable = pathlib.Path(args.executable)
        fields["executable"] = str(executable)
        fields["executable_sha256"] = digest(executable)
    if args.corpus:
        fields["corpus"] = str(pathlib.Path(args.corpus))
        fields["corpus_sha256"] = tree_digest(args.corpus)
    if args.format == "metadata":
        fields.update({
            "commit": git_value("rev-parse", "HEAD"),
            "os": platform.platform(),
            "cpu": cpu_name(),
            "cmake_version": cmake_version(),
        })
    failures = []
    for item in args.expect:
        if "=" not in item:
            parser.error(f"invalid --expect {item!r}; use FIELD=VALUE")
        key, expected = item.split("=", 1)
        actual = str(fields.get(key, "<missing>"))
        if actual != expected:
            failures.append(f"{key}: expected {expected!r}, got {actual!r}")
    if args.compare_build:
        other_values = cache_values(pathlib.Path(args.compare_build))
        other_compiler, other_id, other_version = compiler_values(pathlib.Path(args.compare_build), other_values)
        comparable = {"compiler": compiler, "compiler_id": compiler_id,
                      "compiler_version": command_version(compiler, compiler_id, compiler_version)}
        other = {"compiler": other_compiler, "compiler_id": other_id,
                 "compiler_version": command_version(other_compiler, other_id, other_version)}
        for key in args.compare_field:
            if key not in comparable:
                parser.error(f"unsupported --compare-field {key!r}")
            if comparable[key] != other[key]:
                failures.append(f"{key}: build={comparable[key]!r}, compare-build={other[key]!r}")
    if args.format == "json":
        print(json.dumps(fields, sort_keys=True))
    else:
        prefix = "#" if args.format == "metadata" else "CI_CONFIG"
        for key, value in fields.items():
            print(f"{prefix} {key}: {value}" if args.format == "metadata" else f"{prefix} {key}={value}")
    for failure in failures:
        print(f"CI_CONFIG ASSERTION FAILED: {failure}", file=os.sys.stderr)
    if failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
