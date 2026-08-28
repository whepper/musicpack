# MusicPack — Project Roadmap

Last updated: 2026-08-28.

The Musepack codec foundation is no longer the primary active engineering area.
**Musepack encoder optimization is complete** (see below), and the project has
moved to **real-world MusicPack validation and dogfooding**.

Detailed optimization history is preserved in the milestone reports listed
under "Completed work"; this file is the current status and does not require
reading the full history to understand where the project stands.

## ✅ Completed — Musepack Encoder Optimization (2026-08-27)

The encoder SIMD optimization track is **officially complete**.

- **Phase 3 delivered the last justified encoder performance gains** — about
  5–10% end-to-end encoding speedup at q5/q6/q7, byte-identical to the
  same-toolchain reference, via lane-parallel FFT, psycho spectrum/windowing
  SIMD, and CVD L/R cepstrum FFT batching.
- **The final ARM64 and x86-64 psychoacoustic investigation is complete.** Fine-grained
  per-function profiling on both architectures (native NEON; SSE2 under Rosetta)
  confirmed that the remaining candidate functions do not justify SIMD
  implementation. On ARM64 the five investigated candidates are each ≤3.4% of
  encoder CPU; on x86-64 each ≤3.1%. `CVD2048` dominates (~13% ARM64 / ~15%
  x86-64) and its one real hotspot (the cepstrum FFT) was already vectorized
  through `Cepstrum2048_2`/`rdft4`.
- **The conclusion holds across ARM64 and x86-64 evidence**, so it is not an
  artifact of one platform.
- **Musepack bit-exactness remains a hard compatibility requirement.** No
  optimization that changes encoder output is acceptable; the retained
  optimizations are proven byte-identical.
- **Reopening the track** should only happen if new profiling evidence
  identifies a materially different / high-value opportunity, with a separate
  scope decision (consistent with `AGENTS.md`).

Reference: `psy-optimization-final.md`,
`psy-optimization-milestone-3.md`,
`measurements/psy-model-profile/{arm64-apple-m5,x86_64-rosetta}.md`.

## 🚀 Current phase — MusicPack Real-world Validation & Dogfooding

Purpose: exercise the **complete** MusicPack workflow with real music and find
problems through actual use. This is a validation phase, not a feature roadmap.
The goal is evidence for future development, not speculative capability building.

### Workflow under test

```
FLAC → MusicPack Author → metadata → encode → .mpack → MusicPack Server → Web client → actual listening
```

### Areas to exercise

- **Authoring** — importing real FLAC albums; MusicBrainz matching; release/edition
  selection; multi-disc albums; artwork; metadata edge cases; q6 encoding;
  package validation; Sonic Analysis.
- **`.mpack`** — package integrity; hashes; metadata; multiple editions; incomplete or
  malformed packages; repeated validation; moving/copying packages.
- **Server** — real collection scanning; repeated scans; adding/removing albums;
  maintenance; verification/quarantine; restarts; collection growth; operational
  problems.
- **Web client** — browsing a real collection; release-group/edition navigation;
  search; album playback; gapless playback; normalization; seeking; queue;
  Media Session; Musepack WASM playback; mobile/responsive behaviour;
  reconnect/network interruption.
- **Audio quality** — actual listening with q6; comparison with source where useful;
  track boundaries; normalization; unusual source material / sample rates.
- **Operational UX** — the key question: *can MusicPack be used as a real personal
  music library without developer intervention?*

### Principle

> Fix problems discovered through real use before adding speculative capabilities.

### Explicitly out of scope during dogfooding

Do **not** start implementing these now; they remain future ideas where already
documented, to be reconsidered only after dogfooding produces evidence:

- native iOS
- server-side recommendations
- scrobbling
- OpenSubsonic
- speculative `.mpack` redesigns
- unrelated UX features — with one recorded exception: the **UI v2
  redesign** (consumer client + Author adoption) was explicitly
  user-approved scope and shipped as phases P0–P7 on 2026-08-28 (see
  `ui-v2-design.md`). The freeze applies again to future UX work.

## Completed work (engineering history, preserved)

| Milestone | Status | Report |
|---|---|---|
| Decoder synthesis SIMD (Milestone 1) | ✅ | `decoder-simd-milestone-1.md`, `decoder-simd-milestone-1-integrity-review.md` |
| Encoder analysis-filterbank SIMD (Milestone 2) | ✅ | `encoder-simd-milestone-2.md` |
| Psychoacoustic SIMD (Phase 3) | ✅ | `psy-optimization-milestone-3.md` |
| Final psychoacoustic investigation (ARM64 + x86-64) | ✅ | `psy-optimization-final.md`, `measurements/psy-model-profile/` |
| CVD/CEP analysis | ✅ | `measurements/cvd-analysis-profile/arm64-apple-m5.md` |
| MusicPack UI v2 (consumer P0–P5 + Author P6, docs P7) | ✅ | `ui-v2-design.md` |
