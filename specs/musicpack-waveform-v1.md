# MusicPack Waveform Envelope — v1

> **Status: NORMATIVE for the v1 freeze.** Version 1. The waveform envelope
> is an **optional** per-track asset carried by `.mpack` v1 packages. The
> binary payload format, the manifest representation, the quantization
> formula, and the integrity rules in this document are stable; an
> incompatible change requires a `version` bump.
>
> Waveform is **derived package data**: a precomputed visualization channel
> generated from source PCM during authoring. `libmusicpack` is the
> authoritative parser and validator. Servers and clients consume it; they
> do not regenerate package waveform data.

## 1. Scope and purpose

A `.mpack` v1 package may carry a precomputed waveform envelope for every
track. The envelope is a coarser, visualization-oriented summary of the
track's amplitude over time — enough to drive a user-facing seek/progress
control without decoding audio.

```text
MusicPack Author
      ↓
analyse original decoded source PCM
      ↓
one .wfm per track, referenced from the track in manifest.json
      ↓
musicpack-server streams the stored bytes
      ↓
web player renders <canvas> as the seek control
```

The envelope is **not** a measurement (it is not loudness, not RMS in the
BS.1770 sense, not a perceptual metric). It is a deterministic, cheap-to-
decode, human-scale summary suitable for "where am I in the song" and "click
to jump there". A waveform-backed seek control is the v1 default seek UX;
the existing linear progress bar is the fallback when waveform is absent.

### Intended features

* Full-track waveform on the Now Playing seek control.
* Click/touch/keyboard seeking into the underlying audio.
* Per-track envelope proportional to the authoritative playback duration.
* Coexists with gapless transitions, BS.1770 normalization, WASM and
  native audio playback.

## 2. Conceptual pipeline

```text
track source PCM (mono / stereo / multichannel, 1..8 channels, integer or float)
      ↓
libmusicpack waveform accumulator
      ↓
100 ms fixed buckets, computed on cumulative sample-time boundaries
      ↓
per-bucket peak (max |sample| across channels)
  + per-bucket RMS (sqrt(sum(s²)/N) across all channels and frames)
      ↓
logarithmic uint8 quantization at -60 dBFS floor
      ↓
2 bytes per bucket, fixed `peak-rms-u8` encoding
      ↓
analysis/waveform/<DD>-<TT>.wfm
```

The accumulator is **streaming and constant-memory**: it retains one bucket
at a time (`peak_abs`, `sum_sq`, `frame_count` keyed to the cumulative
sample clock) and flushes a finalized bucket the instant the cumulative
sample count crosses the next 100 ms boundary. No whole-track buffering.

## 3. Scope: per-track only in v1

v1 stores **one waveform envelope per track**. There is no album-waveform
asset type. A future album-waveform envelope can be derived client-side by
concatenating per-track envelopes; the per-track design allows that without
introducing a second storage format in this version.

## 4. Package location and manifest reference

The waveform binary sits next to other derived analysis assets:

```text
Album.mpack/
├── manifest.json
├── audio/                       (existing)
└── analysis/
    └── waveform/
        ├── 01-01.wfm            (disc 1 / track 1)
        ├── 01-02.wfm
        └── 02-01.wfm            (disc 2 / track 1)
```

Filenames are zero-padded `<DD>-<TT>.wfm` (2-digit disc, 2-digit track).
The path is **multi-disc safe**: `02-01.wfm` and `01-02.wfm` resolve to
disc 2 track 1 and disc 1 track 2 respectively. Each path must be unique
across the entire package's asset paths.

The manifest carries the per-track reference directly on the track object
(it is track-scoped, not album-scoped):

```json
"tracks": [{
  "track": 1, "title": "...",
  "audio": { "path": "audio/01.mpc", "sha256": "...", "codec": "musepack-sv8" },
  "loudness": { "trackLUFS": -7.19, "truePeakDbTP": -4.18 },
  "waveform": {
    "version": 1,
    "path": "analysis/waveform/01-01.wfm",
    "sha256": "<64 lowercase hex>",
    "intervalMs": 100,
    "encoding": "peak-rms-u8",
    "floorDb": -60,
    "points": 2843
  }
}]
```

`waveform` is optional at the `.mpack` v1 format level. A package without
`waveform` on a given track (or without any `waveform` blocks at all) is
completely valid. Author defaults to generating it for every track.

## 5. Binary payload: `peak-rms-u8`

The binary file is a flat sequence of buckets, two bytes each:

```text
peak_u8, rms_u8, peak_u8, rms_u8, ...
```

* `peak_u8` is the quantized `max(|sample|)` across all channels in the
  bucket.
* `rms_u8` is the quantized `sqrt(sum(s²)/N)` over all channels and frames
  in the bucket (single denominator; not per-channel RMS).
* No per-file header. The interpretation (bucket size, encoding name,
  floor) is carried by the manifest.

### Bucket boundaries

The accumulator uses **cumulative sample-time boundaries** to avoid drift
from integer-rounded per-bucket counts:

```text
bucket_index = floor((cumulative_frames * 1000) / (sample_rate * 100))
```

A bucket is flushed every time `bucket_index` advances. The final partial
bucket (where the source ends before the next boundary) is retained and
flushed at EOF. `points` in the manifest equals the number of buckets
written, including the final partial one.

For a 283.9 s source at 44.1 kHz this produces `floor(283900 / 100) + 1` =
2839 buckets; at 48 kHz exactly the same duration produces 2839 buckets.
Different sample rates for the same duration produce the same bucket count
(within one bucket for non-integer division remainders).

### Channel handling

* The accumulator considers all channels and all frames in a bucket.
* `peak` = `max(|sample|)` across all channels and frames.
* `rms` = `sqrt(sum(s²)/N)` where `N` is the total number of *channel*
  samples (frames × channels). Stereo double-tracked content and mono have
  comparable RMS values per bucket.
* Channels above stereo are accepted (the underlying `musicpack_audio_*`
  decoder supports 1..8 channels) and contribute to both statistics
  identically.

### Source-PCM normalization

Source samples are normalized to `[-1, 1]` float before statistics. For
integer PCM (`bits` in `musicpack_audio_format`) the audio layer produces
left-aligned samples; the normalization is the source-bit-depth contract
defined in `<musicpack/audio.h>`. For 32-bit IEEE float WAV (informational
only; never an encoder source), samples used as-is and clamped at the
quantization stage. A value at exactly ±1.0 quantizes to 255; a value
slightly above ±1.0 (some float WAV sources) clamps to 255.

### RMS accumulation safety

`sum_sq` is accumulated in `long double` (then converted to `double`) to
avoid catastrophic cancellation across a long bucket; per-frame precision
is not required because the result is quantized through a 1-byte logarithmic
mapping. `sum_sq` is bounded by `frames_in_bucket * channels` (each
contribution ≤ 1.0² in absolute value), so no overflow risk at
mathematically reasonable bucket counts.

## 6. Quantization formula

```text
floor_db = -60            (fixed for v1; closed)

silent bucket (peak == 0 AND sum_sq == 0):
    peak_u8 = 0
    rms_u8  = 0

0 < a <= 1.0:
    dB              = 20 * log10(a)
    normalized      = (dB - floor_db) / (0 - floor_db)
                     = (dB + 60) / 60
    raw             = round(normalized * 254) + 1
    quantized       = clamp(raw, 1, 255)

a > 1.0 (decoded float overflow):
    quantized = 255                          (clamped; defensive)
```

Special cases:
* `a == 1.0` → `dB == 0` → `normalized == 1` → `raw == 255` → **255**
* `a == 10^-3` (i.e. `floor_db`) → `dB == -60` → `normalized == 0` →
  `raw == 1` → **1**
* `a == 0` (silent) → **0** (without going through the log mapping; both
  `log(0)` and the resulting division are undefined)
* `a > 1.0` (clamped) → **255**

Peak and RMS use the same formula. The mapping is the only mapping v1
defines; closed enum (`encoding: "peak-rms-u8"`). `floorDb == -60` is
closed for v1; a different floor is a v2.

## 7. Manifest contract (normative)

The track-level `waveform` object is optional. When present every field
is **required** (no missing fields, no `null` values other than `path`
having no defaultable form):

| Field        | Type   | Constraint (v1)                                                    |
|--------------|--------|--------------------------------------------------------------------|
| `version`    | integer| exactly `1`                                                        |
| `path`       | string | package-relative; canonical path rules; unique across all assets   |
| `sha256`     | string | exactly 64 lowercase hex characters                                |
| `intervalMs` | integer| exactly `100`                                                      |
| `encoding`   | string | exactly `"peak-rms-u8"`                                            |
| `floorDb`    | integer| exactly `-60`                                                      |
| `points`     | integer| `0 ≤ points ≤ 864000` (≤ 24 h × 10/s); payload size = `points * 2` |

Validation rules enforced by `libmusicpack`:
* Closed enums on `version`, `intervalMs`, `encoding`, `floorDb`.
* `path` is a canonical package-relative path (same rules as audio,
  artwork, ...). Path containment + uniqueness against every other
  referenced asset path.
* `sha256` is exactly 64 lowercase hex characters.
* `points` matches the on-disk payload size divided by 2 (else error).
* `points ≤ 864000` per track (≈ 1.5 MiB maximum payload).
* File exists, is a regular file, hash matches declared `sha256`.
* Per-bucket values are within `0..255`.

A declared waveform that fails any of these rules is an **error**: the
package is malformed and `verify` reports it. A package without waveform
is **completely valid** — there is no implicit requirement that waveform
be present.

### Duration consistency

`points` and the authoritative track `duration` may differ by up to one
bucket from container/source timing. Validated as a warning, not an error:

```text
|points - round(duration_seconds * 10)| <= 2
```

Packages outside this tolerance render and seek correctly (the client maps
waveform points proportionally over the authoritative duration), so this is
never a hard error.

## 8. Integrity

Every waveform asset is integrity-protected exactly like every other
referenced asset: declared `sha256` is verified against the bytes on disk
at `musicpack-package-verify` time. Quarantine rules in
`docs/server-untrusted-package-hardening.md` apply unchanged: a waveform
checksum-failed package is invisible/unservable until it verifies.

`manifest.json` is not self-hashed.

## 9. Size limits (normative)

* **Per-bucket payload**: `points * 2` bytes.
* **Per-track payload**: ≤ 1,728,000 bytes (≈ 1.5 MiB; `points ≤ 864000`).
* **Per-album aggregate**: bounded by the existing
  `MUSICPACK_MANIFEST_MAX_TOTAL_BYTES` (64 GiB) and the per-file size bound
  (`MUSICPACK_MANIFEST_MAX_FILE_SIZE`, 8 GiB).

At 100 ms × peak+rms u8 the raw payload is ~1.2 KB/minute of audio
(10 buckets/s × 2 bytes × 60 s ≈ 1,200 bytes/min).

## 10. Security model (normative)

The waveform asset is untrusted package input. `libmusicpack` enforces:
* Path validation identical to every other referenced asset.
* Bounded payload size (`points * 2`, capped at 1.5 MiB per track).
* SHA-256 verification on the bytes on disk.
* `points` upper bound (864000) prevents allocation amplification.

A package-provided waveform can never cause the player to invoke any audio
decoder or any encoder. The waveform bytes are render-only data.

## 11. Author pipeline (normative)

### Default behavior

MusicPack Author generates waveform envelopes **automatically and by
default for every track** as a distinct stage between Encode and Build:

```text
Inspect
→ Metadata
→ Encode
→ Waveform    ← automatic, default-on
→ Build
→ Verify
```

Generation is cancellable and progress-reporting (per-track, like Sonic
Analysis), but the Author UI does not require the user to initiate it.

### Author opt-out

The Author UI exposes an explicit **"Generate waveforms"** toggle in the
waveform panel. Disabling the toggle sets `waveformAnalysis.status` to
`"disabled"` in the draft, which `musicpack build-draft` interprets as an
explicit user opt-out and builds the package without waveform data.

A missing `waveformAnalysis` block (neither `"ready"` nor `"disabled"`) is
**not** an implicit opt-out. `build-draft` treats it as an error. This
prevents the pipeline from silently shipping a package with a broken
declaration if generation was skipped by accident.

### Failure handling

When waveform generation is enabled (`status != "disabled"`) and any
track fails to produce a valid envelope (decode error, write error,
hash mismatch, validation rejection), `musicpack build-draft` aborts the
build with a clear track-specific error. The staging directory is removed.
The user sees the error in the Author status panel and can either retry
generation, fix the underlying source issue, or explicitly opt out.

### Codec safety

Waveform generation runs as a **separate native source decode pass** in
`musicpack waveform-draft`. It does **not** share state with the
`encode-draft` path and does **not** invoke `mpcenc` or any encoder.
Musepack-encoded output is provably unchanged whether or not waveform
generation runs (proven by re-encode equality in `tests/run_encode.sh`).

Source formats supported are whatever `musicpack_audio_open` accepts:
FLAC, WAV (integer PCM and 32-bit float; other variants rejected upstream),
and Musepack SV7/SV8 (the latter is informational; authoring normally
operates on FLAC/WAV sources). Waveform generation treats all three
identically.

## 12. Server (server-data model)

`musicpack-server` ingests waveform assets during scan/verify:

* New `track_waveforms` table (migration 5) keyed by `track_id` (one row
  per track). Stores `version, relative_path, sha256, file_size, interval_ms,
  encoding, floor_db, points` mirroring the manifest contract.
* Re-ingest replaces existing rows (PK = `track_id`).
* Quarantine rules: a checksum-failed waveform makes the owning package
  invisible and unservable, identical to checksum-failed audio.

### API (HTTP, `/api/v1`)

`GET /api/v1/tracks/{id}/waveform`

* Returns the raw `.wfm` bytes (`Content-Type: application/octet-stream`;
  MIME table entry `application/vnd.musicpack.waveform-v1+octet-stream` for
  clients that care about the explicit type).
* Authentication: same as `/tracks/{id}/audio` — bearer token or session
  cookie.
* ETag: strong, `"<sha256>"`.
* Cache-Control: `private, max-age=0, must-revalidate`.
* Security headers: `X-Content-Type-Options: nosniff`, `Content-Security-Policy: sandbox; default-src 'none'; img-src 'self'`,
  `Content-Disposition: attachment; filename="<safe-name>.wfm"`.
* 304 on `If-None-Match`. 416 / Range not supported (waveform assets are
  ≤ 1.5 MiB; clients always consume them whole).

Track JSON (`GET /tracks/{id}` and the embedded `media[].tracks[]` in
`/releases/{id}`) gains an optional `waveform` field:

* `null` when the package has no waveform for the track (old packages,
  or this track opted out).
* Otherwise: `{ "version": 1, "intervalMs": 100, "encoding": "peak-rms-u8",
  "floorDb": -60, "points": N, "url": "/api/v1/tracks/{id}/waveform" }`.

## 13. Client rendering (expected behavior)

The first-party web player uses waveform envelopes to power the Now Playing
seek control:

* When a track carries a waveform, the linear `<input type="range">` is
  replaced by a `<canvas>`-backed waveform seek control.
* The control shows the full track; buckets are downsampled to display
  width preserving visible peaks (the maximum bucket peak and the
  root-mean-square of bucket RMS within each pixel column).
* A playhead overlay marks elapsed vs remaining, updated smoothly as the
  controller reports position.
* Click, pointer-drag, and tap on the canvas seek; arrow/Home/End/PageUp/
  PageDown on a focused (visually-hidden) sibling `<input type="range">`
  provide keyboard accessibility. The focused canvas shows a visible focus
  ring (the existing `:focus-visible` outline color).
* Track transitions (gapless or otherwise) swap the waveform data
  automatically; the audio path is not touched.
* When waveform is absent, the existing `<input type="range">` control is
  the seek UI. Playback never depends on waveform availability.

## 14. Forward compatibility

* A future `version: 2` waveform (different quantization, different
  encoding) is identifiable by `version` and ignored by a v1 client.
* Unknown fields inside the manifest `waveform` object are ignored on
  read; preserved on write only when the field is known.
* `floorDb: -60` is closed for v1; different floors imply a v2. Clients
  receiving a `floorDb` they do not recognize fall back to the linear
  seek control.
* `encoding: "peak-rms-u8"` is closed for v1; alternative encodings
  imply a v2. Clients receiving an unknown encoding fall back.

## 15. What is intentionally not in v1

* **Album/continuous waveform**: deferred. A future v2 can derive a
  continuous album envelope from per-track envelopes at serve time or
  client render time.
* **Multi-resolution envelopes** (e.g. fine + coarse buckets): deferred.
  100 ms is the single v1 bucket size.
* **Stereo L/R separation** in the binary payload: the v1 envelope is
  channel-combined peak/RMS. Independent L/R envelopes are a v2 option.
* **Re-derivation at scan time**: servers and clients consume the stored
  envelope; they do not regenerate it. Regeneration happens only in
  MusicPack Author.

## 16. Resolved design questions

* **Per-track on `track` vs package-level `analysis[]`**: per-track on
  `track` — waveform is naturally track-scoped derived data, the existing
  `track.audio` and `track.loudness` are track-scoped siblings.
* **Default ON in Author** with explicit opt-out — the user's confirmed
  design choice. Missing `waveformAnalysis` is treated as an error in
  `build-draft` unless `status == "disabled"`.
* **Hard fail on generation error** when enabled — another confirmed
  choice. Silent drop is reserved for forward-compatible opt-out only.
* **Cumulative sample-time bucket boundaries** — chosen over per-bucket
  sample counting to avoid timing drift across 24-hour tracks.
* **Multi-disc-safe filenames** `<DD>-<TT>.wfm` — disc-1 track-12 and
  disc-12 track-1 never collide.
* **100 ms buckets** — the v1 resolution; matches commercial music
  players and the user's task description.
* **`peak-rms-u8` encoding** — chosen over JSON arrays or PNG renderings
  for size, simplicity, and bit-for-bit determinism.
* **Generic decode via `musicpack_audio_*`** — FLAC, WAV, and Musepack
  sources handled identically; no special-cased paths.

## 17. License

This specification is part of the MusicPack repository and inherits the
project license. It is not a vendored third-party specification.
