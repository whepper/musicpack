#!/usr/bin/env python3
"""Emit a concise, greppable CMake configuration summary for CI."""
import argparse
import hashlib
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
        return compiler, ""
    content = compiler_files[-1].read_text(encoding="utf-8", errors="replace")
    if not compiler:
        match = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)', content)
        compiler = match.group(1) if match else ""
    match = re.search(r'set\(CMAKE_C_COMPILER_ID "([^"]+)', content)
    return compiler, match.group(1) if match else ""


def version(command):
    if not command:
        return "unknown"
    try:
        return subprocess.check_output([command, "--version"], text=True,
                                       stderr=subprocess.STDOUT).splitlines()[0]
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "missing"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--reference", action="store_true")
    parser.add_argument("--executable")
    args = parser.parse_args()
    build = pathlib.Path(args.build)
    values = cache_values(build)
    compiler, compiler_id = compiler_values(build, values)
    fields = {
        "role": args.role,
        "os": platform.system(),
        "arch": platform.machine(),
        "compiler": compiler or "unknown",
        "compiler_id": compiler_id or "unknown",
        "compiler_version": version(compiler),
        "cmake_build_type": values.get("CMAKE_BUILD_TYPE", "<multi-config>"),
        "cmake_configurations": values.get("CMAKE_CONFIGURATION_TYPES", ""),
        "simd_option": values.get("MPC_ENABLE_SIMD", ""),
        "decoder_simd_enabled": values.get("MPC_DECODER_SIMD_ENABLED", ""),
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
    for key, value in fields.items():
        print(f"CI_CONFIG {key}={value}")


if __name__ == "__main__":
    main()
