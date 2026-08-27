# Phase 2 — Bit-exact encoder SIMD optimization (Milestone 2 report)

Status: **COMPLETE.** The encoder's analysis filterbank (`libmpcenc/analy_filter.c`)
now has a bit-exact SIMD implementation (ARM64 NEON / x86-64 SSE2) behind the
shared `common/mpc_simd.h` abstraction, selected once at init, with the scalar
path preserved as the reference. All optimized outputs are byte-identical to
the scalar reference on every platform. The FP policy and the canonical
reference corpus were frozen before any SIMD work.

## Byte-identity claim (explicit)

**Optimized encoder files are byte-identical to their platform-specific
scalar reference** (same build, same compiler/flags) on every tested
architecture. They are **not** byte-identical across architectures, because
the scalar encoder itself is not cross-platform byte-identical today (see
ground-truth findings). On ARM64 clang — the platform the canonical manifest
was frozen on — both scalar and SIMD additionally match the committed
manifest. This distinction is the contract; it is not hidden.

## Current scalar cross-platform bit-exactness (ground truth, Phase A-first)

Determined empirically before any SIMD work, using the extended corpus at
q5/q6/q7:

* ARM64 clang -O0 (frozen-manifest platform): matches the pristine reference
  manifest **282/282**.
* x86-64 gcc -O0: only **28/282** match the arm64-clang manifest.
* Conclusion: the scalar encoder is **not cross-platform byte-identical**.
  Causes: FP codegen differences (clang contracts FMA at -O0 on arm64, gcc
  does not) and libm in the init tables (`cos()` in `Klemm()`,
  `atan()/cos()` in the FAST_MATH tables). These are properties of the
  scalar baseline, not of the SIMD optimization.
* Contract therefore: per-platform scalar identity (SIMD == same-build
  scalar). `enc_compat` live-mode (modernized == pristine reference, same
  toolchain) passes on ubuntu gcc, macos arm64 clang, and Windows MSVC.

## FP semantics / compiler safeguards (Phase C)

* **Canonical FP contract:** floating-point multiply and add are evaluated
  separately in source order; compiler FMA contraction is **not** part of the
  encoder's defined behavior. Rationale: contraction is a compiler
  optimization not specified by the encoder algorithm, and allowing it makes
  encoded output depend on target/compiler behavior.
* **Controls:** `-ffp-contract=off` on all encoder TUs (libmpcenc,
  libmpcpsy, mpcenc) for GCC/Clang; `/fp:precise` on MSVC (VS2022
  `/fp:precise` defaults to `fp_contract(off)`; `/fp:strict` reserved as a
  fallback for older MSVC). The decoder intentionally keeps its default
  (it allows contraction; Milestone 1).
* **One-time manifest re-freeze (documented):** the previous pristine
  manifest had captured clang's compiler-dependent FMA contraction at -O0.
  Under the new policy **253/282 hashes changed** (85 distinct input files)
  because FMA was eliminated; the previous baseline is preserved in git
  (`17eaecd`). The canonical manifest was re-frozen exactly once from the
  pristine reference under the policy and is now immutable (must not be
  regenerated to accommodate SIMD or later optimizations).
* The SIMD kernels use no FMA, no horizontal reductions, no reassociation,
  and replicate signed-zero behavior (accumulators start with the first
  product, not `0 + product`).

## Reference corpus design (Phase A)

`tests/generate_encoder_corpus.py` — 94 deterministic WAVs:

* the full existing `generate_corpus` set (silence, quiet, full-scale,
  impulse, transients, tonal, white noise, high-dynamic, asymmetry,
  phase-inverted, clipping, chirp, odd counts, very short) at 32/44.1/48 kHz;
* pink noise, low-frequency (30 Hz), near-Nyquist high-frequency, and
  multi-tone material;
* long dense inputs (30 s/60 s) so benchmarks are not open/header-overhead
  dominated;
* MPC-frame-boundary edge cases (1151/1152/1153, 2303/2304/2305 samples);
* a true mono WAV.

Encoded at q5/q6/q7 → `tests/encoder_reference_manifest.txt` (282 SHA-256
entries) from the pristine reference (r475, git 05d97a5) built at -O0 under
the FP policy. `tests/run_enc_compat.sh` + CTest `enc_compat` verify the
built encoder byte-for-byte: manifest mode (local, arm64-clang only) or live
`REF_MPCENC` mode (CI, toolchain-agnostic per-platform proof). CI gates
`enc_compat` live-mode only, since the manifest only matches its freeze
platform/compiler/optimization.

## Profiling results (Phase B)

macOS `sample` of a scalar Release encode (5-minute dense input):

```
cftfsub / rdft / bitrv2  (FFT, libmpcpsy)    63 samples
PowSpec256  (power spectrum)                 16 samples
Psychoakustisches_Modell                     14 samples
pow (libm, in psy)                           14 samples
Analyse_Filter / Vectoring / Matrixing      (not in top-20)
```

**The analysis filter is NOT the encoder hotspot.** The FFT + psychoacoustic
model in `libmpcpsy` dominates; the analyser is roughly 8–10% of total encode
time. The plan's expected hotspot was therefore wrong for the encoder; the
mandated `analy_filter` SIMD was still implemented (bit-exact) but its
end-to-end gain is correspondingly small (below). This is documented rather
than expanding scope into the psy model.

## Optimization architecture (Phases D/E/F)

* `common/mpc_simd.h` — the f32x4 abstraction moved from `libmpcdec` (one
  logical kernel for NEON/SSE2/wasm SIMD128; `mpcmath.h` gained the missing
  include guard).
* `libmpcenc/analy_filter_simd.c`:
  * **Vectoring** (32 outputs, 16-tap dots): lane = output index. The scalar
    `EXPR` macro parenthesizes each 8-term group, so each output is
    `sum(c1-chain) + sum(c2-chain)` — **two separately folded 8-term sums
    joined by one add** — reproduced exactly with two per-lane accumulators.
    Transposed `CiC`/`Xo` tables derived from the post-`Klemm()` `Ci_opt`.
  * **Matrixing** (32-tap dots per band): lane = band index, `y` broadcast,
    transposed `MT` table.
  * Vectorized groups of 4; the structurally isolated outputs (0, 16) and the
    region tails are handled scalar with the same arithmetic.
* **Dispatch:** `mpc_enc_set_impl()` / `mpc_enc_select_impl()` (module-level
  pointers, selected once). `mpc_enc_simd_init()` builds the transposed
  tables **explicitly after `Klemm()`** in `mpc_encoder_init`. `mpcenc` gains
  a hidden `--impl scalar|simd` flag for end-to-end A/B.
* Scalar kernels stay compiled and are the reference; SIMD is gated by
  `MPC_ENABLE_SIMD` + aarch64/x86-64 detection. No SSE4.1/AVX2/AVX-512, no
  threads, no wasm encoder.

## Optimizations attempted but rejected (changed output)

1. **FMA contraction in the SIMD path** — rejected; the FP policy defines
   mul+add and CI live-mode validates against the pinned pristine reference.
2. **Single-accumulator Vectoring (one 16-term chain)** — initially produced
   ~1-ulp divergence in the subband output; root-caused to the `EXPR` macro's
   parenthesization (`sum(c1) + sum(c2)`, not a flat chain). Fixed with two
   accumulators; rejected formulation removed.
3. **Building the SIMD coefficient tables before `Klemm()`** — rejected;
   produced a header-level divergence (char 42) because the tables read the
   pre-reorder `Ci_opt`. Fixed by explicit post-`Klemm()` init in
   `mpc_encoder_init`; caught by the CLI `--impl` A/B check.

## Regression tests (Phase G)

* `enc_compat`: modernized encoder vs canonical manifest / live `REF_MPCENC`.
  **282/282 at -O0**; live-mode green on all CI platforms.
* `enc_ab` (`tests/enc_synth_ab.c`): white-box — forces scalar and SIMD
  through the real init path and asserts **bit-identical subband output over
  8 state-evolving frames** plus `Analyse_Init`; reports the first
  (frame, band, subframe) divergence if any. Passes on ubuntu/macos/windows.
* CLI A/B: `mpcenc --impl scalar` vs `--impl simd` encodes are byte-identical
  (e.g., 30 s q6 and 60 s q6, at -O0 and Release).
* Full suite: 18/18 CTest pass with encoder SIMD on (and the decoder SIMD
  unchanged).

## SHA-256 compatibility results

| platform / build | scalar == canonical manifest | SIMD == scalar |
|---|---|---|
| arm64 clang -O0 (Apple M5) | 282/282 | bit-identical (enc_ab, --impl A/B) |
| x86-64 gcc -O0 | manifest is arm64-clang-only; not compared | bit-identical |
| arm64 clang Release | live-mode vs pristine | bit-identical |
| x86-64 gcc Release | live-mode vs pristine | bit-identical |
| Windows MSVC Release | live-mode vs pristine | bit-identical |

## ARM64 benchmark results (Apple M5, clang 21, Release)

Analyser kernel (isolated `Analyse_Filter`): scalar 13.5 µs/frame vs SIMD
7.9 µs/frame → **1.70× analyser speedup**.

End-to-end encode (94-file corpus, best-of-3, realtime multiplier):

| quality | scalar | SIMD | speedup |
|---|---|---|---|
| q5 | 244.5x | 259.3x | 1.06x |
| q6 | 239.1x | 257.6x | **1.08x** |
| q7 | 237.8x | 253.9x | 1.07x |

## x86-64 benchmark results (AMD EPYC 9V74, GCC 13.3, Release, CI)

| quality | scalar | SIMD | speedup |
|---|---|---|---|
| q5 | 118.1x | 130.4x | 1.10x |
| q6 | 115.4x | 128.2x | **1.11x** |
| q7 | 112.8x | 126.1x | 1.12x |

q6 (the MusicPack use case) is explicitly covered: ~1.08x arm64 / ~1.11x
x86-64, bit-exact.

## Compiler / platform notes

* MSVC: encoder SIMD compiles via `_M_X64` (SSE2); `/fp:precise` gives the
  required non-contracted semantics on VS2022. `enc_synth_ab.c` needs
  `_USE_MATH_DEFINES` for `M_PI` on MSVC (fixed).
* The manifest only matches its freeze platform (arm64 clang, -O0); other
  platforms rely on live-mode per-platform proof. Cross-platform scalar
  non-identity is expected and documented (libm + codegen), not a regression.
* No change to the Musepack format, psychoacoustic model, quantizer, quality
  behavior, or bitstream; encoder output bytes are unchanged vs the scalar
  reference.

## Remaining bottlenecks

`libmpcpsy` — the FFT (`fft4g.c`), `PowSpec256`, and `pow`/FAST_MATH table
math — is the dominant encode cost; the analysis filter is ~8–10%. End-to-end
encode speedup is capped at ~1.1x regardless of analyser gains. Benchmark
commands and full per-file data: `bench/encode_bench.sh` (results in
`bench/results/enc-*.tsv`).

## Recommended next step

Per the milestone mandate, no subsequent optimization phase was started.
The highest-value follow-up is profiling and optimizing **libmpcpsy**
(FFT + psychoacoustic analysis) — a much larger end-to-end win than the
analyser, and the same bit-exact lane discipline and FP policy apply. On the
encoder side, SSE4.1/AVX2 and a bit-exact wasm encoder remain future options
if justified by benchmarks. Re-freezing the manifest for any of these is not
allowed without a documented compelling reason (the canonical manifest is
immutable).

## Reproducing

```sh
cmake -S . -B build -DMPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target mpcenc mpc_enc_synth_ab
python3 tests/generate_encoder_corpus.py /tmp/enc-corpus
bash tests/run_enc_compat.sh build/codec/mpcenc/mpcenc            # manifest mode
bench/encode_bench.sh /tmp/enc-corpus build/codec/mpcenc/mpcenc 5,6,7 3
build/codec/mpcenc/mpcenc --silent --overwrite --impl scalar --quality 6 in.wav a.mpc
build/codec/mpcenc/mpcenc --silent --overwrite --impl simd   --quality 6 in.wav b.mpc
cmp a.mpc b.mpc                                            # byte-identical
```
