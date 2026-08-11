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
    out = Path(report_root) / ("float-repr-%s.md" % profile_id)
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
    out = Path(report_root) / ("evaluate-%s.md" % profile_id)
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
    out = Path(report_root) / ("cross-codec-%s.md" % profile_id)
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
    write_raw(report_root, "cross-codec-" + profile_id,
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


def write_human_pairwise_html(path: Path, seed_entries, methods,
                              blind: bool, profile_ids: Dict[str, str],
                              k: int = 8) -> Path:
    """Much-easier-to-review human evaluation page: pairwise A/B.

    Each seed shows two ranked lists (one per method) with same-album /
    same-artist badges relative to the seed, and asks a single question —
    "which list is a better match?" (A better / tie / B better / neither),
    plus an optional 0-3 score per method. Ratings persist to localStorage
    and download as JSON, separate from embeddings. In blind mode the
    methods are Method A/B (profile ids hidden).
    """
    import html as htmlmod

    def esc(s):
        return htmlmod.escape(str(s))

    method_display = {}
    for i, m in enumerate(methods):
        method_display[m] = ("Method %s" % "AB"[i]) if blind else profile_ids.get(m, m)

    cards = []
    for seed in seed_entries:
        cols = []
        for i, m in enumerate(methods):
            lis = []
            for nn in seed["nearest"][m]:
                badges = []
                if nn.get("same_album"):
                    badges.append("<span class='badge album'>same album</span>")
                elif nn.get("same_artist"):
                    badges.append("<span class='badge artist'>same artist</span>")
                lis.append(
                    "<li><span class='sim'>%.3f</span> %s %s</li>"
                    % (nn["similarity"], esc(nn["label"]), "".join(badges)))
            cols.append(
                "<td><h3>%s</h3><ol>%s</ol></td>"
                % (esc(method_display[m]), "".join(lis)))
        cards.append(
            "<div class='seed' data-seed='%d'>"
            "<h2>Seed %d: %s</h2>"
            "<p class='ctx'>album: %s · genres: %s</p>"
            "<table><tr>%s</tr></table>"
            "<div class='pick'>"
            "<b>Which list is a better match?</b><br>"
            "<label><input type='radio' name='pick%d' value='A'> A better</label> "
            "<label><input type='radio' name='pick%d' value='tie'> tie</label> "
            "<label><input type='radio' name='pick%d' value='B'> B better</label> "
            "<label><input type='radio' name='pick%d' value='neither'> neither</label> "
            "<br>Score A 0-3: <select class='score' data-m='0'><option value=''>-</option>"
            "<option value='0'>0</option><option value='1'>1</option>"
            "<option value='2'>2</option><option value='3'>3</option></select> "
            "Score B 0-3: <select class='score' data-m='1'><option value=''>-</option>"
            "<option value='0'>0</option><option value='1'>1</option>"
            "<option value='2'>2</option><option value='3'>3</option></select> "
            "<button class='save'>Save</button>"
            "%s</div>"
            "</div>"
            % (seed["index"], seed["index"] + 1, esc(seed["label"]),
               esc(seed.get("album", "")), esc(", ".join(seed.get("genres", []))),
               "".join(cols),
               seed["index"], seed["index"], seed["index"], seed["index"],
               ("<button class='reveal'>Reveal methods</button>" if blind else "")))

    page = _PAIRWISE_TEMPLATE % {
        "title": "MusicPack Sonic — human evaluation (pairwise)",
        "cards": "\n".join(cards),
        "method_names": esc("|".join(profile_ids.get(m, m) for m in methods)),
        "k": k,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(page)
    return path


_PAIRWISE_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>%(title)s</title>
<style>
  body {{ font-family: -apple-system, system-ui, sans-serif; max-width: 72em;
         margin: 2em auto; padding: 0 1em; color: #222; }}
  .seed {{ border: 1px solid #ddd; border-radius: 10px; padding: 1em 1.5em;
          margin: 1.5em 0; background: #fff; }}
  .seed h2 {{ margin-top: .2em; }}
  .ctx {{ color: #777; font-size: .9em; margin: .2em 0 .8em; }}
  table {{ width: 100%%; border-collapse: collapse; }}
  td {{ width: 50%%; vertical-align: top; padding-right: 1em; }}
  ol li {{ margin: .35em 0; }}
  .sim {{ color: #999; font-variant-numeric: tabular-nums; margin-right: .5em; }}
  .badge {{ font-size: .72em; border-radius: 4px; padding: 1px 5px;
           margin-left: .4em; vertical-align: 1px; }}
  .badge.album {{ background: #e3f2fd; color: #1565c0; }}
  .badge.artist {{ background: #f3e5f5; color: #6a1b9a; }}
  .pick {{ margin-top: .8em; background: #f7f7f7; border-radius: 8px;
          padding: .7em 1em; }}
  .pick b {{ margin-right: .6em; }}
  button {{ margin-left: .6em; }}
  .saved {{ color: #0a7; margin-left: .6em; font-size: .85em; }}
</style>
</head>
<body>
<h1>%(title)s</h1>
<p>For each seed, compare the two lists of top-%(k)d neighbours. Badges mark
neighbours from the seed's own album/artist (context, not necessarily good).
Pick which list is the better musical match; optionally score each 0-3
(<strong>0</strong> unrelated, <strong>1</strong> weak, <strong>2</strong>
plausible, <strong>3</strong> strongly similar). Ratings are stored in your
browser and downloadable as JSON.</p>
%(cards)s
<hr>
<button id="dl">Download ratings JSON</button>
<script>
const KEYS = 'musicpack-sonic-ratings-pairwise';
function load() {{ try {{ return JSON.parse(localStorage.getItem(KEYS)) || {{}}; }}
                   catch (e) {{ return {{}}; }} }}
function collect() {{
  const r = load();
  document.querySelectorAll('.seed').forEach((card) => {{
    const s = +card.dataset.seed;
    if (!r[s]) r[s] = {{ pick: '', score: {{}}, }} ;
    const sel = card.querySelector('input[name="pick' + s + '"]:checked');
    if (sel) r[s].pick = sel.value;
    card.querySelectorAll('.score').forEach((el) => {{
      if (el.value !== '') r[s].score[el.dataset.m] = +el.value;
    }});
  }});
  return r;
}}
document.querySelectorAll('.save').forEach((b) => b.addEventListener('click', () => {{
  localStorage.setItem(KEYS, JSON.stringify(collect()));
  const ok = document.createElement('span'); ok.className='saved'; ok.textContent='saved';
  b.parentNode.appendChild(ok);
  setTimeout(() => ok.remove(), 1200);
}}));
document.getElementById('dl').addEventListener('click', () => {{
  const data = collect(); localStorage.setItem(KEYS, JSON.stringify(data));
  const blob = new Blob([JSON.stringify(data, null, 1)], {{type: 'application/json'}});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = 'ratings-pairwise.json'; a.click();
}});
document.querySelectorAll('.reveal').forEach((b) => b.addEventListener('click', () => {{
  b.disabled = true; b.textContent = '%(method_names)s';
}}));
</script>
</body>
</html>
"""


def write_human_triple_html(path: Path, seed_entries, methods, profile_ids,
                            k: int = 8) -> Path:
    """Three-way blind listening evaluation (OpenL3 / Discogs / CLAP).

    Metadata-free by default: columns show Artist — Track only. No
    similarity numbers, no same-album/artist/genre badges, no context, no
    model identity. The A/B/C -> model mapping is randomized per seed (done
    by the caller). The reviewer picks the best recommendation set
    (A/B/C/None), optionally scores each method 0-3 and adds notes. Reveal
    shows model identity + similarity/metadata for post-choice inspection.
    """
    import html as htmlmod

    def esc(s):
        return htmlmod.escape(str(s))

    cards = []
    for seed in seed_entries:
        cols = []
        for slot in ("A", "B", "C"):
            method = seed["mapping"][slot]
            nn = seed["nearest"].get(method, [])
            lis = []
            for r in nn:
                meta = ("<span class='meta'><span class='sim'>%.3f</span>%s</span>"
                        % (r.get("similarity", 0.0),
                           "".join(
                               ("<span class='badge album'>same album</span>"
                                if r.get("same_album") else "")
                               + ("<span class='badge artist'>same artist</span>"
                                  if r.get("same_artist") else ""))))
                lis.append("<li>%s %s</li>" % (esc(r["label"]), meta))
            cols.append(
                "<div class='col' data-slot='%s'>"
                "<h3><span class='slotname'>Method %s</span>"
                "<span class='realname'>%s</span></h3>"
                "<ol>%s</ol></div>"
                % (slot, slot,
                   esc(profile_ids.get(method, "")),
                   "".join(lis)))
        cards.append(
            "<div class='seed' data-seed='%d'>"
            "<h2>Seed %d: %s</h2>"
            "<div class='cols'>%s</div>"
            "<div class='pick'>"
            "<b>Best recommendation set:</b><br>"
            "<label><input type='radio' name='best%d' value='A'> A</label> "
            "<label><input type='radio' name='best%d' value='B'> B</label> "
            "<label><input type='radio' name='best%d' value='C'> C</label> "
            "<label><input type='radio' name='best%d' value='None'> None / all poor</label>"
            "<br>"
            "<span class='sc'>A: <select class='score' data-slot='A'>"
            "<option value=''>-</option><option value='0'>0</option>"
            "<option value='1'>1</option><option value='2'>2</option>"
            "<option value='3'>3</option></select></span> "
            "<span class='sc'>B: <select class='score' data-slot='B'>"
            "<option value=''>-</option><option value='0'>0</option>"
            "<option value='1'>1</option><option value='2'>2</option>"
            "<option value='3'>3</option></select></span> "
            "<span class='sc'>C: <select class='score' data-slot='C'>"
            "<option value=''>-</option><option value='0'>0</option>"
            "<option value='1'>1</option><option value='2'>2</option>"
            "<option value='3'>3</option></select></span><br>"
            "<textarea class='notes' rows='2' cols='70' "
            "placeholder='optional notes (e.g. \"A too broad, B very "
            "convincing, C semantically rather than sonically similar\")'></textarea>"
            "<br><button class='save'>Save</button>"
            "<button class='reveal'>Reveal methods + metadata</button>"
            "</div></div>"
            % (seed["index"], seed["index"] + 1, esc(seed["label"]),
               "".join(cols),
               seed["index"], seed["index"], seed["index"], seed["index"]))

    page = _TRIPLE_TEMPLATE % {
        "title": "MusicPack Sonic — blind listening evaluation (three-way)",
        "cards": "\n".join(cards),
        "k": k,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(page)
    return path


_TRIPLE_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>%(title)s</title>
<style>
  body {{ font-family: -apple-system, system-ui, sans-serif; max-width: 76em;
         margin: 2em auto; padding: 0 1em; color: #222; }}
  .seed {{ border: 1px solid #ddd; border-radius: 10px; padding: 1em 1.5em;
          margin: 1.5em 0; }}
  .seed h2 {{ margin-top: .2em; }}
  .cols {{ display: flex; gap: 1.2em; }}
  .col {{ flex: 1; min-width: 0; }}
  .col h3 {{ margin: .2em 0 .6em; }}
  ol li {{ margin: .35em 0; }}
  .meta {{ display: none; }}
  .seed.revealed .meta {{ display: inline; }}
  .seed.revealed .slotname {{ display: none; }}
  .realname {{ display: none; }}
  .seed.revealed .realname {{ display: inline; }}
  .sim {{ color: #999; font-variant-numeric: tabular-nums; margin-right: .4em; }}
  .badge {{ font-size: .72em; border-radius: 4px; padding: 1px 5px; }}
  .badge.album {{ background: #e3f2fd; color: #1565c0; }}
  .badge.artist {{ background: #f3e5f5; color: #6a1b9a; }}
  .pick {{ margin-top: .9em; background: #f7f7f7; border-radius: 8px;
          padding: .7em 1em; }}
  .pick b {{ margin-right: .5em; }}
  .sc {{ margin-right: .8em; }}
  textarea {{ margin: .5em 0; width: 100%%; box-sizing: border-box; }}
  button {{ margin-right: .6em; }}
  .saved {{ color: #0a7; margin-left: .6em; font-size: .85em; }}
</style>
</head>
<body>
<h1>%(title)s</h1>
<p>For each seed, three methods (A/B/C) each recommend %(k)d neighbour tracks.
Compare the three lists and answer <b>which set actually sounds most
related to the seed</b>. Metadata (album/artist/similarity) and model
identity are hidden — click <i>Reveal</i> only after you have recorded your
choice. Scores: <strong>0</strong> unrelated, <strong>1</strong> weak,
<strong>2</strong> convincing, <strong>3</strong> excellent. Ratings are
stored in your browser and downloadable as JSON.</p>
%(cards)s
<hr>
<button id="dl">Download ratings JSON</button>
<script>
const KEYS = 'musicpack-sonic-ratings-triple';
function load() {{ try {{ return JSON.parse(localStorage.getItem(KEYS)) || {{}}; }}
                   catch (e) {{ return {{}}; }} }}
function collect() {{
  const r = load();
  document.querySelectorAll('.seed').forEach((card) => {{
    const s = +card.dataset.seed;
    if (!r[s]) r[s] = {{ best: '', score: {{}}, notes: '' }};
    const sel = card.querySelector('input[name="best' + s + '"]:checked');
    if (sel) r[s].best = sel.value;
    card.querySelectorAll('.score').forEach((el) => {{
      if (el.value !== '') r[s].score[el.dataset.slot] = +el.value;
    }});
    const nt = card.querySelector('.notes');
    if (nt && nt.value) r[s].notes = nt.value;
  }});
  return r;
}}
document.querySelectorAll('.save').forEach((b) => b.addEventListener('click', () => {{
  localStorage.setItem(KEYS, JSON.stringify(collect()));
  const ok = document.createElement('span'); ok.className='saved'; ok.textContent='saved';
  b.parentNode.appendChild(ok); setTimeout(() => ok.remove(), 1200);
}}));
document.getElementById('dl').addEventListener('click', () => {{
  const data = collect(); localStorage.setItem(KEYS, JSON.stringify(data));
  const blob = new Blob([JSON.stringify(data, null, 1)], {{type: 'application/json'}});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = 'ratings-triple.json'; a.click();
}});
document.querySelectorAll('.reveal').forEach((b) => b.addEventListener('click', () => {{
  b.closest('.seed').classList.add('revealed');
  b.disabled = true; b.textContent = 'revealed';
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
