# Decoder benchmarks

Repeatable measurements for the libmusepack decoder, used by the SIMD
optimization milestone. Every number in the milestone report is reproducible
with the commands below.

## Layout

| path              | purpose                                              |
|-------------------|------------------------------------------------------|
| `decode_bench.c`  | native benchmark (white-box, can force scalar/simd)  |
| `make_corpus.sh`  | generate WAV corpus + encode to SV8 `.mpc`           |
| `run_bench.sh`    | run native bench, record metadata + results CSV      |
| `wasm_bench.mjs`  | Node WASM benchmark (block-size A/B)                 |
| `results/`        | timestamped TSV records (gitignored)                 |

## Build

The bench binary is built whenever tests are enabled:

```sh
cmake -S . -B build -DMPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target mpc_bench
```

A `Release`/`-O3` build type is required for meaningful numbers; the repo
default CI configure does not set one (Linux/macOS would build unoptimized).

## Corpus

```sh
bench/make_corpus.sh /tmp/bench-corpus build/mpcenc/mpcenc
```

Generates ~17 deterministic signal kinds at 32/44.1/48 kHz (from
`tests/generate_corpus.py`), encodes each at Q5 and Q7, and copies the
committed `tests/fixtures/` as short-input references.

## Native benchmark

```sh
bench/run_bench.sh /tmp/bench-corpus            # scalar + simd
bench/run_bench.sh /tmp/bench-corpus --impl scalar --iterations 5
bench/run_bench.sh /tmp/bench-corpus --impl simd --iterations 5
```

Output rows: `file sample_rate channels audio_s wall_ms cpu_ms realtime_x
impl`. The record header captures commit, compiler/version, arch, CPU model,
flags, and the corpus size. Append to `results/<timestamp>.tsv`.

Realtime multiplier = `audio_seconds / wall_seconds`; higher is better.
Iterate a few times on an idle machine; keep the frequency fixed
(`sudo cpupower frequency-set -g performance`) on x86 if numbers look noisy.

## WASM benchmark

Three configurations isolate (a) build-config fixes, (b) LLVM
auto-vectorization, and (c) the explicit SIMD kernel:

```sh
# 1. scalar wasm baseline
emcmake cmake -S . -B build-wasm-scalar -DMPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release \
  -DMPC_WASM_SIMD=OFF -DMPC_ENABLE_SIMD=OFF
cmake --build build-wasm-scalar -j

# 2. auto-vectorization only (-msimd128, scalar C)
emcmake cmake -S . -B build-wasm-autovec -DMPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release \
  -DMPC_WASM_SIMD=ON -DMPC_ENABLE_SIMD=OFF
cmake --build build-wasm-autovec -j

# 3. explicit SIMD kernel
emcmake cmake -S . -B build-wasm-simd -DMPC_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release \
  -DMPC_WASM_SIMD=ON -DMPC_ENABLE_SIMD=ON
cmake --build build-wasm-simd -j

node bench/wasm_bench.mjs build-wasm-scalar/wasm/musepack.js /tmp/bench-corpus/corpus/uncorrelated_noise-q7.mpc
node bench/wasm_bench.mjs build-wasm-autovec/wasm/musepack.js ...
node bench/wasm_bench.mjs build-wasm-simd/wasm/musepack.js ...
```

The `--blocks=1152,4608` option (default both) measures the JS<->wasm
boundary cost per `_mpc_wasm_read` call.

## Methodology (recording)

Record for every optimization: commit, compiler + version, architecture,
CPU model, build flags, corpus, before/after wall time, and percentage
improvement. `run_bench.sh` stamps these into `results/`. Do not merge
performance claims based on single runs; compare medians of >=3 runs.
