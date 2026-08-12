## Environment

- Commit: `21341e90bbe57623359a647f1c93c005d28325b3`
- OS: macOS 26.5.1 (25F80), ARM64
- CPU: Apple M5
- Compiler: Apple Clang 21.0.0 (`clang-2100.1.1.101`)
- CMake: 4.4.2
- Build: Release, `-O3 -DNDEBUG`, `MPC_ENABLE_SIMD=ON`, native tuning off
- Binary SHA-256: `81d644714de504bb2f1ac2b806c7c7ec94e5d736431078b22e820fea48b5b7ba`
- Corpus SHA-256: `6859e2129a29888a21be1d6a56fc28a306a9cbd0924f0cca26ee66f95adaacee`

## Method

136 files, three decodes per observation, five independent paired
repetitions, alternating scalar/SIMD order. Per-file medians were computed
first; the headline is the median of 136 paired SIMD/scalar ratios.

## Result

- Median per-file scalar throughput: 3137x realtime
- Median per-file NEON throughput: 5120x realtime
- Median paired speedup: **1.62x**
- Paired speedup range: 1.19x-2.01x

Raw and full summary:

- `docs/measurements/decoder-simd-m1/raw/arm64-apple-m5.tsv`
- `docs/measurements/decoder-simd-m1/raw/arm64-apple-m5-summary.tsv`
