## Environment

- Commit: `21341e90bbe57623359a647f1c93c005d28325b3`
- Runner: GitHub Actions Ubuntu x86-64
- CPU: AMD EPYC 9V74 80-Core Processor
- Compiler: GCC 13.3.0
- CMake: 3.31.6
- Build: Release, `-O3 -DNDEBUG`, `MPC_ENABLE_SIMD=ON`, native tuning off
- Binary SHA-256: `97945aeda5f6487192f53faa4ea655d1738fe2cf05f2cd6deca24b5e29d4e47e`
- Corpus SHA-256: `5fedbe08b4ab8505d1d4afdfdd4288d363b060a917b8f2bc1fb8e15b7e589400`
- Workflow: https://github.com/whepper/musicpack/actions/runs/31591449091

## Method

136 files, three decodes per observation, five independent paired
repetitions, alternating scalar/SIMD order. Per-file medians were computed
first; the headline is the median of 136 paired SIMD/scalar ratios.

## Result

- Median per-file scalar throughput: 1521x realtime
- Median per-file SSE2 throughput: 2174x realtime
- Median paired speedup: **1.43x**
- Paired speedup range: 1.18x-1.60x

Raw and full summary TSVs are retained in the `bench-native-x64` workflow
artifact.
