# Fine-grained psychoacoustic model profile (x86-64, SSE2)

Date: 2026-08-27. Host: Apple M5, arm64, macOS 26.5.1, running an
**x86-64 / SSE2** build translated by Rosetta 2. Compiler: Apple Clang
21.0.0 targeting `x86_64` (`-DCMAKE_OSX_ARCHITECTURES=x86_64`), Release
`-O3 -DNDEBUG`, `-ffp-contract=off`, `MPC_ENABLE_SIMD=ON`
(SSE2 code path), `MPC_ENABLE_PSY_PROFILE=ON`. Workload: deterministic
`long_60s_44100.wav` (sha256 `df1402c9…dba8`, from
`tests/generate_encoder_corpus.py`). Method: one warm-up plus five measured
runs per quality, process CPU-time medians (`CLOCK_PROCESS_CPUTIME_ID`).

Raw results: `bench/results/psy-model-profile-x86_64-rosetta-simd.json` and
`-scalar.json`.

## Why this host

The investigation required per-function attribution on x86-64, which the
repository previously only had at coarse FFT-share granularity
(`docs/measurements/phase3/x86_64-github-epyc.md`). This machine is Apple
Silicon, so the x86-64 numbers here come from an x86_64/SSE2 binary executed
under Rosetta 2. **Caveat:** Rosetta translates the x86-64 instruction stream
to arm64, so absolute times are inflated (roughly 1.3x vs the native arm64
build on the same silicon) and the scalar/SIMD speed gap is compressed. What is
robust under translation is the **relative per-function ranking and share**,
because every counted region runs through the same translator. The coarse
Phase-3 x86-64 share (77.57% `libmpcpsy`/encoder at q6, GCC 13.3, AMD EPYC)
agrees with the fine-grained 77.82% psy/encoder measured here, which supports
the use of this proxy for the *attribution* question.

The fine-grained sub-function timers wrap each function body with the same
`mpc_psy_profile_now()` clock used by the coarse counters; the ~50 clock-read
pairs added per frame inflate the enclosing denominators by roughly 10%, so
**absolute per-function times and their ranking are the reliable metric**;
percentage shares are slightly diluted by that overhead (consistent with the
prior ARM64 fine-grained doc).

## q6 model-body ranking (SIMD / SSE2, primary)

| Rank | Function | ns/frame | % model | % encoder | calls/frame |
|---:|---|---:|---:|---:|---:|
| 1 | CVD2048 | 51.7 ms | 19.3 | 15.1 | 1 |
| 2 | CEP_Analyse2048 | 32.7 ms | 12.2 | 9.5 | 2 |
| 3 | CEP_maxsearch | 15.3 ms | 5.7 | 4.5 | 4 |
| 4 | CEP_correlation | 11.4 ms | 4.3 | 3.3 | 2 |
| 5 | CalcUnpred | 10.8 ms | 4.0 | 3.1 | 4 |
| 6 | CalcTemporalThreshold | 9.4 ms | 3.5 | 2.8 | 4 |
| 7 | FindOptimalANS | 10.5 ms | 3.9 | 3.0 | 2 |
| 8 | AdaptThresholds | 9.0 ms | 3.4 | 2.6 | 4 |
| 9 | CalcShortThreshold | 6.7 ms | 2.5 | 1.9 | 4 |
| 10 | SpreadingSignal | 6.0 ms | 2.2 | 1.7 | 4 |
| 11 | SubbandEnergy | 5.4 ms | 2.0 | 1.6 | 4 |
| 12 | PartitionEnergy | 4.6 ms | 1.7 | 1.3 | 4 |
| 13 | ApplyTonalityOffset | 4.5 ms | 1.7 | 1.3 | 2 |
| 14 | ApplyLtq | 4.3 ms | 1.6 | 1.3 | 4 |
| 15 | CalculateSMR | 3.6 ms | 1.4 | 1.1 | 4 |
| 16 | RaiseSMR_Signal | 3.0 ms | 1.1 | 0.9 | 4 |
| 17 | CalcMSThreshold | 2.9 ms | 1.1 | 0.9 | 2 |
| 18 | WeightedPartitionEnergy | 2.7 ms | 1.0 | 0.8 | 2 |
| 19 | logfast | 2.2 ms | 0.8 | 0.6 | 2 |
| 20 | AdaptLtq | 1.5 ms | 0.6 | 0.5 | 2 |
| 21 | PreechoControl | 1.4 ms | 0.5 | 0.4 | 4 |

## The five functions named by the investigation

| Function | % encoder (x86-64 q6 simd) | % encoder (arm64 q6 simd) | Character |
|---|---:|---:|---|
| CalcUnpred | 3.1 | 3.4 | per-sample `cos`+`sqrt`+`fabs`; trig cannot be bit-exactly vectorized with `f32x4` |
| SubbandEnergy | 1.6 | 1.7 | data-dependent aliasing branches + negative-offset pointers |
| ApplyLtq | 1.3 | 1.6 | `sqrt` per FFT line |
| CalculateSMR | 1.1 | 1.3 | 16-wide `minf` reduction |
| PreechoControl | 0.4 | 0.4 | two `minf` per partition |

All five are ≤3.1% of encoder CPU on x86-64 and ≤3.4% on arm64. They are not
materially larger on x86-64 than on arm64, so the x86-64 measurement does
**not** contradict the existing conclusion.

## Coarse CPU shares (q6)

| config | total CPU | psy/encoder | FFT/encoder | FFT/psy | PowSpec+window/psy | other psy/psy |
|---|---:|---:|---:|---:|---:|---:|
| x86-64 simd | 343.3 ms | 77.82% | 12.42% | 15.98% | 17.69% | 66.30% |
| x86-64 scalar | 389.7 ms | 80.13% | 21.90% | 27.19% | 16.71% | 56.09% |

The `other psy` bucket (66.3% of psychoacoustic model in the SIMD build) is
where the named candidates live, and it is dominated by `CVD2048` and its CEP
internals (rows 1–4 above), not by the five named functions.

## Conclusion

The x86-64 fine-grained attribution matches the arm64 attribution: the five
suspected candidates are individually small and mostly libm-bound or branchy,
while `CVD2048` (already vectorized at the cepstrum-FFT level) and its
cross-correlation/max-search internals dominate. No single named candidate is
both ≥2% of encoder and cleanly bit-exact-vectorizable. This is the
confirmation required before formally closing encoder optimization; see
`docs/psy-optimization-final.md`.
