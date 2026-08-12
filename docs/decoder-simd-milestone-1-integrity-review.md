# Decoder SIMD Milestone 1 integrity review

## Verdict

**PASS** at commit `21341e90bbe57623359a647f1c93c005d28325b3`.

The review found no decoder DSP defect. The initial hardening attempt failed
because its Wasm EOF check, test registration, CI compiler matching, negative
dispatch proof, and benchmark evidence were incomplete. Those validation
defects are resolved and the repaired gates passed on every claimed runtime.

## Critical findings resolved

- Wasm EOF uses canonical `MUSEPACK_ERR_EOF == -5`; both the benchmark and
  scalar/SIMD differential test require exact EOF and complete sample count.
- `wasm_synth_ab` registers from the decoder's exported CMake capability and
  is executed by exact name with `--no-tests=error`.
- Linux GCC and Clang modern/reference encoder builds use matched compilers;
  Apple Clang and MSVC remain matched by their platform toolchains.
- The scalar-only job no longer runs a foreign manifest. Its dedicated
  `synth_simd_unavailable` test directly calls the selector, requires failure,
  and verifies that scalar dispatch was not changed.
- `synth_state_ab` forces distinct scalar/SIMD function pointers and compares
  mono and stereo PCM plus complete `V_L`/`V_R` histories over eight evolving
  frames. Output canaries and untouched mono right-channel state are checked.
- Wasm forcing hooks are compiled/exported only with
  `MPC_WASM_TEST_HOOKS=ON`; production Wasm exports remain unchanged.
- Benchmark inputs are validated, complete decode counts are required, raw
  repetitions are retained, native AB/BA and Wasm three-arm order are
  counterbalanced, and median/min/max summaries are generated.
- Corpus identity uses stable relative names plus bytes through portable
  Python hashing. Actual `.js`, `.wasm`, input, binary, and corpus hashes are
  retained with build/runtime metadata.

## Correctness evidence

- Local Apple M5 Release: `synth_ab` and `synth_state_ab` passed.
- Local Apple M5 ASan+UBSan Debug: `synth_ab` and `synth_state_ab` passed.
- Local scalar-only Release: `synth_simd_unavailable` passed.
- Hosted native jobs directly ran `synth_ab`, `synth_state_ab`, `enc_ab`, and
  `psy_ab` by exact test name on Linux GCC, Linux Clang, macOS ARM64 Apple
  Clang, and Windows x64 MSVC.
- Hosted Wasm directly ran `wasm_smoke` and `wasm_synth_ab` by exact name.
- The six-fixture full-file float PCM differential retains the original
  `2 / 32768` tolerance and rejects non-finite output.

## CI evidence

Successful hosted CI:

- [CI run 31591449183](https://github.com/whepper/musicpack/actions/runs/31591449183)
  - Linux x86-64 GCC: direct decoder gates, ordinary Unix suites, matched GCC
    live compatibility.
  - Linux x86-64 Clang: direct decoder gates, ordinary Unix suites, matched
    Clang live compatibility.
  - macOS ARM64 Apple Clang: architecture assertion, direct decoder gates,
    ordinary Unix suites, matched Apple Clang live compatibility.
  - Windows x64 MSVC: direct decoder gates and matched MSVC live
    compatibility.
  - Linux scalar-only: applicable suites plus direct forced-SIMD rejection.
  - Wasm/Node: golden smoke plus forced scalar/explicit-SIMD differential.
- [Benchmark run 31591449091](https://github.com/whepper/musicpack/actions/runs/31591449091)
  completed both the fresh x86-64 and three-configuration Wasm matrices and
  uploaded raw and summary artifacts.

## Fresh benchmark evidence

All rows are Release, native tuning off, five independent repetitions. Native
files use three decode iterations per repetition and alternate scalar/SIMD
order. The primary native statistic is the median paired speedup across the
136 per-file repetition medians. Wasm uses the 1152-frame read block and
counterbalances scalar/autovec/explicit order across five runs.

| Target | Scalar | Comparison | Median paired speedup |
|---|---:|---:|---:|
| Apple M5 ARM64, Apple Clang 21 | 3137x median/file | NEON 5120x median/file | **1.62x** |
| AMD EPYC 9V74 x86-64, GCC 13.3 | 1521x median/file | SSE2 2174x median/file | **1.43x** |

For the 48-second Wasm fixture on Node 22.23.1 / V8 12.4, Emscripten 6.0.6,
AMD EPYC 7763:

| Wasm configuration | Median realtime-x | Min-max | vs scalar |
|---|---:|---:|---:|
| scalar | 768x | 761-799x | 1.00x |
| autovec SIMD128 | 825x | 808-830x | 1.07x |
| explicit SIMD128 | 1979x | 1937-2010x | **2.58x** |

Retained concise evidence:

- `docs/measurements/decoder-simd-m1/arm64-apple-m5.md`
- `docs/measurements/decoder-simd-m1/x86_64-github-epyc.md`
- `docs/measurements/decoder-simd-m1/wasm-github-epyc.md`

Raw and full per-file x86-64/Wasm TSVs are available in the benchmark
workflow artifacts linked above. ARM64 raw and summary TSVs are committed
under `docs/measurements/decoder-simd-m1/raw/`.

## Remaining limitations

- Windows ARM64 has compile-time `_M_ARM64` and `arm64_neon.h` support, but no
  hosted Windows ARM64 runtime was available. No runtime claim is made.
- The Wasm decoder module remains single-threaded with no pthreads, shared
  linear memory, or Asyncify. Memory-backed decoding needs no cross-origin
  isolation. The separate demand-streaming web application intentionally uses
  a `SharedArrayBuffer` mailbox and requires COOP/COEP.
- Hosted runners are shared infrastructure. Min/max spread is retained and
  medians are used; individual short-file outliers remain expected.

## Final milestone status

Decoder SIMD Milestone 1 is genuinely complete. The existing NEON, SSE2, and
Wasm SIMD128 implementations are exercised directly, unavailable forced SIMD
fails closed, state evolution is compared, and fresh performance evidence is
reproducible from retained metadata and raw artifacts. No new optimization
phase is warranted by this review.
