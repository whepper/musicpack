#!/usr/bin/env python3
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause
"""Generate a compact, deterministic MusicPack v1 conformance corpus."""

import argparse
import copy
import hashlib
import json
import os
import shutil
import sys


HASH = lambda data: hashlib.sha256(data).hexdigest()


def base_manifest(path="audio/01.bin", digest=None):
    return {
        "format": "musicpack", "version": 1,
        "album": {"title": "Conformance", "artists": [{"name": "Tester"}]},
        "media": [{"disc": 1, "tracks": [{"track": 1, "title": "One",
                   "audio": {"path": path, "sha256": digest or HASH(b"one")}}]}],
    }


def write_package(root, manifest, files=None):
    os.makedirs(root)
    for path, data in (files or {}).items():
        full = os.path.join(root, *path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    with open(os.path.join(root, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")


def write_raw_manifest(root, content, files=None):
    os.makedirs(root)
    for path, data in (files or {}).items():
        full = os.path.join(root, *path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    with open(os.path.join(root, "manifest.json"), "wb") as f:
        f.write(content)


def add_case(cases, group, name, manifest, files=None):
    write_package(os.path.join(cases["root"], name + ".mpack"), manifest, files)
    cases[group].append(name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    args = parser.parse_args()
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    cases = {"root": out, "valid": [], "invalid_manifest": [], "invalid_verify": []}

    one = {"audio/01.bin": b"one"}
    add_case(cases, "valid", "minimal", base_manifest(), one)

    files = {
        "audio/01.bin": b"one", "audio/02.bin": b"two", "audio/03.bin": b"three",
        "artwork/front.bin": b"front", "booklet/booklet.txt": b"booklet",
        "lyrics/01.txt": b"lyrics", "extras/notes.txt": b"notes",
        "analysis/external.json": b"{}",
    }
    complete = base_manifest()
    complete.update({
        "album": {"title": "Complete", "artists": [{"name": "Alpha", "role": "main"},
                  {"name": "Beta"}], "releaseType": "compilation",
                  "originalReleaseDate": "2001-02-03", "genres": ["Rock", "Pop"]},
        "release": {"releaseDate": "2020-01-02", "edition": "Deluxe", "country": "GB",
                    "label": "Label", "catalogueNumber": "CAT-1", "notes": "Notes"},
        "identifiers": {"musicbrainzReleaseGroupId": "group", "musicbrainzReleaseId": "release",
                        "barcode": "123"},
        "identity": {"source": "local", "confidence": "none"},
        "source": {"type": "digital-download", "store": "Store", "sourceId": "album-id"},
        "media": [
            {"disc": 1, "format": "Digital", "title": "Main", "tracks": [
                {"track": 1, "title": "One", "artists": [{"name": "Alpha"}],
                 "identifiers": {"isrc": "ISRC", "musicbrainzTrackId": "track",
                                 "musicbrainzRecordingId": "recording"},
                 "source": {"store": "Store", "trackId": "one"},
                 "sourceAudio": {"codec": "flac", "md5": "abc"}, "duration": 1.5,
                 "loudness": {"trackLUFS": -12.0, "truePeakDbTP": -1.0},
                 "audio": {"path": "audio/01.bin", "sha256": HASH(files["audio/01.bin"]),
                           "codec": "test"}},
                {"track": 2, "title": "Two", "audio": {"path": "audio/02.bin",
                 "sha256": HASH(files["audio/02.bin"])}}]},
            {"disc": 2, "format": "CD", "tracks": [{"track": 1, "title": "Three",
             "audio": {"path": "audio/03.bin", "sha256": HASH(files["audio/03.bin"])}}]}],
        "artwork": [{"role": "front", "path": "artwork/front.bin",
                     "sha256": HASH(files["artwork/front.bin"])}],
        "booklet": [{"path": "booklet/booklet.txt", "sha256": HASH(files["booklet/booklet.txt"])}],
        "lyrics": [{"path": "lyrics/01.txt", "sha256": HASH(files["lyrics/01.txt"])}],
        "extras": [{"path": "extras/notes.txt", "sha256": HASH(files["extras/notes.txt"])}],
        "analysis": [{"type": "future-analysis", "path": "analysis/external.json",
                      "sha256": HASH(files["analysis/external.json"])}],
        "loudness": {"algorithm": "ITU-R BS.1770-5", "albumLUFS": -12.0,
                     "albumTruePeakDbTP": -1.0},
        "provenance": {"tool": "conformance", "toolVersion": "1"},
        "xFutureField": {"preserved": True},
    })
    add_case(cases, "valid", "complete", complete, files)

    unicode_files = {"audio/01-unicode.bin": b"unicode", "artwork/back.bin": b"back"}
    unicode_manifest = base_manifest("audio/01-unicode.bin", HASH(b"unicode"))
    unicode_manifest["album"] = {"title": "Cafe\u0301", "artists": [{"name": "Beyonce\u0301"}]}
    unicode_manifest["artwork"] = [{"role": "front", "path": "artwork/back.bin",
                                    "sha256": HASH(b"back")}]
    add_case(cases, "valid", "unicode", unicode_manifest, unicode_files)

    # ---- credit anchors (Phase 2A: optional musicbrainzId / sortName) ----
    anchors = base_manifest()
    anchors["album"]["artists"] = [
        {"name": "Alpha", "role": "main", "sortName": "Alpha",
         "musicbrainzId": "5441c29d-3602-4898-b1a1-b77fa23b8e50"},
        {"name": "Beta"},
    ]
    anchors["media"][0]["tracks"][0]["artists"] = [
        {"name": "Gamma", "musicbrainzId": "70b2a40e-8f4d-4c6b-b6ce-8f1e0a6dc3ba"},
    ]
    add_case(cases, "valid", "credit-anchors", anchors, one)

    # ---- waveform envelope (peak-rms-u8, 100 ms, -60 dBFS floor) ---------
    # Valid: a track with a correctly-sized .wfm payload (20 bytes for 10 buckets).
    waveform_files = {
        **one,
        "analysis/waveform/01-01.wfm": b"\x00" * 20,
    }
    waveform_manifest = base_manifest()
    waveform_manifest["media"][0]["tracks"][0]["waveform"] = {
        "version": 1,
        "path": "analysis/waveform/01-01.wfm",
        "sha256": HASH(b"\x00" * 20),
        "intervalMs": 100,
        "encoding": "peak-rms-u8",
        "floorDb": -60,
        "points": 10,
    }
    add_case(cases, "valid", "with-waveform", waveform_manifest, waveform_files)

    # ---- alternate representations (Phase 3: track.representations[]) -----
    reps_files = {**one, "audio/01-alt.bin": b"alt"}
    reps_manifest = base_manifest()
    reps_manifest["media"][0]["tracks"][0]["representations"] = [
        {"path": "audio/01-alt.bin", "sha256": HASH(b"alt"),
         "label": "FLAC 24/96", "codec": "flac"},
    ]
    add_case(cases, "valid", "with-representations", reps_manifest, reps_files)

    rep_mutations = {
        # missing checksum on the representation entry
        "rep-missing-sha": lambda m: m["media"][0]["tracks"][0].update(
            representations=[{"path": "audio/01-alt.bin"}]),
        # representation path collides with the primary audio object
        "rep-dup-path-primary": lambda m: m["media"][0]["tracks"][0].update(
            representations=[{"path": "audio/01.bin",
                              "sha256": HASH(b"one")}]),
        # two representations sharing one path
        "rep-dup-path-self": lambda m: m["media"][0]["tracks"][0].update(
            representations=[
                {"path": "audio/01-alt.bin", "sha256": HASH(b"alt")},
                {"path": "audio/01-alt.bin", "sha256": HASH(b"two")}]),
        # traversal in a representation path
        "rep-traversal": lambda m: m["media"][0]["tracks"][0].update(
            representations=[{"path": "../evil.bin", "sha256": HASH(b"x")}]),
    }
    for name, mutate in rep_mutations.items():
        manifest = base_manifest()
        mutate(manifest)
        add_case(cases, "invalid_manifest", name, manifest, one)

    malformed = os.path.join(out, "malformed-json.mpack")
    os.makedirs(malformed)
    with open(os.path.join(malformed, "manifest.json"), "w", encoding="utf-8") as f:
        f.write("{")
    cases["invalid_manifest"].append("malformed-json")

    mutations = {
        "wrong-format": lambda m: m.update(format="other"),
        "unsupported-version": lambda m: m.update(version=2),
        "missing-album": lambda m: m.pop("album"),
        "empty-artists": lambda m: m["album"].update(artists=[]),
        "bad-release-type": lambda m: m["album"].update(releaseType="mixtape"),
        "bad-medium-format": lambda m: m["media"][0].update(format="DAT"),
        "duplicate-disc": lambda m: m["media"].append(copy.deepcopy(m["media"][0])),
        "duplicate-track": lambda m: m["media"][0]["tracks"].append(copy.deepcopy(m["media"][0]["tracks"][0])),
        "bad-checksum-form": lambda m: m["media"][0]["tracks"][0]["audio"].update(sha256="A" * 64),
        "missing-audio-checksum": lambda m: m["media"][0]["tracks"][0]["audio"].pop("sha256"),
        "bad-duration": lambda m: m["media"][0]["tracks"][0].update(duration=0),
        "bad-loudness": lambda m: m["media"][0]["tracks"][0].update(loudness={"trackLUFS": -9999, "truePeakDbTP": 0}),
        "sonic-without-profile": lambda m: m.update(analysis=[{"type": "sonic", "path": "analysis/a", "sha256": HASH(b"a")}]),
        "duplicate-asset-path": lambda m: m.update(extras=[{"path": "audio/01.bin", "sha256": HASH(b"one")}]),
        "wrong-track-object": lambda m: m["media"][0].update(tracks=[42]),
        "wrong-media-object": lambda m: m.update(media=[42]),
        "wrong-audio-object": lambda m: m["media"][0]["tracks"][0].update(audio=42),
        "wrong-artists-object": lambda m: m["album"].update(artists={}),
        "bad-credit-mbid-type": lambda m: m["album"]["artists"][0].update(
            musicbrainzId=123),
        "bad-credit-sortname-type": lambda m: m["album"]["artists"][0].update(
            sortName=5),
        "bad-track-credit-mbid-type": lambda m:
            m["media"][0]["tracks"][0].update(
                artists=[{"name": "X", "musicbrainzId": 7}]),
        "bad-identity-enum": lambda m: m.update(identity={"source": "bogus"}),
        "partial-album-loudness": lambda m: m.update(loudness={"albumLUFS": -12}),
        "fractional-track": lambda m: m["media"][0]["tracks"][0].update(track=1.5),
        "oversized-disc": lambda m: m["media"][0].update(disc=2147483648),
    }
    for name, mutate in mutations.items():
        manifest = base_manifest()
        mutate(manifest)
        add_case(cases, "invalid_manifest", name, manifest, one)
    for name, path in enumerate(["../x", "/tmp/x", "a\\b", "a//b", "a/./b", "a/../b", "", "audio/"]):
        manifest = base_manifest(path=path)
        add_case(cases, "invalid_manifest", "unsafe-path-%d" % name, manifest, one)

    waveform_mutations = {
        # Closed-enum violations
        "waveform-bad-version": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 2, "path": "analysis/waveform/01-01.wfm",
                      "sha256": "a" * 64, "intervalMs": 100,
                      "encoding": "peak-rms-u8", "floorDb": -60, "points": 10}),
        "waveform-bad-encoding": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 1, "path": "analysis/waveform/01-01.wfm",
                      "sha256": "a" * 64, "intervalMs": 100,
                      "encoding": "binary-f32le", "floorDb": -60, "points": 10}),
        "waveform-bad-interval": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 1, "path": "analysis/waveform/01-01.wfm",
                      "sha256": "a" * 64, "intervalMs": 50,
                      "encoding": "peak-rms-u8", "floorDb": -60, "points": 10}),
        "waveform-bad-floor": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 1, "path": "analysis/waveform/01-01.wfm",
                      "sha256": "a" * 64, "intervalMs": 100,
                      "encoding": "peak-rms-u8", "floorDb": -30, "points": 10}),
        "waveform-too-many-points": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 1, "path": "analysis/waveform/01-01.wfm",
                      "sha256": "a" * 64, "intervalMs": 100,
                      "encoding": "peak-rms-u8", "floorDb": -60,
                      "points": 900000}),
        "waveform-traversal": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 1, "path": "../evil.wfm",
                      "sha256": "a" * 64, "intervalMs": 100,
                      "encoding": "peak-rms-u8", "floorDb": -60, "points": 10}),
        "waveform-points-mismatch": lambda m: m["media"][0]["tracks"][0].update(
            waveform={"version": 1, "path": "analysis/waveform/01-01.wfm",
                      "sha256": "a" * 64, "intervalMs": 100,
                      "encoding": "peak-rms-u8", "floorDb": -60,
                      "points": 999}),
    }
    for name, mutate in waveform_mutations.items():
        manifest = base_manifest()
        mutate(manifest)
        add_case(cases, "invalid_manifest", name, manifest, one)

    for asset_key in ("artwork", "booklet", "lyrics", "extras", "analysis"):
        manifest = base_manifest()
        if asset_key == "artwork":
            manifest[asset_key] = [{"role": "front", "path": "artwork/a.bin"}]
        elif asset_key == "analysis":
            manifest[asset_key] = [{"type": "future", "path": "analysis/a.bin"}]
        else:
            manifest[asset_key] = [{"path": "%s/a.bin" % asset_key}]
        add_case(cases, "invalid_manifest", "missing-%s-checksum" % asset_key, manifest, one)

    raw = json.dumps(base_manifest()).encode("utf-8")
    for name, content in {
        "trailing-json": raw + b" trailing",
        "nul-suffix": raw + b"\0suffix",
        "duplicate-format": raw.replace(b'"format": "musicpack",', b'"format":"musicpack","format":"musicpack",'),
        "duplicate-version": raw.replace(b'"version": 1,', b'"version":1,"version":1,'),
        "duplicate-path": raw.replace(b'"path": "audio/01.bin",', b'"path":"audio/01.bin","path":"audio/01.bin",'),
        "duplicate-sha": raw.replace(b'"sha256":', b'"sha256":"' + b'a' * 64 + b'","sha256":', 1),
        "duplicate-credit-mbid": raw.replace(
            b'"artists": [{"name": "Tester"}]',
            b'"artists": [{"name": "Tester", "musicbrainzId": "one",'
            b' "musicbrainzId": "two"}]'),
    }.items():
        write_raw_manifest(os.path.join(out, name + ".mpack"), content, one)
        cases["invalid_manifest"].append(name)

    add_case(cases, "invalid_verify", "missing-asset", base_manifest(), {})
    add_case(cases, "invalid_verify", "checksum-mismatch", base_manifest(),
              {"audio/01.bin": b"changed"})
    for asset_key, path, data in [
        ("artwork", "artwork/a.bin", b"art"),
        ("booklet", "booklet/a.bin", b"book"),
        ("lyrics", "lyrics/a.bin", b"lyric"),
        ("extras", "extras/a.bin", b"extra"),
        ("analysis", "analysis/a.bin", b"analysis"),
    ]:
        manifest = base_manifest()
        entry = {"path": path, "sha256": HASH(data)}
        if asset_key == "artwork":
            entry["role"] = "front"
        if asset_key == "analysis":
            entry["type"] = "future"
        manifest[asset_key] = [entry]
        add_case(cases, "invalid_verify", "%s-checksum-mismatch" % asset_key,
                 manifest, {"audio/01.bin": b"one", path: b"changed"})
    if os.name != "nt":
        outside = os.path.join(out, "outside.bin")
        with open(outside, "wb") as f:
            f.write(b"one")
        root = os.path.join(out, "symlink-escape.mpack")
        write_package(root, base_manifest(), {})
        os.makedirs(os.path.join(root, "audio"))
        os.symlink(outside, os.path.join(root, "audio", "01.bin"))
        cases["invalid_verify"].append("symlink-escape")

    # Waveform checksum mismatch (file present, hash wrong).
    waveform_bad = {
        **one,
        "analysis/waveform/01-01.wfm": b"\x00" * 20,
    }
    waveform_bad_manifest = base_manifest()
    waveform_bad_manifest["media"][0]["tracks"][0]["waveform"] = {
        "version": 1, "path": "analysis/waveform/01-01.wfm",
        "sha256": "0" * 64, "intervalMs": 100,
        "encoding": "peak-rms-u8", "floorDb": -60, "points": 10,
    }
    add_case(cases, "invalid_verify", "waveform-checksum-mismatch",
             waveform_bad_manifest, waveform_bad)

    del cases["root"]
    with open(os.path.join(out, "cases.json"), "w", encoding="utf-8") as f:
        json.dump(cases, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
