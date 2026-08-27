# Final Musepack encoder psychoacoustic SIMD investigation

Status: **COMPLETE — no further SIMD is justified; encoder optimization is
closed.** This is the final encoder optimization investigation before the project
moves to real-world MusicPack dogfooding and production validation.

## Objective

Determine whether one final worthwhile SIMD optimization exists inside the
encoder psychoacoustic model (`Psychoakustisches_Modell`, `codec/libmpcpsy/psy.c`),
and implement it only if profiling demonstrates that it is justified. The five
functions the task suspected were `CalcUnpred`, `SubbandEnergy`, `ApplyLtq`,
`CalculateSMR`, `PreechoControl`.

## Method

Reused the existing opt-in fine-grained profiling infrastructure rather than
inventing new instrumentation:

- `MPC_ENABLE_PSY_PROFILE=ON` wraps every sub-function in `libmpcpsy` with
  process-CPU timers (`psy_profile.{h,c}`); the encoder emits JSON to
  `MPC_PSY_PROFILE_OUT` (`mpcenc.c:1811`); `bench/profile_psy.py` drives
  reproducible q5/q6/q7 runs and prints a `%model / %psy / %encoder /
  calls-per-frame` ranking.
- Workload: deterministic `long_60s_44100.wav` (sha256 `df1402c9…dba8`, from
  `tests/generate_encoder_corpus.py`), identical to the committed evidence so
  ARM64 re-runs are comparable.
- ARM64: native Apple Clang aarch64, NEON path.
- x86-64: an x86_64/SSE2 build executed under Rosetta 2 on the same Apple
  Silicon host (the genuine gap this task required — the repo previously had
  only coarse x86-64 FFT-share numbers). Rosetta inflates absolute times but
  preserves relative per-function shares; the coarse Phase-3 x86-64 share
  (77.57% `libmpcpsy`/encoder) agrees with the fine-grained 77.82% psy/encoder
  here. See `docs/measurements/psy-model-profile/x86_64-rosetta.md` for the
  caveat.

Build: `cmake -B build -DCMAKE_BUILD_TYPE=Release
-DCMAKE_C_FLAGS="-O3 -DNDEBUG -ffp-contract=off" -DMPC_ENABLE_PSY_PROFILE=ON
-DMPC_ENABLE_SIMD=ON`, then `cmake --build build --target mpcenc`.

## What profiling showed

### Per-function attribution (q6, SIMD, % of total encoder CPU)

| Function | arm64 | x86-64 (Rosetta) | Notes |
|---|---:|---:|---|
| CVD2048 | 13.3 | 15.1 | cepstrum FFT already vectorized (rdft4); rest = CEP internals |
| CEP_Analyse2048 | 7.1 | 9.5 | includes cross-correlation + max-search |
| CEP_correlation | 3.7 | 3.3 | 9-tap dot + divide; branch-discard ~50% |
| CEP_maxsearch | 1.6 | 4.5 | running-max; not safely vectorizable |
| **CalcUnpred** | **3.4** | **3.1** | per-sample `cos`+`sqrt`+`fabs` (trig) |
| CalcTemporalThreshold | 2.7 | 2.8 | `pow` per partition |
| FindOptimalANS | 2.4 | 3.0 | |
| AdaptThresholds | 2.3 | 2.6 | |
| CalcShortThreshold | — | 1.9 | |
| SpreadingSignal | 1.8 | 1.7 | |
| SubbandEnergy | 1.7 | 1.6 | branchy aliasing loop |
| PartitionEnergy | 1.7 | 1.3 | `sqrt` inner loop |
| ApplyTonalityOffset | 1.6 | 1.3 | `pow` per partition |
| **ApplyLtq** | **1.6** | **1.3** | `sqrt` per line |
| **CalculateSMR** | **1.3** | **1.1** | 16-wide min reduction |
| RaiseSMR_Signal | 1.2 | 0.9 | |
| CalcMSThreshold | 1.2 | 0.9 | `double` ratio math |
| WeightedPartitionEnergy | 1.2 | 0.8 | `sqrt` inner loop |
| **PreechoControl** | **0.4** | **0.4** | trivial `minf` |

(`—` = below the printed threshold in that run; sub-counter present in raw JSON.)

### Which function(s) dominated

`CVD2048` dominates on both architectures (already vectorized at the cepstrum-FFT
level via `Cepstrum2048_2`/rdft4). Its remaining cost is the cross-correlation
and max-search loops (`CEP_correlation` + `CEP_maxsearch`), which a prior
`cvd-analysis-profile` study already measured and ruled out (new SIMD divide +
masked-select primitives, ~50% branch discard, net ~1.5–2% end-to-end, not
bit-exact-vectorizable with the existing `f32x4`).

Among the five named candidates, `CalcUnpred` is the largest at ~3% of encoder;
the rest are ≤1.7%.

### SIMD justified?

**No.** For each named candidate:

- **CalcUnpred** — per-sample `cos`/`sqrt`/`fabs`. Trigonometry cannot be
  reproduced bit-exactly by the existing `f32x4` abstraction (no cos/sin
  primitive); a custom SIMD transcendental would risk Musepack bit-exactness and
  is out of scope. ~3% encoder.
- **SubbandEnergy** — 16-iteration inner loop with data-dependent aliasing
  branches and negative-offset pointer arithmetic; small and branchy. <2%.
- **ApplyLtq** — nested loop, `sqrt` per line; small, libm-bound. ~1.3%.
- **CalculateSMR** — 16-wide `minf` reduction; trivially small. ~1.1%.
- **PreechoControl** — two `minf` per partition; negligible. ~0.4%.

All are individually too small and too libm-bound / branchy / data-dependent to
be worth a SIMD port that would add complexity, portability cost, and
bit-exactness risk for a marginal (<~2% end-to-end) gain. The x86-64 measurement
did **not** surface any named candidate materially larger than on arm64, so the
prior conclusion stands.

## Decision

Do **not** implement further psychoacoustic SIMD. `CVD2048`'s one real hotspot
(cepstrum FFT) was already vectorized in the retained Phase 3 work; the
remaining CVD internals and all five named candidate functions are individually
below the bar for a clean, bit-exact, portable SIMD win.

This is consistent with the existing `AGENTS.md` note: *"Encoder SIMD
optimization track: closed."* The x86-64 fine-grained attribution was the only
missing piece and it confirms the closure.

## Validation performed

- **Scalar vs SIMD differential (byte-identity):** encoding the 60 s workload at
  q6 with `--psy-impl scalar` and `--psy-impl simd` produced **byte-identical**
  `.mpc` files (sha256 `d4d417b6…`). The existing A/B invariant holds for this
  build.
- **No production code changed**, so the established compatibility/byte-identity
  gates (`tests/run_compat.sh`, `run_sv8.sh`, `run_encode.sh`, `psy_ab`/`enc_ab`/
  `synth_ab`) remain green by construction. The x86-64 measurement did not
  contradict the current conclusion, so no `codec/` modification was made (per
  the investigation's stop rule).

## Benchmark results

There is no "after" to benchmark: no optimization was implemented. The baseline
fine-grained profiles (this run) are the recorded state:

- ARM64 (NEON) q6 SIMD: total CPU 257.7 ms, psy/encoder 79.31%, FFT/encoder
  14.01%.
- x86-64 (SSE2, Rosetta) q6 SIMD: total CPU 343.3 ms (translated), psy/encoder
  77.82%, FFT/encoder 12.42%.
- Retained Phase 3 end-to-end psycho SIMD gains (already shipped): ~9.7% ARM64,
  ~5.5% x86-64 at q6 — these are the last justified encoder gains.

## Compatibility / byte-identity

Unchanged (no code change). Scalar==SIMD confirmed byte-identical on the
representative workload. The reference corpus remains byte-identical to the
pristine same-toolchain encoder at q5/q6/q7 by construction.

## Tests executed

- Fine-grained profiling: ARM64 (simd + scalar) and x86-64 (simd + scalar) at
  q5/q6/q7, 5 measured runs each.
- Scalar-vs-SIMD byte-identity differential on the 60 s workload (pass).
- Full `compat`/`enc_compat`/CTest re-run was not needed because no production
  code changed; the gates are preserved by construction.

## Files changed

Documentation and benchmark artifacts only (no `codec/` changes):

- `docs/psy-optimization-final.md` — this report.
- `docs/measurements/psy-model-profile/x86_64-rosetta.md` — x86-64 fine-grained
  evidence + caveat.
- `bench/results/psy-model-profile-x86_64-rosetta-simd.json`,
  `…-scalar.json` — raw x86-64 results.
- `bench/results/psy-model-profile-arm64-simd.json` — regenerated ARM64 SIMD
  results (reproduction of committed numbers).

## Recommended next step

Stop encoder SIMD work. The investigation establishes, with evidence on both
ARM64 and x86-64, that no final high-value SIMD optimization remains in the
psychoacoustic model. Move the primary engineering focus to real-world
MusicPack dogfooding and production validation.
