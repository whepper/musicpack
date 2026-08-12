## Environment

- Commit: `21341e90bbe57623359a647f1c93c005d28325b3`
- Runner: Linux 6.17.0-1020-azure, x86-64, AMD EPYC 7763
- Node: 22.23.1; V8: `12.4.254.21-node.56`
- Emscripten: 6.0.6 (`ce75e06884093bcefb86a6b8fd56a5d62a4cc245`)
- CMake: 3.31.6
- Build: Release, `-O3 -DNDEBUG`, native tuning off
- Workflow: https://github.com/whepper/musicpack/actions/runs/31591449091

Configurations:

- Scalar: `MPC_WASM_SIMD=OFF`, `MPC_ENABLE_SIMD=OFF`
- Autovec: `MPC_WASM_SIMD=ON`, `MPC_ENABLE_SIMD=OFF`
- Explicit: `MPC_WASM_SIMD=ON`, `MPC_ENABLE_SIMD=ON`

The artifact records each configuration's JS and Wasm SHA-256 plus every
input hash. For example, scalar Wasm SHA-256 is
`0d55a8524b9df54f80e75aeb90a3926a63b0b6e8ef2e0940ccee87cdc5ecdc18`.

## Method

Five independent repetitions at a 1152-frame read block. Configuration order
cycles scalar/autovec/explicit, explicit/scalar/autovec, and
autovec/explicit/scalar. Every decode requires canonical EOF and exact sample
completion.

## 48-second fixture result

| Configuration | Median realtime-x | Min-max | vs scalar |
|---|---:|---:|---:|
| scalar | 768x | 761-799x | 1.00x |
| autovec | 825x | 808-830x | 1.07x |
| explicit SIMD | 1979x | 1937-2010x | **2.58x** |

Raw and full summary TSVs are retained in the `bench-wasm` workflow artifact.
