#!/usr/bin/env python3
"""Agreement analysis: how closely do OpenL3 / CLAP recommendation lists
match Discogs-EffNet's (the perceptual quality reference)?

For every track in the shared pool, each method produces a cosine
nearest-neighbour list (same retrieval, seed excluded). We then measure,
against the Discogs list:

  * jaccard@k      — overlap of the unordered top-k sets
  * recall@k       — fraction of Discogs' top-k that appear in the method's
                     top-k (how much of the reference list the method finds)
  * mean_rank      — mean position (in the method's list) of the Discogs
                     top-k items that are found (lower = closer ordering)

Reported across the whole pool and restricted to a seed subset.

Usage:
  research/sonic/.venv/bin/python research/sonic/agreement.py \
      --library /path/to/music [--seeds "substring" ...] [--k 5 10]
"""

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np

from benchmark import MISSING, _pick_methods, _prepare, _profile_index, _tracks, canonical_profile, scan  # noqa: E402
from cache import Cache  # noqa: E402
from decode import audio_sha256  # noqa: E402
from metrics import _cosine_matrix, _neighbors  # noqa: E402
from sonic_profile import Profile  # noqa: E402


def load_vectors(tracks, prof, cache):
    vecs = []
    for tr in tracks:
        v = cache.load_track(audio_sha256(tr.path), prof)
        if v is not MISSING and v is not None:
            vecs.append((tr, v))
    return vecs


def neighbor_sets(matrix, k):
    """Per-row ordered neighbor id lists, excluding self."""
    sim = _cosine_matrix(matrix)
    out = []
    for i in range(len(matrix)):
        out.append([int(j) for j in _neighbors(sim, k)[i]])
    return out


def agreement(ref_lists, method_lists, k):
    js = []
    rc = []
    rk = []
    for r, m in zip(ref_lists, method_lists):
        rs, ms = set(r[:k]), set(m[:k])
        inter = rs & ms
        js.append(len(inter) / len(rs) if rs else 0.0)
        rc.append(len(inter) / len(rs) if rs else 0.0)
        pos = [m.index(x) + 1 for x in inter if x in m]
        rk.append(np.mean(pos) if pos else float("nan"))
    return js, rc, rk


def stats(v):
    a = np.asarray([x for x in v if np.isfinite(x)], dtype=np.float64)
    return (a.mean(), a.std(), len(a)) if a.size else (float("nan"), float("nan"), 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True)
    ap.add_argument("--cache", default=str(Path(__file__).parent / "cache"))
    ap.add_argument("--report", default=str(Path(__file__).parent / "reports"))
    ap.add_argument("--models", default=str(Path(__file__).parent / "models"))
    ap.add_argument("--k", type=int, nargs="+", default=[5, 10])
    ap.add_argument("--seeds", nargs="*", default=[])
    ap.add_argument("--max-tracks-per-album", type=int, default=3)
    ap.add_argument("--max-albums-per-artist", type=int, default=2)
    ap.add_argument("--limit", type=int, default=100)
    args = ap.parse_args()

    lib = scan(args.library)
    tracks = _prepare(args, lib)
    cache = Cache(args.cache)

    all_profiles = [canonical_profile(p, Path(args.models))
                    for p in _profile_index(args)]
    methods = _pick_methods(all_profiles, ["openl3", "discogs", "clap"], want=3)
    if methods is None:
        return 2
    discogs = next(p for p in methods if p.model == "discogs-effnet")
    others = [p for p in methods if p.model != "discogs-effnet"]

    vec_by = {p.fingerprint(): load_vectors(tracks, p, cache) for p in methods}
    idx_by = {p.fingerprint(): {t.id: i for i, (t, _) in enumerate(vec_by[p.fingerprint()])}
              for p in methods}

    # restrict to tracks present in all three methods
    common = [tr for tr in tracks
              if all(tr.id in idx_by[p.fingerprint()] for p in methods)]
    order = [tr.id for tr in common]
    pos = {tid: i for i, tid in enumerate(order)}

    def matrix_for(prof):
        vb = vec_by[prof.fingerprint()]
        return np.stack([v for _, v in vb]), vb

    m_d, vb_d = matrix_for(discogs)
    sets_d = neighbor_sets(m_d, max(args.k))
    ref_ids = [[vb_d[j][0].id for j in row] for row in sets_d]

    def ref_slot_of(method, prof):
        m_m, vb_m = matrix_for(prof)
        sets_m = neighbor_sets(m_m, max(args.k))
        out = []
        for row in sets_m:
            ids = [vb_m[j][0].id for j in row]
            out.append([pos.get(i) for i in ids if i in pos])
        return out

    slots = {p.model: ref_slot_of(p, p) for p in others}
    slot_order = {p.model: idx_by[p.fingerprint()] for p in others}

    print("pool: %d tracks, %d albums, %d artists"
          % (len(common), len({(t.artist, t.album) for t in common}),
             len({t.artist for t in common})))

    seed_ids = set()
    if args.seeds:
        for s in args.seeds:
            for tr in common:
                if s.lower() in tr.label.lower():
                    seed_ids.add(tr.id)
                    break

    for k in args.k:
        print("\n== k=%d (reference = Discogs) ==" % k)
        print("%-8s %-9s %-9s %-10s  %s" % ("method", "jaccard", "recall", "meanrank",
                                            "vs-discogs"))
        for prof in others:
            ref_list = []
            meth_list = []
            for i, tr in enumerate(common):
                ref_list.append([order.index(r) for r in ref_ids[i][:k]])
                meth_list.append(slots[prof.model][i][:k])
            js, rc, rk = agreement(ref_list, meth_list, k)
            mj, sj, _ = stats(js)
            mr, sr, _ = stats(rc)
            mk, _, _ = stats(rk)
            print("%-8s %.3f±%.3f  %.3f±%.3f  %.1f  (n=%d)"
                  % (prof.model, mj, sj, mr, sr, mk, len(js)))

        if seed_ids:
            sel = [i for i, tr in enumerate(common) if tr.id in seed_ids]
            print("  -- restricted to %d review seeds --" % len(sel))
            for prof in others:
                js, rc, rk = agreement(
                    [ [order.index(r) for r in ref_ids[i][:k]] for i in sel ],
                    [ slots[prof.model][i][:k] for i in sel ], k)
                mj, sj, _ = stats(js)
                mr, sr, _ = stats(rc)
                mk, _, _ = stats(rk)
                print("  %-8s jaccard %.3f±%.3f  recall %.3f±%.3f  meanrank %.1f"
                      % (prof.model, mj, sj, mr, sr, mk))
    return 0


if __name__ == "__main__":
    sys.exit(main())
