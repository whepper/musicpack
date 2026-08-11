/*
 * WebAssembly decoder benchmark (Node.js).
 *
 * Loads the Emscripten module (build-wasm/wasm/musepack.js), decodes each
 * .mpc fully via _mpc_wasm_open/_mpc_wasm_read, and reports wall-clock
 * decode time and the realtime multiplier. Also measures the JS<->wasm
 * boundary cost by decoding with two block sizes (1152 and 4608 frames per
 * _mpc_wasm_read call). Measure-only: no API change.
 *
 * Usage: node wasm_bench.mjs <module.js> <file.mpc> [--blocks 1152,4608]
 */

import { createRequire } from "module";
import path from "path";

const require = createRequire(import.meta.url);
const fs = require("fs");

const moduleJs = process.argv[2];
const inputMpc = process.argv[3];
const blocksArg = process.argv.find((a) => a.startsWith("--blocks="));
const blockSizes = blocksArg
  ? blocksArg.split("=")[1].split(",").map(Number)
  : [1152, 4608];

if (!moduleJs || !inputMpc) {
  console.error("usage: node wasm_bench.mjs <module.js> <file.mpc>");
  process.exit(2);
}

const bytes = fs.readFileSync(path.resolve(inputMpc));
const module_ = require(path.resolve(moduleJs));

function openDecoder(M) {
  const h = M._mpc_wasm_create();
  const inPtr = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, inPtr);
  const err = M._mpc_wasm_open(h, inPtr, bytes.length);
  if (err !== 0) throw new Error(`mpc_wasm_open returned ${err}`);
  return { h, inPtr };
}

async function main() {
  const M = await module_();

  for (const block of blockSizes) {
    // Warm-up decode (lazy init, caches).
    {
      const d = openDecoder(M);
      const ptr = M._malloc(block * 4 * 2);
      let frames;
      do { frames = M._mpc_wasm_read(d.h, ptr, block); } while (frames > 0);
      M._free(ptr);
      M._free(d.inPtr);
      M._mpc_wasm_destroy(d.h);
    }

    const d = openDecoder(M);
    const ptr = M._malloc(block * 4 * 2);
    const rateHz = M._mpc_wasm_sample_rate(d.h);
    const chan = M._mpc_wasm_channels(d.h);
    const audioS = M._mpc_wasm_length_samples(d.h) / rateHz;

    const t0 = process.hrtime.bigint();
    let total = 0;
    let frames;
    do {
      frames = M._mpc_wasm_read(d.h, ptr, block);
      if (frames > 0) total += frames;
    } while (frames > 0);
    const wallMs = Number(process.hrtime.bigint() - t0) / 1e6;

    M._free(ptr);
    M._free(d.inPtr);
    M._mpc_wasm_destroy(d.h);

    console.log(
      `${inputMpc}\t${rateHz}hz\t${chan}ch\tblock=${block}\t` +
      `${audioS.toFixed(3)}s\taudio\tsum=${total}\t${wallMs.toFixed(1)}ms\t` +
      `${(audioS / (wallMs / 1e3)).toFixed(2)}x`
    );
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
