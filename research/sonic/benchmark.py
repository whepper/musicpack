#!/usr/bin/env python3
"""MusicPack Sonic benchmark harness (research phase).

Subcommands:

  analyze     compute track + album embeddings for a library under a grid of
              profiles (hop x pooling x silence), using the two-level cache
  evaluate    quantitative diagnostics (same-album/artist@N, genre purity,
              album coherence) per profile -> aggregate markdown
  cross-codec FLAC vs MPC-Q6 embedding stability for a sample of tracks
  efficiency  wall time / realtime factor / peak RSS
  float-repr  JSON vs base64-float32 vs binary sizes on N-track albums
  human       static-HTML human evaluation with optional blind mode (M5)

Never requires Essentia. Never commits copyrighted audio or weights.

Examples:
  python research/sonic/benchmark.py analyze --library ~/Music/Test \
      --analyzer openl3 --hop 1.0 --pooling mean-norm --silence rel-20
  python research/sonic/benchmark.py analyze --library ~/Music/Test --quick
  python research/sonic/benchmark.py evaluate --library ~/Music/Test
  python research/sonic/benchmark.py cross-codec --library ~/Music/Test --n 20
  python research/sonic/benchmark.py efficiency --library ~/Music/Test --limit 10
  python research/sonic/benchmark.py float-repr --album-sizes 10 20 100
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from album import ALBUM_EQUAL, ALBUM_DURATION, album_embedding  # noqa: E402
from cache import MISSING, Cache  # noqa: E402
from decode import audio_sha256, decode  # noqa: E402
import report as rep  # noqa: E402
from library import Album, Track, scan  # noqa: E402
from metrics import summary_at_k, album_coherence_at_k  # noqa: E402
from pooling import apply_silence_and_pool, cosine  # noqa: E402
from profile import (  # noqa: E402
    POOL_MEAN,
    POOL_MEAN_NORM,
    POOL_ROBUST_MEAN,
    PoolingParams,
    Profile,
    SilenceParams,
)
from storage import encode, size_bytes  # noqa: E402

DEFAULT_HOPS = [0.5, 1.0]
DEFAULT_POOLINGS = [POOL_MEAN_NORM, POOL_MEAN, POOL_ROBUST_MEAN]
DEFAULT_SILENCES = ["none", "rel-20"]
ROBUST_TRIM = 0.1
K_DEFAULT = 10

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODELS = ROOT / "research/sonic/models"
DEFAULT_CACHE = ROOT / "research/sonic/cache"
DEFAULT_REPORT = ROOT / "research/sonic/reports"


def _silence_param(spec: str) -> SilenceParams:
    if spec == "none":
        return SilenceParams(enabled=False)
    if spec == "rel-20":
        return SilenceParams(enabled=True, threshold_db=-20.0, relative_to_median=True)
    if spec == "abs-70":
        return SilenceParams(enabled=True, threshold_db=-70.0, relative_to_median=False)
    raise argparse.ArgumentTypeError("unknown silence spec: %s" % spec)


def _silence_label(s: SilenceParams) -> str:
    if not s.enabled:
        return "nosil"
    return "rel-%g" % s.threshold_db if s.relative_to_median else "abs-%g" % s.threshold_db


def canonical_profile(prof: Profile, models_dir: Path) -> Profile:
    """Profile with model-specific facts filled in (weight identity), so the
    fingerprint used for cache keys and comparison includes them."""
    from analyzers.openl3 import WEIGHTS_SHA256 as OL3_WEIGHTS
    from analyzers.essentia_discogs import DiscogsAnalyzer

    if prof.model == "openl3":
        d = dict(prof.canonical())
        d["model_weights_sha256"] = OL3_WEIGHTS
        return Profile.from_dict(d)
    if prof.model == "discogs-effnet":
        from analyzers.essentia_discogs import model_sha256
        d = dict(prof.canonical())
        d["model_weights_sha256"] = model_sha256(prof.model_variant)
        return Profile.from_dict(d)
    return prof


def build_analyzer(profile: Profile, models_dir: Path, batch_size: int = 32):
    from analyzers.openl3 import OpenL3Analyzer

    if profile.model == "openl3":
        return OpenL3Analyzer(profile, models_dir, batch_size=batch_size)
    if profile.model == "discogs-effnet":
        from analyzers.essentia_discogs import DiscogsAnalyzer
        return DiscogsAnalyzer(profile, models_dir)
    raise ValueError("unknown analyzer model: %s" % profile.model)


def profile_grid(analyzer: str, hops, poolings, silences, variants=None) -> list:
    if analyzer == "openl3":
        base = dict(model="openl3", model_variant="", model_content="music",
                    model_input_repr="mel256", model_embedding_size=512,
                    model_sample_rate=48000, frontend="kapre", center=True)
    elif analyzer == "discogs":
        from analyzers.essentia_discogs import PATCH_WINDOW_SECONDS
        base = dict(model="discogs-effnet", model_content="discogs",
                    model_input_repr="mel96", model_embedding_size=1280,
                    model_sample_rate=16000, frontend="essentia", center=False)
        variants = variants or ["multi", "release"]
        window = PATCH_WINDOW_SECONDS  # model-fixed patch window (~2.096 s)
    else:
        raise argparse.ArgumentTypeError("unknown analyzer: %s" % analyzer)

    out = []
    for hop in hops:
        for strat in poolings:
            for sil_spec in silences:
                sil = _silence_param(sil_spec)
                if analyzer == "discogs":
                    for variant in variants:
                        p = Profile(
                            **{**base, "model_variant": variant},
                            pooling=PoolingParams(
                                strategy=strat, hop_seconds=hop,
                                window_seconds=window,
                                robust_trim=ROBUST_TRIM if strat == POOL_ROBUST_MEAN else 0.0,
                                silence=sil))
                        out.append(p)
                else:
                    p = Profile(
                        **base,
                        pooling=PoolingParams(
                            strategy=strat, hop_seconds=hop, window_seconds=1.0,
                            robust_trim=ROBUST_TRIM if strat == POOL_ROBUST_MEAN else 0.0,
                            silence=sil))
                    out.append(p)
    return out


def combo_label(p: Profile) -> str:
    variant = ("-" + p.model_variant) if p.model_variant else ""
    return "%s%s hop%g %s %s" % (
        p.model, variant, p.pooling.hop_seconds, p.pooling.strategy,
        _silence_label(p.pooling.silence))


def _tracks(library: list) -> list:
    return [t for a in library for t in a.tracks]


def _subset(tracks: list, per_album=None, albums_per_artist=None) -> list:
    """Deterministic stratified subset: cap tracks per album and albums per
    artist, preserving scan order (artist/album/track sorted). Used to make a
    100-1000 track representative benchmark of a larger library."""
    if per_album is None and albums_per_artist is None:
        return tracks
    albums = []
    for tr in tracks:
        key = (tr.artist, tr.album)
        if not albums or albums[-1][0] != key:
            albums.append([key, []])
        albums[-1][1].append(tr)
    if albums_per_artist is not None:
        seen = {}
        picked = []
        for key, trs in albums:
            if seen.get(key[0], 0) >= albums_per_artist:
                continue
            seen[key[0]] = seen.get(key[0], 0) + 1
            picked.append((key, trs))
    else:
        picked = albums
    out = []
    for _key, trs in picked:
        out.extend(trs[:per_album] if per_album else trs)
    return out


def _groups(library: list):
    tracks = _tracks(library)
    album_ids = [t.id for t in tracks]
    artists = [t.artist for t in tracks]
    genre_sets = [frozenset(t.genres) for t in tracks]
    return album_ids, artists, genre_sets


def _prepare(args, library) -> list:
    tracks = _subset(_tracks(library), args.max_tracks_per_album,
                     args.max_albums_per_artist)
    if getattr(args, "limit", None):
        tracks = tracks[:args.limit]
    return tracks


def _profile_index(args) -> list:
    """Profiles to use: explicit grid, or those recorded by a prior analyze."""
    idx_path = rep.raw_dir(args.report) / "profiles.json"
    if idx_path.is_file():
        data = json.loads(idx_path.read_text())
        return [Profile.from_dict(p) for p in data]
    return profile_grid(args.analyzer, args.hop, args.pooling, args.silence)


def _save_profile_index(args, profiles):
    """Merge the just-computed profiles into profiles.json (by fingerprint),
    so multi-analyzer runs accumulate (openl3 + discogs) instead of
    overwriting each other."""
    path = rep.raw_dir(args.report) / "profiles.json"
    existing = []
    if path.is_file():
        existing = [Profile.from_dict(p) for p in json.loads(path.read_text())]
    merged = {p.fingerprint(): p for p in existing + list(profiles)}
    data = [p.canonical() for p in merged.values()]
    path.write_text(json.dumps(data, indent=1))


# --------------------------------------------------------------------------
# analyze
# --------------------------------------------------------------------------
def cmd_analyze(args) -> int:
    library = scan(args.library)
    tracks = _prepare(args, library)
    profiles = [canonical_profile(p, args.models)
                for p in profile_grid(args.analyzer, args.hop, args.pooling, args.silence)]
    _save_profile_index(args, profiles)

    cache = Cache(args.cache)
    analyzers = {}
    for prof in profiles:
        analyzers[prof.fingerprint()] = build_analyzer(prof, args.models,
                                                       batch_size=args.batch_size)

    stats = {"tracks": len(tracks), "decode_fail": 0, "no_embedding": 0,
             "embeddings": 0, "analyzed_profiles": len(profiles),
             "wall_s": 0.0}
    t0 = time.time()
    for n, tr in enumerate(tracks, 1):
        try:
            sha = audio_sha256(tr.path)
        except OSError as e:
            print("skip %s (%s)" % (tr.path, e))
            continue
        pcm = sr = None
        for prof in profiles:
            if cache.load_track(sha, prof) is not MISSING:
                continue
            if pcm is None:
                try:
                    pcm, sr, _ = decode(tr.path, args.mpcdec)
                except Exception as e:
                    print("decode fail %s (%s)" % (tr.path.name, e))
                    stats["decode_fail"] += 1
                    break
            a = analyzers[prof.fingerprint()]
            model_key = prof.model_key()
            windows = cache.load_windows(sha, model_key)
            if windows is None:
                windows = a.window_embeddings(pcm, sr)
                cache.store_windows(sha, model_key, windows)
            v = apply_silence_and_pool(
                windows, pcm, sr, prof.pooling.silence,
                prof.pooling.strategy, prof.pooling.robust_trim)
            cache.store_track(sha, prof, v)
            if v is None:
                stats["no_embedding"] += 1
            else:
                stats["embeddings"] += 1
        if n % 10 == 0 or n == len(tracks):
            print("[analyze] %d/%d tracks in %.0fs (emb=%d, noemb=%d, fail=%d)"
                  % (n, len(tracks), time.time() - t0, stats["embeddings"],
                     stats["no_embedding"], stats["decode_fail"]), flush=True)
    stats["wall_s"] = time.time() - t0

    # album embeddings (equal + duration) per profile
    album_data = {}
    for prof in profiles:
        album_data[prof.id] = {}
        for strategy in (ALBUM_EQUAL, ALBUM_DURATION):
            vectors = {}
            for album in library:
                vecs = []
                for tr in album.tracks:
                    sha = audio_sha256(tr.path)
                    v = cache.load_track(sha, prof)
                    if v is MISSING:
                        v = None
                    vecs.append((tr.id, v, tr.duration or 0.0))
                emb = album_embedding(vecs, strategy)
                if emb is not None:
                    vectors[album.id] = encode(emb, "base64-f32le")
            album_data[prof.id][strategy] = vectors
    rep.write_raw(args.report, "albums", album_data)

    print("analyzed %d tracks x %d profiles in %.1fs "
          "(embeddings=%d, no_embedding=%d, decode_fail=%d)"
          % (stats["tracks"], len(profiles), stats["wall_s"],
             stats["embeddings"], stats["no_embedding"], stats["decode_fail"]))
    return 0


# --------------------------------------------------------------------------
# evaluate
# --------------------------------------------------------------------------
def cmd_evaluate(args) -> int:
    library = scan(args.library)
    tracks = _prepare(args, library)
    album_ids, artists, genre_sets = _groups(library)
    profiles = _profile_index(args)
    cache = Cache(args.cache)
    _save_profile_index(args, profiles)

    per_profile = {}
    dataset = {"tracks": len(tracks),
               "albums": len({(t.artist, t.album) for t in tracks}),
               "artists": len({t.artist for t in tracks})}
    for prof in profiles:
        emb = []
        avail = []
        for tr in tracks:
            v = cache.load_track(audio_sha256(tr.path), prof)
            if v is not MISSING and v is not None:
                emb.append(v)
                avail.append(tr)
        if len(emb) < 2:
            continue
        import numpy as np
        matrix = np.stack(emb)
        albums = [t.album for t in avail]
        artists_v = [t.artist for t in avail]
        genres_v = [frozenset(t.genres) for t in avail]
        m = summary_at_k(matrix, albums, artists_v, genres_v, k=args.k)

        # album coherence: album embeddings via equal weighting
        album_vecs, album_arts = [], []
        for album in library:
            vecs = []
            for tr in album.tracks:
                v = cache.load_track(audio_sha256(tr.path), prof)
                if v is not MISSING and v is not None:
                    vecs.append((tr.id, v, tr.duration or 0.0))
            emb_a = album_embedding(vecs, ALBUM_EQUAL)
            if emb_a is not None:
                album_vecs.append(emb_a)
                album_arts.append(album.artist)
        if len(album_vecs) >= 2:
            m["album_coherence@%d" % args.k] = album_coherence_at_k(
                np.stack(album_vecs), album_arts, k=args.k)
        per_profile[combo_label(prof)] = m
        print("%s: %d/%d available -> %s" % (combo_label(prof), len(avail),
                                             len(tracks), {k: round(v, 3)
                                                           for k, v in m.items()
                                                           if isinstance(v, float)}))

    for prof in profiles:
        rep.write_evaluate_report(args.report, prof.id, args.k, per_profile, dataset)
    return 0


# --------------------------------------------------------------------------
# cross-codec
# --------------------------------------------------------------------------
def cmd_cross_codec(args) -> int:
    from decode import decode

    library = scan(args.library)
    tracks = _prepare(args, library)
    prof = canonical_profile(
        profile_grid(args.analyzer, args.hop, args.pooling, args.silence)[0],
        args.models)
    analyzer = build_analyzer(prof, args.models, batch_size=args.batch_size)
    cache = Cache(args.cache)

    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    if not args.mpcenc:
        args.mpcenc = ROOT / "build/mpcenc/mpcenc"
    if not args.mpcenc.is_file():
        print("mpcenc not found at %s (build the C tools first)" % args.mpcenc)
        return 2

    import shutil
    import subprocess
    import tempfile
    import numpy as np
    import soundfile as sf

    per_track = []
    for i, tr in enumerate(tracks):
        try:
            pcm, sr, dur = decode(tr.path, args.mpcdec)
        except Exception as e:
            print("skip %s (%s)" % (tr.path.name, e))
            continue
        with tempfile.TemporaryDirectory(prefix="cc-") as tmp:
            wav = os.path.join(tmp, "src.wav")
            flac = os.path.join(tmp, "src.flac")
            mpc = os.path.join(tmp, "src.mpc")
            stereo = np.stack([pcm, pcm], axis=1)
            sf.write(wav, stereo, sr)
            r = subprocess.run([ffmpeg, "-y", "-v", "error", "-i", wav, flac],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print("ffmpeg fail %s" % tr.path.name)
                continue
            r = subprocess.run(
                [str(args.mpcenc), "--silent", "--overwrite", "--quality",
                 str(args.quality), wav, mpc], capture_output=True, text=True)
            if r.returncode != 0:
                print("mpcenc fail %s (%s)" % (tr.path.name, r.stderr.strip()))
                continue
            pcm_f, sr_f, _ = decode(flac)
            pcm_m, sr_m, _ = decode(mpc)

        e0 = _embed(analyzer, prof, cache, pcm, sr)
        ef = _embed(analyzer, prof, cache, pcm_f, sr_f)
        em = _embed(analyzer, prof, cache, pcm_m, sr_m)
        if any(x is None for x in (e0, ef, em)):
            print("no embedding %s" % tr.path.name)
            continue
        per_track.append({
            "index": len(per_track),
            "duration_s": round(dur, 2),
            "cos_source_flac": round(cosine(e0, ef), 5),
            "cos_source_mpc": round(cosine(e0, em), 5),
            "cos_flac_mpc": round(cosine(ef, em), 5),
        })
        print("track %2d  flac=%.3f mpc=%.3f flac-mpc=%.3f"
              % (len(per_track), per_track[-1]["cos_source_flac"],
                 per_track[-1]["cos_source_mpc"], per_track[-1]["cos_flac_mpc"]))

    rows = []
    for metric in ("cos_source_flac", "cos_source_mpc", "cos_flac_mpc"):
        s = rep.stats([t[metric] for t in per_track])
        rows.append([metric, s["n"]] + [s[k] for k in ("mean", "std", "min", "median", "max")])
    rep.write_cross_codec_report(args.report, prof.id, rows, per_track)
    print("cross-codec report written (n=%d)" % len(per_track))
    return 0


def _embed(analyzer, prof, cache, pcm, sr):
    windows = analyzer.window_embeddings(pcm, sr)
    return apply_silence_and_pool(
        windows, pcm, sr, prof.pooling.silence,
        prof.pooling.strategy, prof.pooling.robust_trim)


# --------------------------------------------------------------------------
# efficiency
# --------------------------------------------------------------------------
def cmd_efficiency(args) -> int:
    import psutil

    library = scan(args.library)
    tracks = _prepare(args, library)
    prof = canonical_profile(
        profile_grid(args.analyzer, args.hop, args.pooling, args.silence)[0],
        args.models)
    analyzer = build_analyzer(prof, args.models, batch_size=args.batch_size)

    proc = psutil.Process()
    base_rss = proc.memory_info().rss
    peak = base_rss
    per_track = []
    for tr in tracks:
        t0 = time.time()
        try:
            pcm, sr, dur = decode(tr.path, args.mpcdec)
        except Exception as e:
            print("skip %s (%s)" % (tr.path.name, e))
            continue
        windows = analyzer.window_embeddings(pcm, sr)
        v = apply_silence_and_pool(
            windows, pcm, sr, prof.pooling.silence,
            prof.pooling.strategy, prof.pooling.robust_trim)
        wall = time.time() - t0
        rss = proc.memory_info().rss
        peak = max(peak, rss)
        per_track.append({
            "track": tr.path.name, "audio_s": round(dur, 2),
            "seconds": round(wall, 3),
            "realtime_factor": round(wall / dur, 4) if dur else None,
        })
        print("%6.2fs audio -> %6.2fs wall (%.3fx)  %s"
              % (dur, wall, wall / dur if dur else 0, tr.path.name))

    environment = {
        "host": _host(),
        "profile": prof.id,
        **analyzer.model_identity(),
    }
    rep.write_efficiency_report(args.report, per_track, environment)
    rep.write_raw(args.report, "efficiency",
                  {"per_track": per_track, "peak_rss_delta_bytes": peak - base_rss,
                   "embedding_bytes": size_bytes(
                       encode(pooled_repr(prof), "base64-f32le"), "base64-f32le")})
    print("efficiency report written; peak RSS delta %.1f MB"
          % ((peak - base_rss) / 1e6))
    return 0


def pooled_repr(prof: Profile):
    import numpy as np
    return np.random.RandomState(0).randn(prof.dimensions).astype(np.float32)


def _host() -> str:
    import platform
    return "%s/%s %s" % (platform.system(), platform.machine(), platform.release())


# --------------------------------------------------------------------------
# float-repr
# --------------------------------------------------------------------------
def cmd_float_repr(args) -> int:
    import numpy as np

    profiles = _profile_index(args)
    if not profiles:
        prof = Profile()
        dimensions = prof.dimensions
    else:
        dimensions = profiles[0].dimensions
    rng = np.random.RandomState(0)
    unit = rng.randn(dimensions).astype(np.float32)
    unit /= np.linalg.norm(unit)

    results = {}
    for size in args.album_sizes:
        tracks = [unit + rng.randn(dimensions).astype(np.float32) * 1e-3 for _ in range(size)]
        enc = {}
        for encoding in ("json", "base64-f32le", "binary-f32le"):
            doc = _album_doc(encoding, dimensions, tracks)
            raw = doc.encode("utf-8") if isinstance(doc, str) else doc
            enc[encoding] = {
                "bytes": len(raw),
                "gz": len(gzip_bytes(raw)),
            }
        results["%d-track" % size] = enc
    _save_profile_index(args, profiles or [Profile()])
    prof_id = profiles[0].id if profiles else Profile().id
    rep.write_float_repr_report(args.report, results, prof_id, dimensions)
    for size in args.album_sizes:
        r = results["%d-track" % size]
        print("%3d-track  json=%7d (%7d gz)  b64=%6d (%6d gz)  bin=%5d (%5d gz)"
              % (size, r["json"]["bytes"], r["json"]["gz"],
                 r["base64-f32le"]["bytes"], r["base64-f32le"]["gz"],
                 r["binary-f32le"]["bytes"], r["binary-f32le"]["gz"]))
    return 0


def _album_doc(encoding, dimensions, vectors):
    import numpy as np
    tracks = [{"disc": 1, "track": i + 1,
               "embedding": encode(v, encoding)} for i, v in enumerate(vectors)]
    doc = {
        "format": "musicpack-sonic",
        "version": 1,
        "profile": {"id": "test-profile", "dimensions": dimensions,
                    "distance": "cosine"},
        "tracks": tracks,
    }
    if encoding == "binary-f32le":
        # binary payloads are concatenated; length implied by dimensions
        payload = b"".join(t["embedding"] for t in tracks)
        doc = {"format": "musicpack-sonic", "version": 1,
               "profile": {"id": "test-profile", "dimensions": dimensions,
                           "distance": "cosine", "encoding": "binary-f32le"},
               "trackCount": len(tracks),
               "embeddingsBlob": payload}
        return payload  # return just the blob; sizes reported for the payload
    import json as _json
    return _json.dumps(doc, separators=(",", ":"))


def gzip_bytes(raw: bytes) -> bytes:
    import gzip
    return gzip.compress(raw, compresslevel=9)


# --------------------------------------------------------------------------
# human evaluation
# --------------------------------------------------------------------------
def cmd_human(args) -> int:
    if args.aggregate:
        return _aggregate_ratings(args)

    import numpy as np

    from metrics import nearest

    library = scan(args.library)
    tracks = _prepare(args, library)
    cache = Cache(args.cache)

    all_profiles = [canonical_profile(p, args.models) for p in _profile_index(args)]
    primary = [p for p in all_profiles
               if (p.pooling.strategy == POOL_MEAN_NORM
                   and p.pooling.hop_seconds == 1.0
                   and not p.pooling.silence.enabled)]
    methods = primary or all_profiles[:1]
    if not methods:
        print("no profiles available; run 'analyze' first")
        return 2

    emb_by = {}
    for prof in methods:
        vecs = []
        for tr in tracks:
            v = cache.load_track(audio_sha256(tr.path), prof)
            if v is not MISSING and v is not None:
                vecs.append((tr, v))
        if len(vecs) < 2:
            print("too few embeddings for %s; skip" % combo_label(prof))
            continue
        emb_by[prof.fingerprint()] = vecs
    if not emb_by:
        print("no embeddings in cache for the human-eval profile; run analyze")
        return 2

    seed_idxs = _seed_indexes(tracks, args.seeds)
    seed_entries = []
    for idx in seed_idxs:
        entry = {"index": idx, "label": tracks[idx].label, "nearest": {}}
        for prof in methods:
            vecs = emb_by[prof.fingerprint()]
            query_pos = next((i for i, (t, _) in enumerate(vecs)
                              if t.id == tracks[idx].id), None)
            if query_pos is None:
                continue  # seed has no embedding in this method
            matrix = np.stack([v for _, v in vecs])
            labels = [t.label for t, _ in vecs]
            entry["nearest"][prof.fingerprint()] = nearest(
                matrix, labels, query_pos, k=args.k)
        if entry["nearest"]:
            seed_entries.append(entry)

    profile_ids = {p.fingerprint(): p.id for p in methods}
    method_names = [p.fingerprint() for p in methods]
    out_dir = Path(args.report) / "human"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / ("human-eval-%s.html" % methods[0].id[:8])
    rep.write_human_html(out, seed_entries, method_names, args.blind, profile_ids)
    print("human evaluation page written to %s (%d seeds x %d methods)"
          % (out, len(seed_entries), len(methods)))
    return 0


def _seed_indexes(tracks, seeds):
    if not seeds:
        return list(range(min(5, len(tracks))))
    if seeds == ["all"]:
        return list(range(len(tracks)))
    found = []
    for s in seeds:
        matches = [i for i, t in enumerate(tracks) if s.lower() in t.label.lower()]
        if not matches:
            print("no track matches seed %r" % s)
        found.extend(matches)
    return sorted(set(found))


def _aggregate_ratings(args) -> int:
    """Aggregate a downloaded ratings JSON into the committed report."""
    import numpy as np
    from pathlib import Path as _P

    ratings = json.loads(_P(args.aggregate).read_text())
    scores = {}
    wins = {}
    for seed, data in ratings.items():
        for m, s in (data.get("score") or {}).items():
            scores.setdefault(int(m), []).append(s)
        pref = data.get("pref") or {}
        for m, p in pref.items():
            wins.setdefault(int(m), []).append(p)
    lines = ["# Human evaluation results", "", "Ratings source: %s" % args.aggregate, ""]
    lines.append("| method | mean score (0-3) | n | wins | ties | losses |")
    lines.append("|---|---|---|---|---|---|")
    for m in sorted(scores):
        s = rep.stats(scores[m])
        w = wins.get(m, [])
        lines.append("| Method %s | %.3f | %d | %d | %d | %d |"
                     % ("ABC"[m], s["mean"], s["n"],
                        w.count("win"), w.count("tie"), w.count("lose")))
    lines.append("")
    out = Path(args.report) / "human-results.md"
    out.write_text("\n".join(lines))
    print("aggregate written to %s" % out)
    return 0


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="benchmark", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--library", default=os.environ.get("MUSIC_LIBRARY"),
                        help="music directory (default $MUSIC_LIBRARY)")
    common.add_argument("--cache", default=str(DEFAULT_CACHE))
    common.add_argument("--models", default=str(DEFAULT_MODELS))
    common.add_argument("--report", default=str(DEFAULT_REPORT))
    common.add_argument("--mpcdec", default=None)
    common.add_argument("--batch-size", type=int, default=32)
    common.add_argument("--max-tracks-per-album", type=int, default=None,
                        help="deterministic stratified subset: cap tracks per album")
    common.add_argument("--max-albums-per-artist", type=int, default=None,
                        help="deterministic stratified subset: cap albums per artist")

    pa = sub.add_parser("analyze", parents=[common])
    pa.add_argument("--analyzer", default="openl3", choices=["openl3", "discogs"])
    pa.add_argument("--hop", type=float, action="append", choices=[0.5, 1.0],
                    default=None)
    pa.add_argument("--pooling", action="append",
                    choices=[POOL_MEAN_NORM, POOL_MEAN, POOL_ROBUST_MEAN], default=None)
    pa.add_argument("--silence", action="append",
                    choices=["none", "rel-20", "abs-70"], default=None)
    pa.add_argument("--limit", type=int, default=None)
    pa.add_argument("--quick", action="store_true")
    pa.set_defaults(func=cmd_analyze)

    pe = sub.add_parser("evaluate", parents=[common])
    pe.add_argument("--analyzer", default="openl3", choices=["openl3", "discogs"])
    pe.add_argument("--hop", type=float, action="append", choices=[0.5, 1.0], default=None)
    pe.add_argument("--pooling", action="append",
                    choices=[POOL_MEAN_NORM, POOL_MEAN, POOL_ROBUST_MEAN], default=None)
    pe.add_argument("--silence", action="append",
                    choices=["none", "rel-20", "abs-70"], default=None)
    pe.add_argument("--k", type=int, default=K_DEFAULT)
    pe.add_argument("--limit", type=int, default=None)
    pe.set_defaults(func=cmd_evaluate)

    pc = sub.add_parser("cross-codec", parents=[common])
    pc.add_argument("--analyzer", default="openl3", choices=["openl3"])
    pc.add_argument("--hop", type=float, action="append", choices=[0.5, 1.0], default=[1.0])
    pc.add_argument("--pooling", action="append",
                    choices=[POOL_MEAN_NORM, POOL_MEAN, POOL_ROBUST_MEAN], default=[POOL_MEAN_NORM])
    pc.add_argument("--silence", action="append",
                    choices=["none", "rel-20", "abs-70"], default=["none"])
    pc.add_argument("--quality", type=int, default=6)
    pc.add_argument("--mpcenc", default=None)
    pc.add_argument("--limit", type=int, default=20)
    pc.set_defaults(func=cmd_cross_codec)

    pf = sub.add_parser("efficiency", parents=[common])
    pf.add_argument("--analyzer", default="openl3", choices=["openl3"])
    pf.add_argument("--hop", type=float, action="append", choices=[0.5, 1.0], default=[1.0])
    pf.add_argument("--pooling", action="append",
                    choices=[POOL_MEAN_NORM, POOL_MEAN, POOL_ROBUST_MEAN], default=[POOL_MEAN_NORM])
    pf.add_argument("--silence", action="append",
                    choices=["none", "rel-20", "abs-70"], default=["none"])
    pf.add_argument("--limit", type=int, default=None)
    pf.set_defaults(func=cmd_efficiency)

    pr = sub.add_parser("float-repr", parents=[common])
    pr.add_argument("--album-sizes", type=int, nargs="+", default=[10, 20, 100])
    pr.add_argument("--analyzer", default="openl3", choices=["openl3", "discogs"])
    pr.set_defaults(func=cmd_float_repr)

    ph = sub.add_parser("human", parents=[common])
    ph.add_argument("--seeds", nargs="*", default=[])
    ph.add_argument("--k", type=int, default=10)
    ph.add_argument("--blind", action="store_true")
    ph.add_argument("--limit", type=int, default=None)
    ph.add_argument("--aggregate", default=None,
                    help="path to a downloaded ratings JSON to aggregate")
    ph.set_defaults(func=cmd_human)

    args = ap.parse_args(argv)
    needs_library = args.cmd not in ("float-repr",) and not (
        args.cmd == "human" and args.aggregate)
    if needs_library and not args.library:
        ap.error("--library is required (or set $MUSIC_LIBRARY)")
    if not args.library:
        args.library = str(ROOT / "research/sonic/fixtures")
    if hasattr(args, "hop") and args.hop is None:
        args.hop = DEFAULT_HOPS if not getattr(args, "quick", False) else [1.0]
    if hasattr(args, "pooling") and args.pooling is None:
        args.pooling = (DEFAULT_POOLINGS if not getattr(args, "quick", False)
                        else [POOL_MEAN_NORM])
    if hasattr(args, "silence") and args.silence is None:
        args.silence = (DEFAULT_SILENCES if not getattr(args, "quick", False)
                        else ["none"])
    Path(args.report).mkdir(parents=True, exist_ok=True)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
