# Phase 3 psychoacoustic SIMD integrity review

Review date: 2026-08-12. Baseline: `673ddd7` (the commit immediately before
the CI-fix sequence). Reviewed state: `a9c1408`, followed by the narrowly
scoped integrity corrections described below.

## Verdict

**PASS.** The retained SIMD code is strongly protected against codec-output
regressions, and the CI fix did not weaken tests or alter the reference
manifests. The profiling decision and end-to-end psychoacoustic evidence are
now reproducible and retained. Phase 3 is complete.

## CI-fix diff integrity

The final `673ddd7..a9c1408` diff changed only:

* `libmpcpsy/fft_routines_simd.c`: removed vector-pointer aliasing, used the
  unaligned SIMD wrappers, and aligned the private interleaved FFT buffer;
* `tests/psy_ab.c`: corrected an input-buffer overrun, initialized FFT and
  FAST_MATH tables before kernel use, and removed temporary diagnostics.

No workflow, CMake flag, fixture, compatibility script, reference manifest,
encoder algorithm, quantizer, or bitstream code changed in that range.
`tests/encoder_reference_manifest.txt` was last changed by the documented
one-time freeze at `c62d918`; neither manifest changed during Phase 3 or its
CI fixes. No temporary signal handlers, table dumps, clamps, stage markers,
hardcoded paths, or broad warning suppressions remain.

## Correctness guarantees

* `enc_compat` compares the default optimized encoder against a pristine
  same-toolchain reference over 94 deterministic WAVs at q5/q6/q7: 282/282
  local live-mode matches after this review.
* `psy_ab` now fails if psycho SIMD is not compiled, compares every output of
  every single and batched spectrum kernel, and checks q5/q6/q7 model output
  over eight evolving frames: L/R/M/S SMR and both channels' full transient
  arrays. Each run starts from a snapshot of the production-initialized model.
* `enc_ab` and decoder `synth_ab` now fail instead of silently comparing
  scalar with scalar when their SIMD kernel is absent.
* Decoder `synth_ab` now fails on frame-count differences and reports exact
  versus tolerance-only matches correctly.
* `enc_compat` now fails on a failed reference encode, a missing expected
  hash, or an empty corpus rather than silently reducing coverage.
* Linux and macOS CI now configure `Release` explicitly. Windows CI now runs
  `synth_ab`, `enc_ab`, and `psy_ab` directly in addition to live encoder
  compatibility. The pristine Unix reference remains at matched `-O3`.
* Encoder FP controls remain target-scoped: `-ffp-contract=off` or
  `/fp:precise` applies to `libmpcenc`, `libmpcpsy`, and `mpcenc`, not the
  decoder or unrelated targets.
* Native SIMD uses only ARM64 NEON or x86-64 SSE2 baseline operations. No
  AVX2, SSE4.1, native tuning, threading, quality tuning, or wasm encoder work
  entered Phase 3. Wasm remains decoder-only and retains its scalar,
  auto-vectorized, and explicit SIMD128 benchmark configurations.

## Review issues and disposition

1. Closed: Phase A now has a retained repeat protocol, additive hotspot
   percentages, q5/q6/q7 results, and ARM64 plus x86-64 evidence.
2. Closed: `bench/encode_bench.sh` now keeps the analyser AUTO in both arms
   and switches only `--psy-impl scalar|simd`. It checks byte identity and
   retains q5/q6/q7 end-to-end results on both architectures.
3. The existing FAST_MATH `my_atan2` table-index hazard remains outside this
   optimization change. The corrected test initializes all tables and uses a
   deterministic finite-noise input, but there is no dedicated boundary test.
4. Psychoacoustic dispatch, FFT work buffers, and FFT tables remain
   file-scope mutable state. Single-process CLI operation is deterministic,
   but concurrent encoder sessions are not proven reentrant.
5. Native Windows ARM64 is not covered. CMake recognizes ARM64 processor
   names, while the SIMD abstraction does not currently recognize MSVC's
   `_M_ARM64` macro. Hosted Windows x86-64/MSVC is covered.

## Phase 3 scope decision

PowSpec/windowing and lane-parallel FFT SIMD are implemented and preserve the
scalar per-output arithmetic order under the pinned FP policy on tested
ARM64/Clang and GCC builds. The q5/q6/q7 optimized default remains byte-exact
against the pristine reference. `pow()` and other libm behavior were not
replaced, matching the audit-only scope. No out-of-scope ISA, threading, or
quality work was added.

Phase 3 performance evidence is now retained in
`docs/psy-optimization-milestone-3.md`. The optimization has a measured gain,
and the agreed total-encoder FFT gate has been applied: the sub-35% share does
not justify a broader FFT rewrite. Final status is **COMPLETE**.

## Verification performed

```sh
ctest --test-dir build-enc-rel -R '^(psy_ab|enc_ab|synth_ab)$' --output-on-failure
# 3/3 pass, Apple Clang Release

ctest --test-dir /tmp/psy-gcc14 -R '^(psy_ab|enc_ab|synth_ab)$' --output-on-failure
# 3/3 pass, GCC 14 Release

tests/run_enc_compat.sh build-enc-rel/codec/mpcenc/mpcenc /tmp/ref-build/mpcenc/mpcenc
# 282/282 q5/q6/q7 outputs byte-identical

ctest --test-dir build-enc-rel -E '^compat$' --output-on-failure
# 20/20 applicable local tests pass
```

## Recommended next action

Stop. Keep the bit-exact optimizations justified by the measured 5-10%
end-to-end gain. Do not begin a broader FFT rewrite: FFT did not satisfy the
35% total-encoder CPU admission threshold.
