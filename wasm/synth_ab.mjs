// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

import { createRequire } from "module";
import path from "path";

const require = createRequire(import.meta.url);
const [moduleJs, inputMpc] = process.argv.slice(2);
if (!moduleJs || !inputMpc) {
  console.error("usage: node synth_ab.mjs <module.js> <file.mpc>");
  process.exit(2);
}

const bytes = require("fs").readFileSync(path.resolve(inputMpc));
const moduleFactory = require(path.resolve(moduleJs));
const SCALAR = 1;
const SIMD = 2;
const EOF = -5; // MUSEPACK_ERR_EOF, include/musepack/decoder.h

function decode(M, impl) {
  const h = M._mpc_wasm_create();
  const inPtr = M._malloc(bytes.length);
  const outPtr = M._malloc(1152 * 2 * 4);
  M.HEAPU8.set(bytes, inPtr);
  if (M._mpc_wasm_open(h, inPtr, bytes.length) !== 0)
    throw new Error("open failed");
  if (!M._mpc_wasm_set_synth_impl(h, impl))
    throw new Error(`implementation ${impl} is unavailable`);

  const channels = M._mpc_wasm_channels(h);
  const expectedFrames = M._mpc_wasm_length_samples(h);
  const values = [];
  for (;;) {
    const frames = M._mpc_wasm_read(h, outPtr, 1152);
    if (frames === EOF) break;
    if (frames < 0) throw new Error(`read failed: ${frames}`);
    for (let i = 0; i < frames * channels; i++)
      values.push(M.HEAPF32[(outPtr >> 2) + i]);
  }
  M._free(outPtr);
  M._free(inPtr);
  M._mpc_wasm_destroy(h);
  if (values.length !== expectedFrames * channels)
    throw new Error(`decoded ${values.length / channels} of ${expectedFrames} frames`);
  return values;
}

const M = await moduleFactory();
if (!M._mpc_wasm_has_synth_simd())
  throw new Error("explicit SIMD kernel is not compiled in");
const scalar = decode(M, SCALAR);
const simd = decode(M, SIMD);
if (scalar.length !== simd.length)
  throw new Error(`sample count differs: ${scalar.length} vs ${simd.length}`);
let worst = 0;
for (let i = 0; i < scalar.length; i++) {
  if (!Number.isFinite(scalar[i]) || !Number.isFinite(simd[i]))
    throw new Error(`non-finite PCM at ${i}`);
  const diff = Math.abs(scalar[i] - simd[i]);
  worst = Math.max(worst, diff);
  if (diff > 2 / 32768)
    throw new Error(`PCM diverges at ${i}: ${diff}`);
}
console.log(`PASS wasm synth_ab: ${scalar.length} samples, worst diff ${worst}`);
