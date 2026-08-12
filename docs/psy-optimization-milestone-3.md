# Phase 3 — Psychoacoustic SIMD optimization

Status: **NOT COMPLETE.** Correctness and performance evidence are complete,
but the agreed FFT `>=35%` total-encoder CPU admission rule is not satisfied
on either measured architecture. The rule is not retroactively redefined.

## Scope

Phase 3 retains bit-exact SIMD windowing/power kernels and a lane-parallel
four-stream FFT for the existing psychoacoustic spectrum batches. It does not
change psychoacoustic decisions, quality profiles, quantization, codec math,
the FP policy, or the Musepack bitstream.

## Measurement architecture

* `mpcenc --psy-impl scalar|simd` selects only psychoacoustic dispatch and
  rejects unavailable SIMD.
* `bench/encode_bench.sh` measures end-to-end q5/q6/q7 median wall time and
  requires byte-identical scalar/SIMD outputs.
* `bench/psy_fft_bench.c` measures each retained batch kernel and the
  production call mix.
* `MPC_ENABLE_PSY_PROFILE=ON` adds opt-in process CPU-time counters around
  total steady-state psycho work, FFT, and spectrum dispatch. Normal builds
  contain no counter code.
* macOS `sample` and hosted artifacts retain corroborating native evidence.

## ARM64 result

See `docs/measurements/phase3/arm64-apple-m5.md`.

FFT occupies 31.39% / **31.18%** / 30.71% of total encoder CPU at q5/q6/q7,
while total `libmpcpsy` occupies 77.42% / **76.93%** / 75.38%. FFT is about
40.6% of `libmpcpsy`, but the agreed denominator was total encoder CPU.

End-to-end psycho SIMD improves the representative long-track workload by
9.99% / **9.72%** / 9.06% at q5/q6/q7. The isolated production spectrum mix
is 1.55x / **1.62x** / 1.49x faster.

## x86-64 result

See `docs/measurements/phase3/x86_64-github-epyc.md`.

FFT occupies 27.51% / **27.45%** / 27.15% of total encoder CPU at q5/q6/q7.
End-to-end psycho SIMD improves the representative long-track workload by
5.51% / **5.46%** / 6.03%; the isolated production spectrum mix is 1.33x /
**1.37x** / 1.27x faster.

## Correctness

Direct decoder/encoder/psy A/B passes on macOS, Linux, and Windows MSVC.
Local Release runs pass 20/20 applicable CTests and 282/282 q5/q6/q7 live
reference outputs. Windows direct A/B and compatibility pass in hosted CI;
Wasm, web, and research jobs pass. SIMD-disabled builds compile and all three
A/B tools reject unavailable SIMD. Reference manifests and encoder FP flags
remain unchanged.

## Decision rule

The measured FFT share is below 35% of total encoder CPU on both architectures
at every quality. The final verdict is **NOT COMPLETE**, despite real measured
speedups and strong correctness evidence. Do not begin another optimization
phase without a separate decision on whether the admission rule should change
or the retained implementation should be reverted.
