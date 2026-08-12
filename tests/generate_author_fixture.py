#!/usr/bin/env python3
"""Generate the MusicPack Author development fixture album.

Produces a small, fully tagged, two-disc FLAC album under
tests/reference/author-fixture/ that exercises everything the Author encode
pipeline touches: multiple tracks, disc/track ordering, Vorbis Comments,
MusicBrainz-style identifiers, embedded artwork and an external cover file,
album artist differing from the track artist, and unknown/custom tags that
must survive tag passthrough.

The audio is short generated sine tones - no copyrighted material. FFmpeg is
required to synthesize the FLAC files (the toolchain used for encoding in
development too); the generated files are committed so tests do not need
ffmpeg present.

Usage: python3 tests/generate_author_fixture.py [out-dir]
"""

import argparse
import os
import subprocess
import sys
import tempfile


def run(args, **kw):
    subprocess.run(args, check=True, capture_output=True, **kw)


def ffmpeg_available():
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def make_cover(path, color):
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
         f"color=c={color}:size=300x300", "-frames:v", "1", path])


def make_track(wav, flac, title, artist, album, albumartist, track, total,
               disc, totaldiscs, date, genre, composer, isrc, mbrec, mbtrk,
               mbgroup, mbrel, barcode, comment, extra, frequency, duration,
               cover=None):
    md = ["-metadata", f"title={title}", "-metadata", f"artist={artist}",
          "-metadata", f"album={album}", "-metadata", f"albumartist={albumartist}",
          "-metadata", f"date={date}", "-metadata", f"genre={genre}",
          "-metadata", f"track={track}/{total}",
          "-metadata", f"disc={disc}/{totaldiscs}",
          "-metadata", f"composer={composer}", "-metadata", f"isrc={isrc}",
          "-metadata", f"musicbrainz_recordingid={mbrec}",
          "-metadata", f"musicbrainz_trackid={mbtrk}",
          "-metadata", f"musicbrainz_albumid={mbrel}",
          "-metadata", f"musicbrainz_releasegroupid={mbgroup}",
          "-metadata", f"barcode={barcode}", "-metadata", f"comment={comment}"]
    if extra:
        md += ["-metadata", extra]
    run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
         f"sine=frequency={frequency}:duration={duration}:sample_rate=44100",
         "-c:a", "flac", *md, wav])
    if cover:
        run(["ffmpeg", "-v", "error", "-y", "-i", wav, "-i", cover,
             "-map", "0:a", "-map", "1:v", "-c:a", "flac", "-c:v", "mjpeg",
             "-disposition:v", "attached_pic", *md, flac])
    else:
        run(["ffmpeg", "-v", "error", "-y", "-i", wav, "-c:a", "flac",
             *md, flac])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", nargs="?", default=None)
    args = ap.parse_args()

    if not ffmpeg_available():
        sys.exit("error: ffmpeg is required to regenerate the author fixture")

    root = args.out_dir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "reference", "author-fixture")
    album_dir = os.path.join(root, "Neon Skyline")
    os.makedirs(os.path.join(album_dir, "disc-1"), exist_ok=True)
    os.makedirs(os.path.join(album_dir, "disc-2"), exist_ok=True)

    tmp = tempfile.mkdtemp(prefix="author-fixture-")
    cover = os.path.join(tmp, "cover.jpg")
    make_cover(cover, "0x1a1a3e")

    base = dict(
        album="Neon Skyline", albumartist="The Signal", date="2019-03-15",
        genre="Synthwave", mbgroup="3a2b2a54-8f8a-4e2e-9f1b-000000000000",
        mbrel="1a1b1c1d-2a2b-3a3b-4a4b-5a5b5a5b5a5b", barcode="0198765432107",
    )

    tracks = [
        # disc 1: artist differs from album artist; featuring credit
        dict(disc=1, track=1, total=3, totaldiscs=2, title="Midnight Relay",
             artist="The Signal", composer="N. Ovation",
             isrc="QZABC1900001", mbrec="aaaaaaaa-bbbb-cccc-dddd-000000000001",
             mbtrk="11111111-2222-3333-4444-000000000001",
             frequency=220, duration=4, comment="opening transmission", extra=None,
             cover=True),
        dict(disc=1, track=2, total=3, totaldiscs=2, title="Starlight Drive",
             artist="The Signal feat. Vega", composer="Vega",
             isrc="QZABC1900002", mbrec="aaaaaaaa-bbbb-cccc-dddd-000000000002",
             mbtrk="11111111-2222-3333-4444-000000000002",
             frequency=330, duration=4, comment=None,
             extra='metainfo_artist="https://example.org/vega"'),
        dict(disc=1, track=3, total=3, totaldiscs=2, title="Low Orbit",
             artist="The Signal", composer="N. Ovation",
             isrc="QZABC1900003", mbrec="aaaaaaaa-bbbb-cccc-dddd-000000000003",
             mbtrk="11111111-2222-3333-4444-000000000003",
             frequency=440, duration=4, comment=None, extra=None),
        # disc 2: non-lexicographic filenames (must order by tag, not name)
        dict(disc=2, track=1, total=2, totaldiscs=2, title="Signal Flare",
             artist="The Signal", composer="N. Ovation",
             isrc="QZABC1900011", mbrec="aaaaaaaa-bbbb-cccc-dddd-000000000011",
             mbtrk="11111111-2222-3333-4444-000000000011",
             frequency=550, duration=4, comment=None, extra=None),
        dict(disc=2, track=2, total=2, totaldiscs=2, title="Collision Course",
             artist="The Signal", composer="Vega",
             isrc="QZABC1900012", mbrec="aaaaaaaa-bbbb-cccc-dddd-000000000012",
             mbtrk="11111111-2222-3333-4444-000000000012",
             frequency=660, duration=4, comment=None, extra=None),
    ]

    for t in tracks:
        disc_dir = os.path.join(album_dir, f"disc-{t['disc']}")
        wav = os.path.join(tmp, f"{t['title']}.wav")
        cover_src = cover if t.get("cover") else None
        flac = os.path.join(disc_dir, f"0{t['track']} - {t['title']}.flac")
        make_track(wav, flac, t["title"], t["artist"], base["album"],
                   base["albumartist"], t["track"], t["total"], t["disc"],
                   t["totaldiscs"], base["date"], base["genre"], t["composer"],
                   t["isrc"], t["mbrec"], t["mbtrk"], base["mbgroup"],
                   base["mbrel"], base["barcode"], t["comment"], t["extra"],
                   t["frequency"], t["duration"], cover_src)

    # external cover file in the album root (disc-1 also has embedded art)
    run(["cp", cover, os.path.join(album_dir, "cover.jpg")])

    import shutil
    shutil.rmtree(tmp)
    print(f"generated author fixture album at {album_dir}")


if __name__ == "__main__":
    main()
