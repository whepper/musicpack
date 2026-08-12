/*
 * WebAssembly decoder benchmark (Node.js).
 *
 * Loads the Emscripten module (build-wasm/wasm/musepack.js), decodes each
 * .mpc fully via _mpc_wasm_open/_mpc_wasm_read, and reports wall-clock
 * decode time and the realtime multiplier. Also measures the JS<->wasm
 * boundary cost by decoding with two block sizes (1152 and 4608 frames per
 * _mpc_wasm_read call). Measure-only: no API change.
 *
 * Usage: node wasm_bench.mjs <module.js> <file.mpc> [--blocks 1152,4608] [--runs N]
 */

import { createRequire } from "module";
import path from "path";
import os from "os";

const require = createRequire(import.meta.url);
const fs = require("fs");

const moduleJs = process.argv[2];
const inputMpc = process.argv[3];
const blocksArg = process.argv.find((a) => a.startsWith("--blocks="));
const blockSizes = blocksArg
  ? blocksArg.split("=")[1].split(",").map(Number)
  : [1152, 4608];
const runsArg = process.argv.find((a) => a.startsWith("--runs="));
const runs = runsArg ? Number(runsArg.split("=")[1]) : 3;
const configArg = process.argv.find((a) => a.startsWith("--config="));
const config = configArg ? configArg.split("=")[1] : "unknown";
const runOffsetArg = process.argv.find((a) => a.startsWith("--run-offset="));
const runOffset = runOffsetArg ? Number(runOffsetArg.split("=")[1]) : 0;

if (!moduleJs || !inputMpc) {
  console.error("usage: node wasm_bench.mjs <module.js> <file.mpc>");
  process.exit(2);
}
if (!Number.isInteger(runs) || runs < 1 ||
    blockSizes.some((value) => !Number.isInteger(value) || value < 1) ||
    !Number.isInteger(runOffset) || runOffset < 0) {
  console.error("runs, blocks, and run offset must be positive integers");
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
  const crypto = require("crypto");
  const wasmPath = path.join(path.dirname(path.resolve(moduleJs)), "musepack.wasm");
  console.log(`# node: ${process.version} v8: ${process.versions.v8}`);
  console.log(`# os: ${os.type()} ${os.release()} arch=${os.arch()} cpu=${os.cpus()[0]?.model || "unknown"}`);
  console.log(`# js_sha256: ${crypto.createHash("sha256").update(fs.readFileSync(path.resolve(moduleJs))).digest("hex")}`);
  console.log(`# wasm_sha256: ${crypto.createHash("sha256").update(fs.readFileSync(wasmPath)).digest("hex")}`);
  console.log(`# input_sha256: ${crypto.createHash("sha256").update(bytes).digest("hex")}`);

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

    for (let run = 1; run <= runs; run++) {
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
      // MUSEPACK_ERR_EOF from include/musepack/decoder.h.
      if (frames !== -5 || total !== M._mpc_wasm_length_samples(d.h))
        throw new Error(`incomplete decode: frames=${frames} total=${total}`);
      const wallMs = Number(process.hrtime.bigint() - t0) / 1e6;
      M._free(ptr);
      M._free(d.inPtr);
      M._mpc_wasm_destroy(d.h);
      console.log(
        `${inputMpc}\t${rateHz}\t${chan}\t${block}\t${run + runOffset}\t` +
        `${audioS.toFixed(3)}\t${total}\t${wallMs.toFixed(3)}\t` +
        `${(audioS / (wallMs / 1e3)).toFixed(3)}\t${config}`
      );
    }
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
