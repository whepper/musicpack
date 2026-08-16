# AGENTS.md

Guidance for AI agents working on this Musepack (MPC) codebase. Read this
before making changes.

## What this repository is

Musepack is an open-source lossy audio codec. This repo contains the decoder
(`libmpcdec`), the encoder core (`libmpcenc`, `libmpcpsy`), a WAV helper
(`libwavformat`), shared sources (`common/`), and the CLI tools (`mpcdec`,
`mpcenc`, `mpc2sv8`, `mpccut`, `mpcgain`, `mpcchap`, `wavcmp`). The decoder
supports SV7/SV8; the encoder produces SV8.

This is a **modernized** copy of upstream Musepack r475 (git `05d97a5`).
The working rule is: **modernize the ecosystem aggressively, change the codec
conservatively.** Any risky codec change must be proven behavior-preserving.

## Licensing and copyright rules

MusicPack is a mixed-license repository.

- Inherited and derivative Musepack encoder code is licensed under
  `LGPL-2.1-or-later`.
- MusicPack-owned permissive code uses `BSD-3-Clause`.
- Third-party/vendored code retains its upstream license.
- Public-domain material uses the repository's explicit public-domain
  LicenseRef where applicable.
- Do not introduce `BSD-2-Clause` for MusicPack-owned code.

For materially modified LGPL Musepack files:

- preserve all original copyright notices;
- retain the SPDX license identifier `LGPL-2.1-or-later`;
- retain/add `Modified by the MusicPack Development Team, 2026.`;
- do not replace historical attribution with MusicPack attribution.

New files under codec directories are not automatically BSD: if code is
derived from, transcribed from, or closely adapted from Musepack LGPL
implementation, it must remain `LGPL-2.1-or-later`. In particular, the SIMD
files `libmpcpsy/fft4g_simd.c`, `libmpcpsy/fft_routines_simd.c`, and
`libmpcenc/analy_filter_simd.c` are LGPL-derived code, not BSD code — do not
relicense them.

Generic independently written infrastructure and MusicPack ecosystem code may
use `BSD-3-Clause` where appropriate.

Do not rewrite vendored or historical notices merely for style consistency.

The repository is REUSE-compliant. New files must keep that compliance: use
SPDX copyright/license metadata and ensure the corresponding license is
present under `LICENSES/`. Before committing licensing-related changes, run:

    reuse lint

Current intended license files include `BSD-3-Clause`, `LGPL-2.1-or-later`,
`MIT`, `Zlib`, and `LicenseRef-Public-Domain`.

Licensing changes must not be mixed with functional codec changes.

## Stable decoder API (`libmusepack`)

Since Phase 1, the canonical decoder-facing API lives in `include/musepack/`
(`<musepack/musepack.h>`): an opaque `musepack_decoder` session
(`musepack_decoder_open/read/seek/...`) over the `mpc_reader` input
abstraction, implemented as a thin facade on `mpc_demux` in
`libmpcdec/musepack_decoder.c`. New code should use this API. The historical
`include/mpc/*.h` headers stay installed for compatibility. The decoder also
builds to WebAssembly (`wasm/`, Emscripten) with a browser demo in `demo/`.
Library target is `musepack` (output `libmusepack`), exported as
`Musepack::Decoder`; `mpcdec` remains an in-tree alias.

## Package library (`libmusicpack`)

Since Phase 2, `.mpack` package semantics live in `libmusicpack/` (manifest,
album/track model, assets, SHA-256, BS.1770 loudness, directory storage),
exported as `MusicPack::Package`. The `musicpack` CLI (`info`/`verify`/
`create`/`import`) and the `mpack` CTest suites cover it. The dependency
direction is one-way: `libmusicpack` may use `libmusepack` (the Musepack
reader handoff), never the reverse. The normative spec is
`specs/musicpack-v1.md`; reference packages live in `tests/reference/`.
`libmusepack` must remain codec-only and package-agnostic.

## Building

CMake 3.16+. Configure, build, test:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

Options (see CMakeLists.txt):

| Option               | Default            | Description                              |
|----------------------|--------------------|------------------------------------------|
| `MPC_BUILD_SHARED`   | `ON` (non-Windows) | Build `libmpcdec` as a shared library    |
| `MPC_BUILD_TESTS`    | `OFF`              | Register the CTest suites                |
| `MPC_BUILD_MPCGAIN`  | `ON`               | `mpcgain` (needs libreplaygain)          |
| `MPC_BUILD_MPCCHAP`  | `ON`               | `mpcchap` (needs libcuefile)             |
| `MPC_ENABLE_SIMD`    | `ON`               | Build supported decoder/encoder SIMD     |
| `MPC_ENABLE_NATIVE_TUNING` | `OFF`         | Opt in to `-march=native`                |
| `MPC_ENABLE_PSY_PROFILE` | `OFF`            | Add psychoacoustic CPU-time counters     |

Enable tests with `-DMPC_BUILD_TESTS=ON`. Local CI runs on Linux/macOS/Windows.

## Tests

CTest coverage lives under `tests/`. Important codec-facing tests include:

| Test          | What it checks                                             |
|---------------|------------------------------------------------------------|
| `unit`        | crc32, bit writer/reader, size codes, Cnk tables, Huffman  |
| `api`         | stable `libmusepack` decoder API                           |
| `fixtures`    | decode(fixture.mpc) ≈ golden .wav (tolerance ±2 LSB)       |
| `integration` | end-to-end encode/decode/seek/cut/compare                  |
| `fuzz`        | decoder survives truncated/bit-flipped input               |
| `compat`      | core corpus byte-identical to the reference encoder        |
| `enc_compat`  | extended q5/q6/q7 corpus byte-identical to reference       |
| `synth_ab`    | decoder scalar/SIMD differential                           |
| `enc_ab`      | analysis-filter scalar/SIMD differential                   |
| `psy_ab`      | psychoacoustic scalar/SIMD differential                    |

Package, Sonic, authoring, and server tests are registered alongside these;
inspect `tests/CMakeLists.txt` for the current complete list.

Key details:

- Shell integration/fuzz suites are registered only on UNIX. `compat` and
  `enc_compat` run on all platforms (Windows invokes them through Git Bash).
  Unit, package, server-core, and direct SIMD A/B tests run on all platforms.
- **Fixture comparison is tolerance-based, not byte-exact.** `mpcdec -i`
  uses `pow`/`log10` (ReplayGain), and cross-platform libm/codegen can flip
  a 16-bit sample by ±1 LSB. Use `tests/wavcmp_tol.py` for comparisons.
- **Encoder byte-identity is optimization- and toolchain-dependent.** The
  `compat` test compares against a reference binary built on the same CI
  runner with the same flags (live mode, `REF_MPCENC`), which sidesteps this.
  The committed manifest (`tests/reference_manifest.txt`) is a `-O0` clang
  fallback for local runs and only matches a `-O0` build of the encoder under
  test.
- **License/header-only changes to codec sources must be verified to contain
  no executable-code differences** (strip comments and diff). Never mix
  licensing changes with functional codec changes.

## The reference encoder and `compat`

`compat` regenerates a deterministic corpus (`tests/generate_corpus.py`),
encodes with the built `mpcenc`, and compares SHA-256 at Q{3,5,7}. Two modes:

- **Live (CI):** pass the reference `mpcenc` via `REF_MPCENC` (or as the
  script's second argument). Expected hashes are produced by the reference
  binary on the spot, so both binaries share compiler/flags. CI builds the
  pristine reference from `05d97a5` in a worktree, patches its CMakeLists
  (see below), and runs `compat` this way on all three platforms.
- **Manifest (local fallback):** with no reference binary, compare against
  `tests/reference_manifest.txt` (a `-O0` reference build).

To build the reference commit in a separate worktree:

```sh
git worktree add --detach /tmp/musepack-ref 05d97a5
python3 tests/patch_reference.py /tmp/musepack-ref
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -S /tmp/musepack-ref -B /tmp/ref-build
cmake --build /tmp/ref-build -j --target mpcenc
python3 tests/generate_corpus.py /tmp/corpus
# then SHA-256 every encode at Q{3,5,7} -> tests/reference_manifest.txt
# (or run tests/run_compat.sh <your-mpcenc> /tmp/ref-build/mpcenc/mpcenc for live mode)
```

`tests/patch_reference.py` makes the pristine tree configure cleanly on CI:
it keeps only the encoder subdirs (`libmpcpsy`/`libmpcenc`/`mpcenc`/`include`),
because `mpcgain`/`mpcchap` hard-fail without libreplaygain/libcuefile and
`mpcdec`/`mpc2sv8`/`mpccut` declare duplicate `add_executable` targets under
MSVC. It keeps the reference's hardcoded `-O3` optimization (matching the
Unix Release CI build) and adds `-Wno-error=incompatible-pointer-types`
for GCC 14+. It also applies narrowly proven source corrections already
present in the modernized tree: MSVC `log2`/`asinh` name collisions, the
zero-sample-rate guard, in-bounds `Vectoring` pointers, unsigned bitstream
shifts, matching quantizer declarations, and writable runtime-initialized
FAST_MATH tables. These patches are required for a reliable same-toolchain
Release reference; do not remove them merely to make it look more pristine.

Verified facts (from the compatibility audit):
- Reference vs modernized are byte-identical at matched optimization
  (`-O0` and `-O3`, 0/693 differing either way).
- `-O0` vs `-O3` outputs differ in a few cases (e.g. `clipping_edge` q7) —
  floating-point codegen, not code deltas.
- FAST_MATH scope was narrowed from global (reference) to `mpcpsy`/`mpcenc`
  only (modernized) with zero effect: `libmpcenc` contains no FAST_MATH-guarded
  code.

## Phase 3 psychoacoustic SIMD

Phase 3 is **complete**. Reproducible q5/q6/q7 profiles and retained ARM64 and
x86-64 reports live in `docs/measurements/phase3/`; the milestone verdict is
in `docs/psy-optimization-milestone-3.md`.

- `mpcenc --psy-impl scalar|simd` changes only psychoacoustic dispatch and
  fails closed when SIMD is unavailable. The analyser remains AUTO in both
  benchmark arms.
- `psy_ab` compares distinct scalar/SIMD kernels, batches, and evolving model
  state. `enc_ab` and `synth_ab` also fail closed without their SIMD kernels.
- `bench/encode_bench.sh`, `bench/psy_fft_bench.c`, and
  `bench/profile_psy.py` provide end-to-end, isolated-kernel, and CPU-share
  evidence. Profiling counters exist only with `MPC_ENABLE_PSY_PROFILE=ON`.
- FFT measured below 35% of total encoder CPU on both architectures. The 35%
  rule is a decision gate for a broader FFT rewrite, not a completion gate.
  Do not start further FFT work without new profiling evidence and a separate
  scope decision.
- Retained exact optimizations improve end-to-end encoding by about 5-10%
  while remaining byte-identical to the same-toolchain reference.

## Encoder SIMD optimization track: closed

The encoder SIMD optimization track is **complete**. The final measured step
was CVD L/R cepstrum FFT batching and the fine-grained CVD/CEP profiling that
followed it (see `docs/measurements/cvd-analysis-profile/`). The verdict:

> Further encoder SIMD optimization is not justified at this point — expected
> gains are now marginal relative to implementation complexity, portability
> cost, and bit-exactness risk.

Completed: decoder synthesis SIMD; encoder analysis-filterbank SIMD; psycho
spectrum/windowing SIMD; lane-parallel FFT; CVD L/R cepstrum FFT batching;
ARM64 NEON and x86-64 SSE2 validation; scalar/SIMD byte identity; frozen
encoder compatibility; fine-grained psycho/CVD profiling; seek-table UB
hardening.

Deliberately not pursued: `CEP_correlation`/`CEP_maxsearch` SIMD, `logfast`
optimization, `CalcUnpred`/`CalcTemporalThreshold`/broader scalar-psy loop
vectorization, AVX2/AVX-512, FMA/reassociation, fast-math, approximate
reciprocal/sqrt, and threading of the process-global psycho state. Reopening
any of these requires new profiling evidence and a separate scope decision.

## Undefined behavior

Both of these existed in pristine r475 and were fixed in the modernization
audit; do not reintroduce them:

- `libmpcpsy/psy_tab.c` — `(int)(BandWidth * 64. / SampleFreq)` with 0/0 at
  init (NaN cast, clamped by the 1..31 guards). Now guarded: the division is
  only evaluated when `SampleFreq != 0`.
- `libmpcdec/mpc_bits_reader.h` — left shift of a signed byte by 24
  (`r->buff[-3] << 24`, `r->buff[-4] << (32 - count)`) and `1 << n` in
  `mpc_bits_enum_dec`. Now shifted in `mpc_uint32_t`/`1u`.
- `libmpcpsy/profile.c::SetQualityParams` — `Profiles[i+1]` read past the
  table at `qual == 10` (index 16 of 16). Now uses a clamped second index.
- `libmpcenc/analy_filter.c::Vectoring` (FASTER path) — `c1 = Ci_opt - 8`
  formed a pointer before the array. Now uses post-increment with in-bounds
  initial pointers (trace-identical).
- `libmpcenc/encode_sv7.c::writeBitstream_SV8` — `idx <<= 1` over a signed
  int. Now `unsigned int idx`.

All fixes are proven byte-identical to the reference encoder (0/693 encodes
differ at both `-O0` and `-O3`) and sanitizer-clean (ASan+UBSan, 63 corpus
files × multiple qualities, encoder and decoder).

When running sanitizer builds, note the reference CMakeLists hardcodes
`CMAKE_C_FLAGS`, silently overriding `-DCMAKE_C_FLAGS` — instrument the
reference by editing its worktree CMakeLists, or the "clean" result is a false
negative.

## Codec internals worth knowing

- `mpcenc/mpcenc.c::Quantisierung` uses per-channel static error buffers
  `errorL`/`errorR` (`[32][36 + MAX_NS_ORDER]`). The upstream code had a
  copy-paste bug: the R-channel no-noise-shaping branch passed `errorL[Band]`
  instead of `errorR[Band]`. This is a **real latent bug with zero observable
  effect** (the error-buffer carry is dead: every
  `QuantizeSubbandWithNoiseShaping` memsets `errors[0..5]` first, so the
  carry is never consumed). The fix is correct — **keep it**, don't revert.
- `MAX_NS_ORDER = 6` (`libmpcpsy/libmpcpsy.h`). `NS_Order_L/R` are per-band,
  reset each frame in `NS_Analyse`, set by `FindOptimalANS`.
- Psychoacoustic working history is per-instance (`psy_state_t` embedded in
  `PsyModel`), but implementation dispatch and FFT/FAST_MATH tables remain
  process-global. Do not add new file-scope mutable model state or claim that
  concurrent encoder sessions are fully proven reentrant.
- Combinatorial tables (`Cnk`/`Cnk_len`/`Cnk_lost`) live in
  `common/cnk_tables.h`, shared by decoder and encoder.

## Conventions

- C11, `-Wall -Wextra`, no warnings. No comments unless they explain a
  non-obvious decision (this repo's style is terse).
- Standard `<stdint.h>` types; use `common/fileio.h` helpers for 64-bit I/O
  (`fseeko`/`ftello`; `_fseeki64` on Windows).
- Keep `libmpcdec`/`libmpcenc` public-API-clean: tools should use the public
  headers in `include/mpc/`, not reach into private structs.
- `mpc_seek_t` is `mpc_uint64_t`; the reader interface uses it.
- Legacy autotools/VS2005 files live in `legacy/` (kept for reference, not
  part of the build).

## Git / workflow

- Only commit/push when asked. Stage only intended files; write concise
  commit messages matching the repo history.
- The repo has a working `.gitignore` (build dirs, `*.o`, etc.).
- Reference worktrees and audit scratch builds belong under
  `/var/folders/6h/83xm7ljx4sz5njxdhkjsq53h0000gn/T/opencode` on this machine;
  never commit temp/audit artifacts.
