# CEP_Analyse2048 internal profile (ARM64)

Date: 2026-08-14. Host: Apple M5, arm64, macOS 26.5.1. Compiler: Apple Clang
21.0.0. Build: Release `-O3 -DNDEBUG`, `-ffp-contract=off`, native tuning off,
`MPC_ENABLE_PSY_PROFILE=ON`. Workload: `long_60s_44100.wav` (from
`tests/generate_encoder_corpus.py`). Method: one warm-up + five measured runs,
process CPU-time medians (`CLOCK_PROCESS_CPUTIME_ID`).

Raw results: `raw/cvd-analysis-profile-arm64-scalar.json` and
`raw/cvd-analysis-profile-arm64-simd.json`.

## Instrumentation regions

Added to `cvd.c` (opt-in only, compiled out of normal Release builds):

- `CEP_Analyse2048` — whole function body (both return paths).
- `CEP_correlation` — the cross-correlation loop (`for n = 10..902`, 9-tap
  `Puls[]` dot + 9 squares + one float divide, guarded by `x[0] > 0`).
- `CEP_maxsearch` — the two best-lag max-search loops (`900..50` and
  `100..24`), summed into one counter.
- `logfast` — the two logfast loops in `CVD2048_prepare`.

The interpolation, octave-upsample, `memset` and the early-return branch remain
unmeasured ("other").

## Caveat: instrumentation overhead

Each `CEP_Analyse2048` call now contains ~8 `clock_gettime` calls; this raises
the measured `CVD2048` / `CEP_Analyse2048` totals and their `% encoder`
denominator by a roughly constant amount (measured `total_cpu` ≈ 260 ms vs
~251 ms clean). The **region body times** (`CEP_correlation`, `CEP_maxsearch`)
exclude their own boundary reads and are clean; the `other` bucket is ~80%
instrumentation overhead, not real work. Use the region body times and the
previous (pre-instrumentation) `CVD2048 ≈ 24 ms` figure for clean shares.

## q6 breakdown (SIMD build, the post-FFT baseline)

| Region | body time | % CEP_Analyse2048 (clean) | % CVD2048 (clean) | % total encoder (clean) |
|---:|---:|---:|---:|---:|
| CEP_correlation | 9.6 ms | ~70% | ~40% | ~3.8% |
| CEP_maxsearch | 4.2 ms | ~30% | ~17% | ~1.7% |
| interpolation/upsample/memset | <1 ms | — | — | — |
| (CEP_Analyse2048 total, clean) | ~14 ms | 100% | ~58% | ~5.5% |
| logfast | 1.7 ms | — | ~7% | ~0.65% |
| cepstrum FFT (rdft4) | 7.7 ms | — | ~32% | ~3.1% |

CVD2048 clean total ≈ 24 ms ≈ 9.5% of encoder (cepstrum FFT + CEP_Analyse2048
+ logfast + prepare). q5 and q7 are within ±0.2 ms of q6 for every region.

## Interpretation

The remaining CVD2048 cost is dominated by the cross-correlation loop
(~70% of CEP_Analyse2048, ~3.8% of encoder). The max-search is ~30%
(~1.7%), and `logfast` is negligible (~0.65%). The two max-search loops are a
sequential running-max with strict-`>` update and `>=` local-peak tie checks —
not safely vectorizable. `logfast` operates in double precision and does not
map to the 128-bit `f32x4` abstraction. The correlation loop is the only
candidate, and it requires new `mpc_simd.h` primitives (SIMD divide + masked
select) plus a ~50% branch-discard, for a net of at most ~1.5–2% end-to-end.

## Top remaining scalar psychoacoustic functions (q6, SIMD, % of encoder)

CalcUnpred 3.4%, CalcTemporalThreshold 2.7%, FindOptimalANS 2.4%,
CalcShortThreshold 2.3%, AdaptThresholds 2.3%, SpreadingSignal 1.8%,
PartitionEnergy 1.7%, SubbandEnergy 1.6%, ApplyLtq 1.6%, CalculateSMR 1.3%.
None exceeds ~4% and most are libm-bound (`sqrt`/`pow`) or data-dependent.

## Conclusion

No single remaining scalar region is both ≥2% of encoder and cleanly
bit-exact-vectorizable with the existing `mpc_simd.h`. Further CVD or
psychoacoustic SIMD is not justified at this point.
