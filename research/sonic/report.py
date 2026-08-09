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


def write_human_html(path: Path, seed_entries, methods: Sequence[str],
                     blind: bool, profile_ids: Dict[str, str]) -> Path:
    """Static-HTML human evaluation page.

    Each seed shows top-k neighbours per method. In blind mode the method
    labels are shown as Method A/B and revealed with a toggle. A tiny
    vanilla-JS widget lets the reviewer score each method 0-3 and pick
    A better / B better / tie; ratings persist to localStorage and can be
    downloaded as JSON (ratings are stored separately from embeddings).
    """
    import html as htmlmod

    def esc(s):
        return htmlmod.escape(str(s))

    method_display = {}
    for i, m in enumerate(methods):
        method_display[m] = ("Method %s" % "ABC"[i]) if blind else m

    cards = []
    for seed in seed_entries:
        method_cols = []
        for i, m in enumerate(methods):
            nn_html = "".join(
                "<li><span class='sim'>%.3f</span> %s</li>"
                % (nn["similarity"], esc(nn["label"]))
                for nn in seed["nearest"][m])
            profile_span = (
                "" if blind
                else "<span class='profile'>(%s)</span>" % esc(profile_ids.get(m, "")))
            method_cols.append(
                "<td><h3>%s %s</h3>"
                "<ol>%s</ol>"
                "<b>score 0–3</b> "
                "<select class='score' data-m='%d'>"
                "<option value=''>-</option><option value='0'>0</option>"
                "<option value='1'>1</option><option value='2'>2</option>"
                "<option value='3'>3</option></select><br>"
                "<b>vs other</b> "
                "<select class='pref' data-m='%d'>"
                "<option value=''>-</option><option value='win'>A better</option>"
                "<option value='tie'>tie</option>"
                "<option value='lose'>B better</option></select></td>"
                % (esc(method_display[m]), profile_span, nn_html, i, i))
        cards.append(
            "<div class='seed' data-seed='%d'>"
            "<h2>Seed %d: %s</h2>"
            "<div class='scorebox'><table><tr>%s</tr></table>"
            "<button class='save'>Save ratings</button>"
            "%s</div></div>"
            % (seed["index"], seed["index"] + 1, esc(seed["label"]),
               "".join(method_cols),
               ("<button class='reveal'>Reveal methods</button>" if blind else "")))

    page = _HUMAN_TEMPLATE % {
        "title": "MusicPack Sonic — human evaluation",
        "cards": "\n".join(cards),
        "methods": esc("|".join(methods)),
        "method_names": esc("|".join(method_display[m] for m in methods)),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(page)
    return path


_HUMAN_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>%(title)s</title>
<style>
  body {{ font-family: -apple-system, system-ui, sans-serif; max-width: 64em;
         margin: 2em auto; padding: 0 1em; color: #222; }}
  .seed {{ border: 1px solid #ddd; border-radius: 8px; padding: 1em 1.5em;
          margin: 1.5em 0; }}
  .seed h2 {{ margin-top: 0.3em; }}
  ol li {{ margin: 0.25em 0; }}
  .sim {{ color: #888; font-variant-numeric: tabular-nums; margin-right: .6em; }}
  .profile {{ font-size: 0.7em; color: #999; font-weight: normal; }}
  .scorebox {{ margin-top: 1em; background: #f7f7f7; border-radius: 6px;
              padding: 0.8em; }}
  table {{ border-collapse: collapse; }}
  td {{ padding: 0.4em 1em 0.4em 0; vertical-align: top; }}
  .saved {{ color: #0a7; margin-left: .8em; font-size: .85em; }}
  button {{ margin-right: .6em; }}
</style>
</head>
<body>
<h1>%(title)s</h1>
<p>Score each method 0–3 for how well its top-10 matches the seed.
<strong>0</strong>=unrelated, <strong>1</strong>=weak relationship,
<strong>2</strong>=plausible, <strong>3</strong>=strongly similar.
Optionally pick a preference for each seed. Ratings are stored in your
browser (localStorage) and can be downloaded as JSON — they are never stored
with the embeddings.</p>
%(cards)s
<hr>
<button id="dl">Download ratings JSON</button>
<script>
const METHODS = '%(methods)s'.split('|');
const KEYS = 'musicpack-sonic-ratings';
function load() {{ try {{ return JSON.parse(localStorage.getItem(KEYS)) || {{}}; }}
                   catch (e) {{ return {{}}; }} }}
function persist(r) {{ localStorage.setItem(KEYS, JSON.stringify(r)); }}
function collect() {{
  const r = load();
  document.querySelectorAll('.seed').forEach((card) => {{
    const s = +card.dataset.seed;
    if (!r[s]) r[s] = {{ score: {{}}, pref: {{}} }};
    card.querySelectorAll('.score').forEach((el) => {{
      if (el.value !== '') r[s].score[el.dataset.m] = +el.value;
    }});
    card.querySelectorAll('.pref').forEach((el) => {{
      if (el.value !== '') r[s].pref[el.dataset.m] = el.value;
    }});
  }});
  return r;
}}
document.querySelectorAll('.save').forEach((b) => b.addEventListener('click', () => {{
  persist(collect());
  const ok = document.createElement('span'); ok.className='saved'; ok.textContent='saved';
  b.parentNode.appendChild(ok);
  setTimeout(() => ok.remove(), 1500);
}}));
document.getElementById('dl').addEventListener('click', () => {{
  const data = collect(); persist(data);
  const blob = new Blob([JSON.stringify(data, null, 1)], {{type: 'application/json'}});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'ratings.json';
  a.click();
}});
document.querySelectorAll('.reveal').forEach((b) => b.addEventListener('click', () => {{
  b.disabled = true; b.textContent = '%(method_names)s';
}}));
</script>
</body>
</html>
"""


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
