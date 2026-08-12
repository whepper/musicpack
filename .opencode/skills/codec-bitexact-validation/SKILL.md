---
name: codec-bitexact-validation
description: Use ONLY for Musepack codec SIMD, floating-point, psychoacoustic, encoder compatibility, sanitizer, profiling, or performance changes that require behavior-preserving validation.
---

# Musepack Bit-Exact Validation

Use this workflow when changing `libmpcdec`, `libmpcenc`, `libmpcpsy`, codec
code in `common/`, SIMD dispatch, FP flags, or the reference compatibility
harness. Do not use it for MusicPack package, server, web UI, or Sonic-only
changes.

## Invariants

- Preserve codec math, quality decisions, quantization, defaults, and SV8
  output unless the user explicitly authorizes a codec change.
- Keep `-ffp-contract=off` or `/fp:precise` scoped to encoder/psy targets.
- Never regenerate reference manifests to make a changed encoder pass.
- Compare optimized encoder output with a pristine same-toolchain reference
  at matched optimization. Cross-toolchain byte identity is not expected.
- SIMD A/B tests must fail closed when SIMD is unavailable; scalar-vs-scalar
  comparisons are invalid evidence.
- Treat the FFT 35% total-encoder CPU threshold as the gate for a broader FFT
  rewrite, not as a Phase 3 completion requirement.

## Validation Sequence

1. Configure a Release test build:

   ```sh
   cmake -S . -B build-validation -DMPC_BUILD_TESTS=ON \
     -DMPC_BUILD_MPCGAIN=OFF -DMPC_BUILD_MPCCHAP=OFF \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build build-validation -j
   ```

2. Run direct implementation A/B tests when relevant:

   ```sh
   ctest --test-dir build-validation -R '^(synth_ab|enc_ab|psy_ab)$' \
     --output-on-failure
   ```

3. Run the applicable full suite. On Unix this includes fixtures,
   integration, and fuzz; Windows coverage is provided by CI.

4. For encoder changes, build `05d97a5` in a temporary worktree, run
   `tests/patch_reference.py`, and execute both live compatibility corpora:

   ```sh
   tests/run_compat.sh build-validation/mpcenc/mpcenc /tmp/ref-build/mpcenc/mpcenc
   tests/run_enc_compat.sh build-validation/mpcenc/mpcenc /tmp/ref-build/mpcenc/mpcenc
   ```

5. Configure `-DMPC_ENABLE_SIMD=OFF` for dispatch or SIMD changes. Verify the
   build succeeds and forced SIMD selectors/A/B tests reject unavailable SIMD.

6. Use ASan+UBSan for pointer, bounds, shift, or initialization changes. The
   pristine reference hardcodes `CMAKE_C_FLAGS`; instrument its worktree
   CMakeLists directly or the result is a false negative.

7. For performance claims, keep correctness checks in the benchmark path,
   report medians from at least three runs, and retain architecture, compiler,
   flags, workload, qualities, and raw-result location. Highlight q6.

## Psychoacoustic Measurements

- End-to-end: `bench/encode_bench.sh`, with analyser AUTO in both arms.
- Isolated production mix: `bench/run_psy_fft_bench.sh`.
- CPU shares: configure `MPC_ENABLE_PSY_PROFILE=ON`, then run
  `bench/profile_psy.py` for q5/q6/q7.
- Normal builds must not contain psychoacoustic profiling counters.

## Reporting

State exactly which configurations and platforms passed. Distinguish local
manifest fallback from live same-toolchain compatibility. If a hosted gate is
unavailable, report the gap instead of weakening or redefining the guarantee.
