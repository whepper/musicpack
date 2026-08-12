# CI hardening milestone

## Baseline inventory

Before this cleanup, correctness and benchmark responsibilities were split as
follows. This matrix is the contract preserved by the hardened workflows.

| Workflow | Job | Platform/compiler | Build/configuration | SIMD role | Required gates |
|---|---|---|---|---|---|
| CI | native | Linux GCC, Linux Clang, macOS ARM64 Apple Clang, Windows x64 MSVC | Release | Default native SIMD | `synth_ab`, `synth_state_ab`, `enc_ab`, `psy_ab`; unit/package/server; Unix fixture/integration/fuzz suites; live `compat` and `enc_compat` |
| CI | scalar-only | Linux GCC | Release, `MPC_ENABLE_SIMD=OFF` | Scalar fallback | Applicable scalar suites and `synth_simd_unavailable` |
| CI | Wasm | Linux Emscripten/Node | Release, test hooks only | Explicit Wasm SIMD | `wasm_smoke`, `wasm_synth_ab` |
| CI | web client | Linux host/Emscripten/Node | Default product builds | Deployment integration | Web typecheck, unit/browser tests, gapless Node harness |
| Benchmark | native x64 | Linux GCC | Release | Scalar/SSE2 measurements | Decoder, encoder, psycho FFT/profile measurement artifacts |
| Benchmark | Wasm | Linux Emscripten/Node | Release scalar/autovec/explicit | Three-way Wasm measurement | Validated complete decodes, raw/summary provenance artifacts |

`synth_ab`, `enc_ab`, `psy_ab`, the scalar-only rejection check, and Wasm
scalar/explicit A/B are fail-closed: each is invoked by exact test name with
`--no-tests=error`. Live compatibility builds the patched upstream `05d97a5`
reference with the same selected compiler as the modern build. Release is
explicit on single-config generators and selected with `--config Release` on
multi-config generators.

## Risks found

- Native configure flags, dependency setup, CTest selection, and reference
  compiler arguments were repeated in workflow YAML.
- A broad Unix CTest exclusion command could change coverage when tests were
  added or renamed.
- Build configuration was visible in CMake output but not summarized in a
  consistent, greppable form for every important job.
- Windows reference executable discovery used `find | head`, and comments
  still described obsolete O0 reference alignment.
- Benchmark path triggers omitted subdirectory CMake and public-header changes.

## Changes

- Portable Python helpers assert and emit effective `CI_CONFIG` values,
  execute required CTest names one at a time, and locate a unique reference
  executable.
- Native, scalar-only, Wasm, and benchmark jobs print the same effective
  compiler/build/SIMD configuration summary after configure.
- Required test groups are explicit rather than exclusion-based. Unix-only
  suites remain explicitly Unix-only.
- Reference builds receive the matrix compiler selection and require matching
  compiler path, ID, and CMake-generated version before compatibility tests.
- Benchmark triggers cover codec sources, headers, all CMake files, benchmark
  helpers, tests, Wasm sources, and workflow files while remaining push-only.

## Guarantees after cleanup

- Requested scalar and SIMD implementations are forced and independently
  checked; unavailable SIMD rejects rather than falling back.
- CI asserts compiler ID, selected Release configuration, SIMD option,
  compiled decoder backend, and applicable Wasm flags before running tests.
- Direct decoder state, full-file decoder PCM, encoder filterbank, and
  psychoacoustic differential tests run on every applicable native job.
- Live encoder compatibility pairs GCC with GCC, Clang with Clang, Apple Clang
  with Apple Clang, and MSVC with MSVC.
- Wasm smoke and forced scalar/explicit-SIMD A/B are separate exact gates.
- Correctness CI and performance measurement stay in separate workflows.

## Hosted validation

The configuration enforcement and complete benchmark provenance were validated
at commit `a69b08cabef3e748e4d82e897047649da8adb31c`.

| Workflow | Run | Result |
|---|---|---|
| CI | [31602163181](https://github.com/whepper/musicpack/actions/runs/31602163181) | PASS: Linux GCC/Clang, macOS ARM64 Apple Clang, Windows x64 MSVC, Linux SIMD-off, and Wasm/Node |
| Benchmark | [31602163199](https://github.com/whepper/musicpack/actions/runs/31602163199) | PASS: native x86-64 plus scalar/autovec/explicit-SIMD Wasm measurements and artifacts |

All native jobs logged the claimed platform/compiler/configuration and ran
`synth_ab`, `enc_ab`, `psy_ab`, and live `enc_compat`. The scalar-only job
reported `simd_option=OFF`, `decoder_simd_enabled=FALSE`, and ran the forced
SIMD rejection gate. The Wasm job reported Emscripten, `MPC_WASM_SIMD=ON`,
test hooks enabled, and ran both smoke and scalar/explicit-SIMD A/B gates.

The benchmark run logged effective configuration for native and all three Wasm
builds. Decoder, encoder, psycho FFT, and psycho profile artifacts record the
executed binary where applicable, full commit, effective CMake metadata, and
their input or corpus hashes. The Wasm artifact records JS/module/input hashes
for every execution and five counterbalanced runs. No performance threshold is
evaluated.

## Troubleshooting

| Failure | Check |
|---|---|
| Required CTest missing | Locate the preceding `CI_CONFIG` line, then inspect CMake SIMD options and the named test registration. |
| SIMD A/B unavailable | Confirm `decoder_simd_enabled=TRUE` and `decoder_simd_backend` (`sse2`, `neon`, or `wasm-simd128`) in `CI_CONFIG`; verify the target architecture. |
| Scalar-only rejection fails | Confirm `MPC_ENABLE_SIMD=OFF`; the selector must return failure without changing scalar dispatch. |
| Compatibility mismatch | Compare the modern/reference `CI_CONFIG` compiler fields, reference commit, patch digest, and executable hashes. Do not regenerate manifests. |
| Wasm A/B missing | Confirm `MPC_WASM_SIMD=ON`, `MPC_ENABLE_SIMD=ON`, and `MPC_WASM_TEST_HOOKS=ON` in the Wasm test build. |
| Benchmark result invalid | Check completion errors, raw row count, provenance hashes, and generated summary. Benchmark success means valid measurement execution, not a performance threshold. |

## Limitations

- Windows ARM64 has compile support but no hosted runtime gate.
- Windows uses a multi-config generator: the selected CI configuration is
  explicitly recorded and asserted as `Release`; `CMAKE_BUILD_TYPE` is not
  treated as proof on that platform.
- Emscripten is intentionally installed from its current SDK channel; artifacts
  retain its exact version and hashes.
- Benchmarks run on shared infrastructure. They retain repetitions and spread
  but do not gate on a fixed performance threshold.
- The informational `bench` CTest is intentionally absent from explicit
  correctness suites because it only prints usage instructions; real benchmark
  execution remains in the separate benchmark workflow.
