# Milestone 1 — Modern SIMD decoder optimization (report)

Scope: benchmark infrastructure, profiling, WebAssembly SIMD128, synthesis
filter optimization (NEON / SSE2 / wasm SIMD128), correctness validation and
measurable before/after results. Encoder work was explicitly out of scope.

## Summary

The decoder's synthesis filter (the ISO/IEC 11172-3 polyphase matrixing +
fast-MDCT V-buffer update) was the dominant decode cost (~75% of
`mpc_decoder_decode_frame`). A 128-bit SIMD implementation now ships for all
three target ISAs behind one shared abstraction, selected per decoder
session. The scalar path is byte-for-byte untouched and remains the
reference/fallback.

Measured realtime-multiplier improvements (higher = faster):

| Target                                | scalar | SIMD   | speedup |
|---------------------------------------|--------|--------|---------|
| ARM64 — Apple M5, clang 21, 136-file corpus | 3101x  | 5156x  | **1.66x** |
| x86-64 — AMD EPYC 7763, GCC 13, same corpus | 1515x  | 2271x  | **1.50x** |
| wasm (Node 24, 48 s fixture) scalar    | 773x   | —      | —       |
| wasm autovec (-msimd128, scalar C)     | 812x   | —      | +5% vs scalar |
| wasm explicit SIMD                     | —      | 2042x  | **2.64x vs scalar** |

Native SIMD-enabled CI directly runs `synth_ab` alongside the ordinary test
suites. A scalar-only configuration runs applicable scalar suites and proves
that forced SIMD is rejected. The differential gate uses the decoder's
fixture tolerance; historical ARM64/x86-64 measurements observed zero float
differences on the six exercised fixtures.

## Baseline findings

- The repo's CI configure passes no `CMAKE_BUILD_TYPE`; single-config
  generators on Linux/macOS therefore compiled **unoptimized objects**. All
  benchmark builds used explicit `-DCMAKE_BUILD_TYPE=Release`.
- The wasm build passed `-O3` only at link time; library objects had no `-O`
  flags. Fixing this (target `-O3` + `Release`) is a prerequisite baked into
  all three wasm configurations measured here.
- Fixture comparison is tolerance-based (±2 LSB); the decoder has no
  bit-exact requirement, so FP contraction (FMA) is allowed in the SIMD
  kernel. No target exceeded the tolerance.

## Profiling

macOS `sample` of the scalar decode (48 s fixture):

```
mpc_synthese_filter_float_internal   2572 samples   ~75% of decode_frame
mpc_decoder_read_bitstream_sv8        688 samples   ~20%
other (requant, demux)                143 samples   ~ 5%
```

The synthesis filter is the confirmed hotspot, as the plan predicted.
After SIMD, the filter drops to ~52% of frame cost and the bitstream becomes
the next-largest component. No other routine was worth expanding scope for.

## Changes made

- `libmpcdec/mpc_simd.h` — minimal f32x4 abstraction (load/store/set1/
  add/sub/mul/rev4/swap-pairs/blend-lo-lo/blend-x/extract-lane) over ARM64
  NEON, x86-64 **SSE2** (baseline ISA, no runtime dispatch), and wasm
  SIMD128. SSE2-only per milestone decision; no SSE4.1/AVX2 tier.
- `libmpcdec/synth_filter_simd.c` — two kernels:
  - matrixing FIR: **lane = output index**; four outputs in parallel, each
    lane accumulating its 16 taps in scalar order (no horizontal
    reduction). Coefficients come from a transposed `DiT[16][32]` table so
    each lane reads 4 contiguous V samples and 4 contiguous coefficients.
  - `mpc_compute_new_V`: the two 16-value transform branches (sum and
    scaled-difference) each run the same 4-stage butterfly on 4 f32x4
    groups; the scalar pV permutation is preserved verbatim.
- `libmpcdec/synth_filter.c` — scalar kernel untouched; `mpc_decoder_synthese_filter_float`
  is now a thin dispatcher through a per-decoder function pointer.
- `libmpcdec/decoder.h` / `mpc_decoder.c` — `mpc_decoder_t` gains a `synth`
  fn pointer + transposed `DiT` table (built once in `mpc_decoder_setup`);
  white-box `mpc_decoder_set_synth_impl()` for A/B forcing (extensible for
  future AVX2 dispatch).
- CMake: `MPC_ENABLE_SIMD` (default ON on aarch64 / x86-64 / wasm-SIMD),
  `MPC_WASM_SIMD` (gates `-msimd128` for the 3-config wasm comparison),
  `MPC_ENABLE_NATIVE_TUNING` (opt-in, off by default). Wasm objects get `-O3`;
  `demo/build.sh` and the wasm CI job use `Release`.
- `bench/` — `decode_bench.c` (native, white-box impl forcing), `run_bench.sh`
  (records commit/compiler/arch/CPU/build provenance and independent paired
  repetitions into `results/*.tsv`), `make_corpus.sh`
  (deterministic WAV corpus incl. 30 s/60 s long tracks, encoded Q5/Q7),
  `wasm_bench.mjs` (Node, real `mpc_wasm_*` API, block-size A/B), README.
- `tests/synth_diff.c` — scalar-vs-SIMD differential test, registered as
  CTest `synth_ab` on native builds; Wasm has a separate forced scalar vs
  explicit-SIMD Node differential gate.
- `.github/workflows/bench.yml` — repeatable x86-64 native + 3-config wasm
  benchmark job (artifacts: `bench/results/`).

## Correctness results

- The SIMD-enabled Release CI matrix runs `synth_ab` directly on Linux GCC,
  Linux Clang, macOS Clang, and Windows MSVC. Emscripten/Node runs both the
  golden-WAV smoke test and forced scalar/explicit-SIMD differential test.
- Scalar-only CI runs the applicable suites and separately requires forced
  SIMD to fail closed.
- `synth_ab` historically observed worst diff 0 over six fixtures on ARM64
  Clang and x86-64 GCC. The maintained contract remains tolerance-based.
- A standalone kernel check confirms `mpc_compute_new_V` matches the scalar
  transform on 64/64 outputs on both NEON and SSE2.

## Native ARM64 results (Apple M5, clang 21, Release)

136-file corpus (126 deterministic encodes + fixtures + 30/60 s long
tracks), 3 decode passes per file:

| metric | scalar | SIMD |
|--------|--------|------|
| mean realtime-x | 3101 | 5156 |
| median realtime-x | 3161 | 5124 |
| overall mean speedup | — | **1.66x** (median 1.62x) |

Per-material: 48 s sine 1.73x, low-band noise up to 3.3x, dense
high-bitrate long tracks 1.26–1.31x. Speedup tracks how much of decode time
the filter occupies (smaller for high-bitrate, bitstream-heavy material).

## Native x86-64 results (AMD EPYC 7763, GCC 13.3, Release, same corpus)

| metric | scalar | SIMD |
|--------|--------|------|
| mean realtime-x | 1515 | 2271 |
| median realtime-x | 1491 | 2269 |
| overall mean speedup | — | **1.50x** (median 1.52x) |

(The earlier 2 s-only corpus run on a Xeon 8573C gave 1.39x; long tracks
reduce timing quantization and are the better decode-loop measure. File open
and decoder setup are outside the timer.)
GCC/SSE2 gain is lower than NEON's on the noise material (1.3x vs 3.3x);
suggested follow-up: inspect GCC SSE2 codegen for the strided FIR loads.

## WebAssembly results (Node 24, ubuntu runner, Release)

48 s fixture (most stable):

| config | realtime-x | vs scalar |
|--------|-----------|-----------|
| scalar (Release, no `-msimd128`) | 773x | 1.00x |
| autovec (`-msimd128`, scalar C)  | 812x | 1.05x |
| explicit SIMD                    | 2042x | **2.64x** |

LLVM auto-vectorization is nearly useless here (+5%) — the FIR's strided V
taps and the hand-unrolled transform defeat it. The explicit SIMD kernel is
where the win is. Same pattern on the 2 s corpus (2.1–2.5x, e.g. stereo_sine
437x → 1091x). Block-size boundary check (measure-only): 4608 vs 1152 frames
per `_mpc_wasm_read` adds ~5–30%, largest for low-workload files. No API
change made.

## Regressions / portability concerns

- The SSE2 blend helpers shipped initially with a wrong lane select (only
  visible on x86-64; NEON passed). Caught by the CI fixtures/synth_ab gate
  and fixed with explicit `_mm_shuffle_ps` selects. Lesson recorded: the
  differential test must run on every ISA, not just the dev machine.
- MSVC: `decode_bench` uses `QueryPerformanceCounter`/`clock` (no
  `clock_gettime`). The SIMD kernel compiles under MSVC (SSE2 via `_M_X64`);
  verified by the Windows CI build.
- The Wasm decoder module is single-threaded and uses no Wasm pthreads,
  shared linear memory, or Asyncify; memory-backed decoding needs no
  cross-origin isolation. The demand-streaming web application separately
  uses a SharedArrayBuffer mailbox and therefore requires COOP/COEP.
- `bench/` adds no runtime cost: the bench binary is only built under
  `MPC_BUILD_TESTS`, and its CTest entry is informational (excluded from the
  default suites).

## Recommended next step

Per the milestone mandate, encoder optimization was not started. The natural
next phase is **Phase 2 (encoder)**, applying the same lane=output SIMD
discipline to `libmpcenc/analy_filter.c` with bit-exact SHA-256 regression
as the gate. On the decoder side, only follow-up candidates remain (none
blocking): an SSE4.1 evaluation of the FIR blend/permutes if the GCC x86-64
gain warrants it, AVX2 only after that, and optionally a bigger JS<->wasm
read block (4608) in the web client if boundary overhead is measured in
browser conditions.

## Reproducing

```sh
cmake -S . -B build -DMPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release   # SIMD on (default)
cmake --build build -j --target mpc_bench mpcenc
bench/make_corpus.sh /tmp/mpc-bench "$(pwd)/build/mpcenc/mpcenc"
bench/run_bench.sh /tmp/mpc-bench                                   # scalar + simd
# wasm matrix (3 configs) and methodology: bench/README.md
```

Raw result records and per-file/configuration median/min/max summaries land in
`bench/results/`; the CI run uploads them as workflow artifacts. Historical
numbers above predate the integrity hardening and are not evidence for the
hardened benchmark path; the integrity review records the fresh post-hardening
runs separately.
