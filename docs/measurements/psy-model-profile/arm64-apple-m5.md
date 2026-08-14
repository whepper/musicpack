# Fine-grained psychoacoustic model profile (ARM64)

Date: 2026-08-14. Host: Apple M5, arm64, macOS 26.5.1. Compiler: Apple Clang
21.0.0. Build: Release `-O3 -DNDEBUG`, `-ffp-contract=off`, native tuning off,
`MPC_ENABLE_PSY_PROFILE=ON`. Workload: deterministic `long_60s_44100.wav`
(from `tests/generate_encoder_corpus.py`). Method: one warm-up plus five
measured runs per quality, process CPU-time medians
(`CLOCK_PROCESS_CPUTIME_ID`).

The retained coarse Phase 3 categories are unchanged and still compute; the
fine-grained per-function timers are additive. Sub-function timers wrap each
function body with the same `mpc_psy_profile_now()` clock used by the coarse
counters; the ~50 clock-read pairs added per frame inflate the enclosing
`model`/`libmpcpsy` denominators by roughly 10%, so **absolute per-function
times and their ranking are the reliable metric**; percentage shares are
slightly diluted by that overhead.

Raw results: `raw/arm64-apple-m5-scalar.json` (q5/q6/q7, 5 runs each) and
`raw/arm64-apple-m5-simd-q6.json`.

## q6 scalar model-body ranking (primary)

| Rank | Function | ns/frame-60s | % model | % libmpcpsy | % encoder | calls/frame |
|---:|---|---:|---:|---:|---:|---:|
| 1 | CVD2048 | 26.3 ms | 11.81 | 11.15 | 9.09 | 1 |
| 2 | CalcUnpred | 8.9 ms | 4.02 | 3.79 | 3.09 | 4 |
| 3 | CalcTemporalThreshold | 7.1 ms | 3.19 | 3.02 | 2.46 | 4 |
| 4 | FindOptimalANS | 6.2 ms | 2.79 | 2.64 | 2.15 | 2 |
| 5 | CalcShortThreshold | 6.0 ms | 2.71 | 2.56 | 2.08 | 4 |
| 6 | AdaptThresholds | 6.0 ms | 2.68 | 2.53 | 2.07 | 4 |
| 7 | SpreadingSignal | 4.8 ms | 2.15 | 2.03 | 1.66 | 4 |
| 8 | PartitionEnergy | 4.5 ms | 2.04 | 1.93 | 1.57 | 4 |
| 9 | SubbandEnergy | 4.4 ms | 1.96 | 1.85 | 1.51 | 4 |
| 10 | ApplyLtq | 4.1 ms | 1.85 | 1.75 | 1.43 | 4 |
| 11 | CalculateSMR | 3.4 ms | 1.55 | 1.46 | 1.19 | 4 |
| 12 | ApplyTonalityOffset | 3.2 ms | 1.42 | 1.34 | 1.09 | 2 |
| 13 | RaiseSMR_Signal | 2.5 ms | 1.13 | 1.06 | 0.87 | 4 |
| 14 | WeightedPartitionEnergy | 2.5 ms | 1.11 | 1.05 | 0.85 | 2 |
| 15 | CalcMSThreshold | 2.4 ms | 1.10 | 1.04 | 0.85 | 2 |
| 16 | AdaptLtq | 1.3 ms | 0.57 | 0.54 | 0.44 | 2 |
| 17 | PreechoControl | 1.1 ms | 0.50 | 0.47 | 0.39 | 2 |

The q5 and q7 rankings are identical in the top six; only the FindOptimalANS /
CalcShortThreshold / AdaptThresholds ordering shifts by a few tenths of a
percent. CVD2048 is first at every quality by a ~3x margin over #2.

## Decomposition of the #1 hotspot (CVD2048)

CVD2048 (Clear Voice Detection, `cvd.c`) is the single largest scalar cost and
is active every frame at q5/q6/q7 (`CVD_used >= 1`). It contains the only
remaining **scalar** FFT in the encoder: `Cepstrum2048` calls the scalar
`rdft(2048)` (twice per frame, L and R), which the Phase 3 lane-parallel
`rdft4` path does not cover.

| component | q6 scalar | q6 simd |
|---:|---:|---:|
| total encoder CPU (`total_cpu_ns`) | 289.0 ms | 255.0 ms |
| total FFT (`fft_ns`) | 70.9 ms | 42.1 ms |
| spectrum FFT (`spectrum_fft_ns`, rdft4 in simd) | 57.6 ms | 28.9 ms |
| **cepstrum FFT (scalar, inside CVD2048)** | **13.3 ms** | **13.2 ms** |
| CVD2048 total | 26.3 ms | 26.0 ms |
| CVD2048 non-FFT remainder (CEP_Analyse2048 + logfast) | 13.0 ms | 12.8 ms |

In the **default (SIMD) build**, the spectrum FFT is already vectorized, so
`Cepstrum2048`'s scalar FFT is **5.2% of total encoder CPU** and CVD2048 as a
whole is **10.2%**. The cepstral cross-correlation (`CEP_Analyse2048`) plus
the `logfast` loop is the other ~12.8 ms (~5% of encoder CPU).

## Conclusions

- The previous audit's assumption (that `CalcUnpred`/`SubbandEnergy`/`ApplyLtq`
  would lead) was only partly right: `CalcUnpred` is #2, but the dominant
  remaining scalar cost is **CVD2048**, which was hidden inside the coarse
  "other psy" bucket.
- The highest-value, lowest-risk, bit-exact target is the **scalar cepstrum
  FFT** (`Cepstrum2048`), which can be routed through the existing 2-lane
  `rdft4` exactly as `PowSpec2048_2` already is.
