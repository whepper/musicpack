#!/usr/bin/env python3
"""Collect reproducible Phase 3 psychoacoustic CPU-share evidence."""

import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import tempfile
import time
import wave


def run(args):
    with wave.open(args.input, "rb") as wav:
        workload = {
            "path": os.path.basename(args.input),
            "sha256": hashlib.sha256(open(args.input, "rb").read()).hexdigest(),
            "sample_rate": wav.getframerate(),
            "channels": wav.getnchannels(),
            "samples_per_channel": wav.getnframes(),
            "audio_seconds": wav.getnframes() / wav.getframerate(),
        }

    qualities = [int(value) for value in args.qualities.split(",")]
    result = {
        "schema_version": 1,
        "commit": subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], text=True
        ).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"])),
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": {
            "os": platform.platform(),
            "arch": platform.machine(),
            "cpu": platform.processor(),
        },
        "binary_sha256": hashlib.sha256(open(args.mpcenc, "rb").read()).hexdigest(),
        "build": build_metadata(args.build),
        "workload": workload,
        "method": {
            "clock": "CLOCK_PROCESS_CPUTIME_ID",
            "warmup_runs": args.warmup,
            "measured_runs": args.runs,
        },
        "qualities": [],
    }

    for quality in qualities:
        runs = []
        for index in range(args.warmup + args.runs):
            with tempfile.TemporaryDirectory(prefix="mpc-psy-profile-") as tmp:
                output = os.path.join(tmp, "out.mpc")
                profile_path = os.path.join(tmp, "profile.json")
                env = dict(os.environ, MPC_PSY_PROFILE_OUT=profile_path)
                start = time.perf_counter_ns()
                process = subprocess.run(
                    [args.mpcenc, "--silent", "--overwrite", "--psy-impl", args.impl,
                     "--quality", str(quality), args.input, output],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env,
                )
                wall_ns = time.perf_counter_ns() - start
                if process.returncode != 0:
                    raise SystemExit("profile encode failed: " + process.stderr.strip())
                with open(profile_path, encoding="utf-8") as profile_file:
                    profile = json.load(profile_file)
                if index >= args.warmup:
                    total_psy = profile["model_ns"] + profile["raise_smr_ns"] + profile["ns_analyse_ns"]
                    powspec_nonfft = profile["spectrum_ns"] - profile["spectrum_fft_ns"]
                    other_psy = total_psy - profile["fft_ns"] - powspec_nonfft
                    profile.update({
                        "wall_ns": wall_ns,
                        "total_psy_ns": total_psy,
                        "powspec_nonfft_ns": powspec_nonfft,
                        "other_psy_ns": other_psy,
                        "fft_share_pct": profile["fft_ns"] * 100.0 / total_psy,
                        "powspec_nonfft_share_pct": powspec_nonfft * 100.0 / total_psy,
                        "other_psy_share_pct": other_psy * 100.0 / total_psy,
                        "psy_share_of_wall_pct": total_psy * 100.0 / wall_ns,
                        "fft_share_of_encoder_pct": profile["fft_ns"] * 100.0 / profile["total_cpu_ns"],
                        "psy_share_of_encoder_pct": total_psy * 100.0 / profile["total_cpu_ns"],
                    })
                    runs.append(profile)
        shares = [entry["fft_share_pct"] for entry in runs]
        median = {
            key: statistics.median(entry[key] for entry in runs)
            for key in ("wall_ns", "total_psy_ns", "fft_ns", "powspec_nonfft_ns",
                        "other_psy_ns", "fft_share_pct", "powspec_nonfft_share_pct",
                        "other_psy_share_pct", "psy_share_of_wall_pct",
                        "fft_share_of_encoder_pct", "psy_share_of_encoder_pct")
        }
        result["qualities"].append({
            "quality": quality,
            "psy_impl": args.impl,
            "runs": runs,
            "median": median,
            "fft_ge_35_percent_of_psy": min(shares) >= 35.0,
            "fft_ge_35_percent_of_encoder": min(
                entry["fft_share_of_encoder_pct"] for entry in runs) >= 35.0,
            "fft_share_range_pct": [min(shares), max(shares)],
        })
    return result


def build_metadata(build):
    if not build:
        return {}
    script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "scripts", "ci_config.py")
    output = subprocess.check_output(
        ["python3", script, "--build", build, "--role", "benchmark-psy-profile",
         "--selected-config", "Release", "--format", "json"], text=True)
    return json.loads(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpcenc", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--impl", choices=("scalar", "simd"), default="scalar")
    parser.add_argument("--qualities", default="5,6,7")
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--build")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    result = run(args)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    for quality in result["qualities"]:
        median = quality["median"]
        print("q%d FFT/psy %.2f%%, FFT/encoder %.2f%%, PowSpec/window %.2f%%, other psy %.2f%%, psy/encoder %.2f%%, encoder >=35%% %s" %
              (quality["quality"], median["fft_share_pct"],
               median["fft_share_of_encoder_pct"],
               median["powspec_nonfft_share_pct"], median["other_psy_share_pct"],
               median["psy_share_of_encoder_pct"],
               "YES" if quality["fft_ge_35_percent_of_encoder"] else "NO"))


if __name__ == "__main__":
    main()
