"""Pairwise human-evaluation HTML and ratings aggregation (model-free)."""

import json

import report as rep


def _seeds():
    return [{
        "index": 0, "label": "Artist — Track", "artist": "Artist",
        "album": "Album", "genres": ["Rock"],
        "nearest": {
            "m1": [{"rank": 1, "label": "SameAlbum — T",
                    "similarity": 0.9, "same_album": True, "same_artist": True},
                   {"rank": 2, "label": "Other — T",
                    "similarity": 0.7, "same_album": False, "same_artist": False}],
            "m2": [{"rank": 1, "label": "Other — T",
                    "similarity": 0.8, "same_album": False, "same_artist": True}],
        },
    }]


def test_pairwise_html_renders_blind(tmp_path):
    out = rep.write_human_pairwise_html(
        tmp_path / "p.html", _seeds(), ["m1", "m2"], True,
        {"m1": "musicpack-sonic-v1-aaaa", "m2": "musicpack-sonic-v1-bbbb"}, k=8)
    t = out.read_text()
    assert "Method A" in t and "Method B" in t
    assert "same album" in t and "same artist" in t
    assert "Which list is a better match?" in t
    assert "<h3>musicpack-sonic-v1" not in t  # blind headings are Method A/B


def test_pairwise_html_reveals_methods_when_not_blind(tmp_path):
    out = rep.write_human_pairwise_html(
        tmp_path / "p2.html", _seeds(), ["m1", "m2"], False,
        {"m1": "openl3", "m2": "discogs"}, k=8)
    t = out.read_text()
    assert "openl3" in t and "discogs" in t
    assert "Reveal methods" not in t


def test_aggregate_pairwise_ratings(tmp_path, monkeypatch):
    ratings = {"0": {"pick": "A", "score": {"0": 3, "1": 1}},
               "1": {"pick": "tie", "score": {"0": 2, "1": 2}}}
    src = tmp_path / "ratings.json"
    src.write_text(json.dumps(ratings))

    class Args:
        aggregate = str(src)
        report = str(tmp_path)

    from benchmark import _aggregate_ratings
    monkeypatch.setattr(rep, "stats", lambda v: {
        "n": len(v), "mean": sum(v) / len(v),
        "std": 0.0, "min": min(v), "median": sorted(v)[len(v) // 2], "max": max(v)})
    assert _aggregate_ratings(Args()) == 0
    md = (tmp_path / "human-results.md").read_text()
    assert "A better | 1 |" in md and "tie better | 1 |" in md
    assert "Method A | 2.500 | 2 |" in md


def _triple_seeds():
    return [{
        "index": 0, "label": "SeedArtist — SeedTrack",
        "mapping": {"A": "m1", "B": "m2", "C": "m3"},
        "nearest": {
            "m1": [{"rank": 1, "label": "N1 — T", "similarity": 0.99,
                    "same_album": True, "same_artist": True}],
            "m2": [{"rank": 1, "label": "N2 — T", "similarity": 0.80,
                    "same_album": False, "same_artist": False}],
            "m3": [{"rank": 1, "label": "N3 — T", "similarity": 0.70,
                    "same_album": False, "same_artist": False}],
        },
    }]


def test_triple_html_hides_metadata_and_similarity(tmp_path):
    out = rep.write_human_triple_html(
        tmp_path / "t.html", _triple_seeds(), ["m1", "m2", "m3"],
        {"m1": "openl3", "m2": "discogs-multi", "m3": "clap"}, k=8)
    t = out.read_text()
    # model names hidden until reveal (in .realname, CSS-hidden)
    assert "openl3" in t and "discogs-multi" in t and "clap" in t
    assert "<span class='realname'>openl3</span>" in t
    # similarity + badges are inside .meta, CSS-hidden by default
    assert "same album" in t and "same artist" in t
    assert "0.99" in t
    # controls present
    assert "Best recommendation set" in t
    assert "None / all poor" in t
    assert "data-slot='A'" in t and "data-slot='B'" in t and "data-slot='C'" in t
    assert "textarea" in t and "Reveal methods + metadata" in t


def test_aggregate_triple_ratings(tmp_path, monkeypatch):
    ratings = {"0": {"best": "A", "score": {"A": 3, "B": 1, "C": 2},
                     "notes": "A very convincing"},
               "1": {"best": "None", "score": {"A": 0, "B": 1, "C": 0}}}
    src = tmp_path / "ratings.json"
    src.write_text(json.dumps(ratings))

    class Args:
        aggregate = str(src)
        report = str(tmp_path)

    from benchmark import _aggregate_ratings
    monkeypatch.setattr(rep, "stats", lambda v: {
        "n": len(v), "mean": sum(v) / len(v),
        "std": 0.0, "min": min(v), "median": sorted(v)[len(v) // 2], "max": max(v)})
    assert _aggregate_ratings(Args()) == 0
    md = (tmp_path / "human-results.md").read_text()
    assert "| A | 1 |" in md and "| None | 1 |" in md
    assert "| A | 1.500 | 2 |" in md  # scores A: (3+0)/2
    assert "A very convincing" in md  # notes preserved
