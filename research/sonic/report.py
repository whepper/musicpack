"""Aggregate report generation (Markdown) and raw data persistence.

Reports committed to the repo are aggregate-only (counts, means, medians);
raw per-track JSON with relative paths lives in reports/raw/ (gitignored).
"""

from __future__ import annotations

import gzip
import json
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence

import numpy as np

from storage import encode

RAW_DIR = "raw"


def raw_dir(report_root: Path) -> Path:
    d = Path(report_root) / RAW_DIR
    d.mkdir(parents=True, exist_ok=True)
    return d


def write_raw(report_root: Path, name: str, data: dict) -> Path:
    path = raw_dir(report_root) / (name + ".json")
    path.write_text(json.dumps(data, indent=1, default=_json_default))
    return path


def read_raw(report_root: Path, name: str) -> dict:
    path = raw_dir(report_root) / (name + ".json")
    return json.loads(path.read_text())


def _json_default(o):
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError(type(o))


def markdown_table(headers: Sequence[str], rows: Sequence[Sequence[object]]) -> str:
    def cell(v) -> str:
        if isinstance(v, float):
            return "%.4f" % v
        return str(v)

    head = "| " + " | ".join(headers) + " |"
    sep = "|" + "|".join(["---"] * len(headers)) + "|"
    body = ["| " + " | ".join(cell(c) for c in row) + " |" for row in rows]
    return "\n".join([head, sep] + body) + "\n"


def stats(vals: Sequence[float]) -> Dict[str, float]:
    a = np.asarray(list(vals), dtype=np.float64)
    if a.size == 0:
        return {"n": 0, "mean": float("nan"), "std": float("nan"),
                "min": float("nan"), "median": float("nan"), "max": float("nan")}
    return {
        "n": int(a.size),
        "mean": float(a.mean()),
        "std": float(a.std()),
        "min": float(a.min()),
        "median": float(np.median(a)),
        "max": float(a.max()),
    }


def float_repr_table(results: Dict[str, Dict]) -> str:
    """results[album_label][encoding] = {'bytes': int, 'gz_bytes': int}"""
    encodings = ("json", "base64-f32le", "binary-f32le")
    headers = ["album"] + [
        "json B", "json gz", "b64 B", "b64 gz", "bin B", "bin gz"
    ]
    rows = []
    for album, enc in results.items():
        rows.append([
            album,
            enc["json"]["bytes"], enc["json"]["gz"],
            enc["base64-f32le"]["bytes"], enc["base64-f32le"]["gz"],
            enc["binary-f32le"]["bytes"], enc["binary-f32le"]["gz"],
        ])
    return markdown_table(headers, rows)


def write_float_repr_report(report_root: Path, results: Dict[str, Dict],
                            profile_id: str, dimensions: int) -> Path:
    out = Path(report_root) / ("float-repr-%s.md" % profile_id[:8])
    md = [
        "# Float representation study — profile %s" % profile_id,
        "",
        "Embedding dimensions: %d. Sizes are raw bytes and gzip-compressed "
        "bytes per whole album document." % dimensions,
        "",
        float_repr_table(results),
    ]
    out.write_text("\n".join(md))
    return out


def write_evaluate_report(report_root: Path, profile_id: str, k: int,
                          per_profile: Dict[str, Dict[str, float]],
                          dataset: Dict[str, object]) -> Path:
    out = Path(report_root) / ("evaluate-%s.md" % profile_id[:8])
    rows = []
    for combo, metrics in sorted(per_profile.items()):
        rows.append([
            combo,
            metrics.get("same_album@%d" % k, float("nan")),
            metrics.get("same_artist@%d" % k, float("nan")),
            metrics.get("genre_purity@%d" % k, float("nan")),
            metrics.get("album_coherence@%d" % k, float("nan")),
        ])
    md = [
        "# Quantitative evaluation — profile %s" % profile_id,
        "",
        "Dataset: %d tracks, %d albums, %d artists (aggregate; see raw). "
        "Diagnostics, not ground truth — they never define 'similar'." % (
            dataset.get("tracks", 0), dataset.get("albums", 0),
            dataset.get("artists", 0)),
        "",
        "| pooling/hop/silence | same_album@%d | same_artist@%d | "
        "genre_purity@%d | album_coherence@%d |" % (k, k, k, k),
        "|---|---|---|---|---|",
    ]
    for row in rows:
        md.append("| " + " | ".join(
            "%.4f" % v if isinstance(v, float) else str(v) for v in row) + " |")
    md.append("")
    out.write_text("\n".join(md))
    return out


def write_cross_codec_report(report_root: Path, profile_id: str, rows,
                             per_track: List[Dict]) -> Path:
    out = Path(report_root) / ("cross-codec-%s.md" % profile_id[:8])
    md = [
        "# Cross-codec stability — profile %s" % profile_id,
        "",
        "For each source track: cosine(source-embedding, flac-embedding) and "
        "cosine(source-embedding, mpc-q6-embedding). Ideal is ~1.0; lossy "
        "codecs should not change what the music 'sounds like'.",
        "",
        markdown_table(
            ["metric", "n", "mean", "std", "min", "median", "max"], rows),
    ]
    out.write_text("\n".join(md))
    write_raw(report_root, "cross-codec-" + profile_id[:8],
              {"profile": profile_id, "per_track": per_track})
    return out


def write_efficiency_report(report_root: Path, per_track: List[Dict],
                            environment: dict) -> Path:
    out = Path(report_root) / "efficiency.md"
    secs = [t["seconds"] for t in per_track]
    audio = [t["audio_s"] for t in per_track]
    rtf = [t["realtime_factor"] for t in per_track]
    wall_per_min = [t["seconds"] / (t["audio_s"] / 60.0) for t in per_track]

    def row(metric, vals):
        s = stats(vals)
        return [metric] + [s[k] for k in ("n", "mean", "std", "min", "median", "max")]

    md = [
        "# Efficiency benchmark",
        "",
        "Environment: " + ", ".join(
            "%s=%s" % (k, v) for k, v in sorted(environment.items())) + ".",
        "",
        markdown_table(
            ["metric", "n", "mean", "std", "min", "median", "max"],
            [
                row("wall s / track", secs),
                row("wall s / min audio", wall_per_min),
                row("realtime factor", rtf),
            ],
        ),
        "",
        "Realtime factor: seconds of analysis per second of audio "
        "(0.15x = 1 min of music in 9 s). Wall s / min audio is the same "
        "quantity scaled to one minute of music.",
    ]
    out.write_text("\n".join(md))
    return out
