# Phase 3 x86-64 evidence

Date: 2026-08-12. Host: GitHub `ubuntu-latest`, AMD EPYC 7763, x86-64.
Compiler: GCC 13.3.0. Build: Release, `-ffp-contract=off`, native tuning off.
Validation source: `848aa31`; strict-reference follow-up `90b9841` changes
only the pristine reference patch. Benchmark workflow:
https://github.com/whepper/musicpack/actions/runs/31582286580.

## Phase A CPU shares

Workload: deterministic `long_60s_44100.wav`, one warm-up plus five measured
runs, opt-in process CPU-time counters.

| quality | total libmpcpsy / encoder | FFT / encoder | FFT / libmpcpsy | PowSpec/window / libmpcpsy | other psy / libmpcpsy |
|---|---:|---:|---:|---:|---:|
| q5 | 77.91% | 27.51% | 35.30% | 23.89% | 40.80% |
| **q6** | **77.57%** | **27.45%** | **35.40%** | **24.03%** | **40.58%** |
| q7 | 76.14% | 27.15% | 35.65% | 24.18% | 40.16% |

Result: **the FFT `>=35%` total-encoder CPU decision rule is not satisfied
on x86-64.** FFT narrowly exceeds 35% inside `libmpcpsy`; that is not the
agreed denominator.

The hosted runner does not provide reliable native `perf` access, so Linux
exp/log/libm attribution remains part of `other psy`. No percentage is
fabricated from unavailable sampling.

## End-to-end psychoacoustic A/B

Aggregate of the three long deterministic tracks. The captured artifact used
three-run medians; the final workflow is configured for five. The analyser is
AUTO in both arms and every scalar/SIMD MPC pair was byte-identical.

| quality | scalar wall | SIMD wall | scalar realtime | SIMD realtime | speedup |
|---|---:|---:|---:|---:|---:|
| q5 | 330.05 ms | 312.82 ms | 363.6x | 383.6x | 5.51% |
| **q6** | **331.10 ms** | **313.95 ms** | **362.4x** | **382.2x** | **5.46%** |
| q7 | 338.65 ms | 319.40 ms | 354.3x | 375.7x | 6.03% |

## Isolated spectrum/FFT production mix

| quality | scalar | SIMD | throughput gain |
|---|---:|---:|---:|
| q5 | 48.742 ms | 36.771 ms | 1.33x |
| **q6** | **51.157 ms** | **37.394 ms** | **1.37x** |
| q7 | 47.028 ms | 37.086 ms | 1.27x |

Artifacts contain the full 94-file q5/q6/q7 TSV, isolated-kernel TSV, and
all scalar/SIMD profile runs.
