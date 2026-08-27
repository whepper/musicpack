#!/usr/bin/env python3
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
"""Regenerate the test-musicpack-album.mpack fixture's waveform envelopes.

One-shot developer helper. Reads the existing fixture, runs
`musicpack waveform-draft` against its already-Musepack-encoded tracks
(libmusicpack can decode Musepack through `musicpack_audio_*`), and
patches the manifest with the per-track `waveform` reference.

This produces the deterministic, committed waveform artifacts that the
rest of the test suite (server ingest, server endpoint, server
quarantine, web waveform parsing/render) consumes.

Usage:
  python3 tests/add_waveform_to_fixture.py \
      --musicpack build/core/musicpack/musicpack
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(REPO, "tests", "reference", "test-musicpack-album.mpack")
DRAFT_TMP = PKG + ".wf-draft.json"
STAGING = PKG + ".wf-staging"


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("FAIL %s: rc=%d\nSTDOUT:\n%s\nSTDERR:\n%s\n" %
                         (what, r.returncode, r.stdout, r.stderr))
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--musicpack", required=True)
    args = ap.parse_args()

    # Read the existing package's manifest as the draft skeleton.
    run([args.musicpack, "info", PKG, "--json"], "info existing pkg")
    info = json.loads(subprocess.run(
        [args.musicpack, "info", PKG, "--json"], capture_output=True, text=True).stdout)
    media_info = info["discs"][0]["tracks"]
    if any("waveform" in t for t in media_info):
        print(f"{PKG}: already has waveform, leaving as-is")
        return

    draft = {
        "schema": "musicpack-draft", "version": 1,
        "sourceRoot": PKG,
        "album": info["album"],
        "media": [{
            "disc": info["discs"][0]["disc"],
            "tracks": [{
                "track": i + 1,
                "title": t["title"],
                "audioPath": t["audio"],
            } for i, t in enumerate(media_info)],
        }],
        "artwork": [], "booklet": [], "lyrics": [], "extras": [],
    }
    with open(DRAFT_TMP, "w") as f:
        json.dump(draft, f)

    if os.path.exists(STAGING):
        subprocess.run(["rm", "-rf", STAGING])

    # waveform-draft is its own decode pass. It can read MPC through
    # libmusicpack's musepack backend; the output is the per-track
    # peak-rms-u8 envelope (from the same PCM bytes the decoder
    # surfaces for the player).
    r = subprocess.run([args.musicpack, "waveform-draft",
                        "--draft", DRAFT_TMP,
                        "-o", STAGING,
                        "--json"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("waveform-draft failed:\nSTDOUT:\n%s\nSTDERR:\n%s\n"
                         % (r.stdout, r.stderr))
        sys.exit(1)
    print(r.stdout)

    # Copy the .wfm files into analysis/waveform/ and patch manifest.json.
    wf_dir = os.path.join(PKG, "analysis", "waveform")
    os.makedirs(wf_dir, exist_ok=True)
    # waveform-draft writes to <staging>/waveform/<DD>-<TT>.wfm
    wf_source_dir = os.path.join(STAGING, "waveform")
    for i in range(len(media_info)):
        src_wf = os.path.join(wf_source_dir, "%02d-%02d.wfm" % (1, i + 1))
        dst_wf = os.path.join(wf_dir, "%02d-%02d.wfm" % (1, i + 1))
        if not os.path.exists(src_wf):
            sys.stderr.write(f"missing waveform for disc 1 track {i+1}\n")
            sys.exit(1)
        with open(src_wf, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        with open(dst_wf, "wb") as fo, open(src_wf, "rb") as fi:
            fo.write(fi.read())
        size = os.path.getsize(dst_wf)
        print(f"track {i+1}: {size} bytes, sha256 {sha}")

    manifest_path = os.path.join(PKG, "manifest.json")
    with open(manifest_path) as f:
        manifest = json.load(f)
    for di, disc in enumerate(manifest["media"]):
        for ti, track in enumerate(disc["tracks"]):
            wf_rel = "analysis/waveform/%02d-%02d.wfm" % (di + 1, ti + 1)
            wf_path = os.path.join(PKG, wf_rel)
            with open(wf_path, "rb") as f:
                sha = hashlib.sha256(f.read()).hexdigest()
            track["waveform"] = {
                "version": 1, "path": wf_rel, "sha256": sha,
                "intervalMs": 100, "encoding": "peak-rms-u8",
                "floorDb": -60,
                "points": os.path.getsize(wf_path) // 2,
            }
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")

    subprocess.run(["rm", "-rf", STAGING, DRAFT_TMP])
    print(f"\nregenerated waveform for {PKG}")


if __name__ == "__main__":
    main()