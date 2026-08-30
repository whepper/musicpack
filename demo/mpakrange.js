// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

/*
 * Browser/Node MPAK HTTP Range transport (specs/mpak-http-range-design.md).
 *
 * Bridges asynchronous browser fetch() to the synchronous WASM range-source
 * model with the smallest possible bridge: an **async acquire phase** that
 * downloads and validates the complete container over HTTP Range requests,
 * followed by a **synchronous serve phase** whose reads are plain memory
 * copies. This is the demo's proven Phase 4 pattern (see
 * wasm/range_library.js): the WASM module stays single-threaded, no
 * Asyncify and no SharedArrayBuffer/COOP-COEP are required. Demand-driven
 * per-miss fetching (the Phase 5 networker.js model) needs Atomics.wait in
 * a worker plus cross-origin isolation and a JS block cache duplicating the
 * C-side 64 KiB cache; it remains a documented future path.
 *
 * Validation parity with the mpakhttp libcurl adapter:
 *   - discovery: Range: bytes=0-262143; 206 + Content-Range required;
 *     total size fixed for the session; a short '*'-total body is the whole
 *     object, an ambiguous full-length body is probed with bytes=0-0;
 *   - every response: 206 required (200 = server ignored Range -> Tier-B
 *     fail-fast), Content-Range start/end/total must match the request and
 *     the session total, the body must be exactly the declared length;
 *   - Content-Encoding responses are rejected (byte offsets must be
 *     transport-transparent) and so are multipart/byteranges responses
 *     (parity with the mpakhttp adapter);
 *   - a strong ETag from discovery is replayed as If-Range on every block
 *     fetch; a 200 answer (validator mismatch) fails the session;
 *   - 404 maps to MISSING; every other non-206 is a hard transport error;
 *   - one timeout per request (AbortSignal), no retries;
 *   - the acquisition is bounded: a discovery total above maxBytes
 *     (default 2 GiB) is rejected before any container-sized allocation;
 *   - duplicate response headers are combined by fetch (", ") and then
 *     fail Content-Range parsing — fail-closed, like the libcurl adapter's
 *     validation.
 *
 * One-active-source-per-Module: the WASM imports (mpakRangeSize/Read/
 * Destroy) are process-global on the Module, so install() refuses a second
 * source while another is installed (typed INSTALLED error) and destroy()
 * releases the slot. Close one package (or destroy its source) before
 * installing the next.
 *
 * Browser limitations (documented in specs/mpak-http-range-design.md):
 *   - If-Range is not a CORS-safelisted header: for cross-origin containers
 *     it triggers a preflight the server must allow. Disable with
 *     { ifRange: false } for restrictive servers (the total-size check and
 *     the MPAK CRC/SHA-256 integrity layers still apply).
 *   - Content-Range values beyond Number.MAX_SAFE_INTEGER (2^53) are
 *     rejected as malformed; container sizes are far below that.
 *
 * Usage:
 *   const src = await MpakRange.acquire(url);        // async acquire+validate
 *   Module.mpakRangeSize    = src.wasmSize;          // install the imports
 *   Module.mpakRangeRead    = src.wasmRead;          // (wasm/mpak_range_library.js)
 *   Module.mpakRangeDestroy = src.wasmDestroy;
 *   const h = Module._mpak_wasm_open_range(src.size); // sync: MPAK core reads
 *   ...
 *   Module._mpak_wasm_close(h);                      // adopts the source
 *   // on open failure the caller owns src and calls src.destroy() itself
 */

(function (root, factory) {
  if (typeof module !== "undefined" && module.exports) {
    module.exports = factory();
  } else {
    root.MpakRange = factory();
  }
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const DISCOVERY_BYTES = 262144;          // 256 KiB (design §3 R0)
  const BLOCK = 64 * 1024;                 // 64 KiB wire blocks (design §8)
  const DEFAULT_MAX_BYTES = 2 * 1024 * 1024 * 1024;   // 2 GiB
  const MAX_SAFE = Number.MAX_SAFE_INTEGER;

  function MpakRangeError(code, detail, status) {
    const e = new Error("mpakrange: " + code +
                        (detail ? ": " + detail : "") +
                        (status ? " (HTTP " + status + ")" : ""));
    e.name = "MpakRangeError";
    e.code = code;
    e.status = status || 0;
    return e;
  }

  /* Codes mirror demo/reader_mailbox.js + the mpakhttp mapping. */
  const ERR = {
    NETWORK: "NETWORK",     // fetch threw / aborted / connection loss
    TIMEOUT: "TIMEOUT",     // request exceeded the per-request timeout
    HTTP: "HTTP",           // unexpected HTTP status (detail: status)
    MISSING: "MISSING",     // 404 (maps to MUSICPACK_ERR_MISSING upstream)
    ERR_200: "ERR_200",     // server ignored Range (design Tier-B)
    RANGE: "RANGE",         // missing/malformed/mismatched Content-Range
    TRUNCATED: "TRUNCATED", // body length != the declared range length
    ENCODING: "ENCODING",   // Content-Encoding on a range response
    CHANGED: "CHANGED",     // If-Range mismatch / session total changed
    INSTALLED: "INSTALLED", // another source is installed on this Module
    SIZE: "SIZE",           // total exceeds maxBytes / unallocatable
  };

  function isTimeout(e) {
    return e && (e.name === "TimeoutError" ||
                 /timed?\s?out|TIMEOUT/i.test(String(e && e.message)));
  }

  /* strict "bytes <start>-<end>/<total|*>" (unit tokens are case-
     insensitive; anything else — including fetch-combined duplicate
     headers — fails closed) */
  function parseContentRange(v) {
    if (typeof v !== "string") return null;
    const m = /^bytes\s+(\d+)-(\d+)\/(\d+|\*)$/i.exec(v.trim());
    if (!m) return null;
    const start = Number(m[1]);
    const end = Number(m[2]);
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end))
      return null;                                   // > 2^53: malformed here
    if (start > end) return null;
    if (m[3] === "*") return { start, end, total: null };
    const total = Number(m[3]);
    if (!Number.isSafeInteger(total)) return null;
    return { start, end, total };
  }

  function strongETag(res) {
    const v = res.headers.get("ETag");
    if (v == null) return null;
    const t = v.trim();
    return t.length > 0 && !t.startsWith("W/") ? t : null;
  }

  /* One ranged GET with the session's validation. Returns { cr, body }. */
  async function rangedGet(url, rangeStart, rangeEnd, etag, opts) {
    // Accept-Encoding: identity — browsers/undici otherwise auto-decode
    // gzip/deflate and may hide the header, which would silently break
    // byte offsets (same defense as the mpakhttp adapter). Not a
    // CORS-safelisted header: cross-origin containers need a preflight.
    const headers = {
      Range: "bytes=" + rangeStart + "-" + rangeEnd,
      "Accept-Encoding": "identity",
    };
    if (etag) headers["If-Range"] = etag;
    let res, body;
    try {
      res = await fetch(url, {
        headers,
        signal: AbortSignal.timeout(opts.timeoutMs),
        redirect: "follow",
      });
      body = new Uint8Array(await res.arrayBuffer());
    } catch (e) {
      if (isTimeout(e)) throw MpakRangeError(ERR.TIMEOUT, rangeStart);
      throw MpakRangeError(ERR.NETWORK, String((e && e.message) || e));
    }
    if (res.status === 404) throw MpakRangeError(ERR.MISSING, url, 404);
    if (res.status === 200)
      throw MpakRangeError(ERR.ERR_200, "server ignored Range", 200);
    if (res.status !== 206)
      throw MpakRangeError(ERR.HTTP, "expected 206", res.status);
    if (res.headers.get("Content-Encoding"))
      throw MpakRangeError(ERR.ENCODING, "offsets must be transparent",
                           res.status);
    const ctype = res.headers.get("Content-Type");
    if (ctype && /^multipart\/byteranges/i.test(ctype.trim()))
      throw MpakRangeError(ERR.RANGE,
                           "multipart/byteranges responses are not supported",
                           res.status);
    const cr = parseContentRange(res.headers.get("Content-Range"));
    if (!cr || cr.start !== rangeStart)
      throw MpakRangeError(ERR.RANGE,
                           "missing/malformed/mismatched Content-Range",
                           res.status);
    return { res, cr, body };
  }

  function validateRange(cr, body, rangeStart, rangeEnd, size, what) {
    const expectEnd = Math.min(rangeEnd, size - 1);
    if (cr.total != null && cr.total !== size)
      throw MpakRangeError(ERR.CHANGED,
                           "total " + cr.total + " != session " + size);
    if (cr.end !== expectEnd || body.length !== expectEnd - rangeStart + 1)
      throw MpakRangeError(ERR.TRUNCATED,
                           what + ": bytes " + cr.start + "-" + cr.end +
                           ", " + body.length + " bytes");
  }

  /*
   * Acquire phase: download + validate the complete container over Range
   * requests. Resolves with a source object serving reads synchronously.
   */
  async function acquire(url, opts) {
    opts = opts || {};
    const cfg = {
      timeoutMs: opts.timeoutMs > 0 ? opts.timeoutMs : 10000,
      ifRange: opts.ifRange !== false,
      maxBytes: opts.maxBytes > 0 ? opts.maxBytes : DEFAULT_MAX_BYTES,
    };

    /* ---- discovery: bytes=0-262143 (design §3 R0) ---- */
    let d = await rangedGet(url, 0, DISCOVERY_BYTES - 1, null, cfg);
    let size, etag = strongETag(d.res);
    if (d.cr.total != null) {
      if (d.cr.total <= 0) throw MpakRangeError(ERR.RANGE, "empty object");
      size = d.cr.total;
    } else if (d.body.length < DISCOVERY_BYTES) {
      size = d.body.length;                    // '*': short body = whole object
    } else {
      /* '*': ambiguous full-length body — probe the size like mpakhttp */
      const p = await rangedGet(url, 0, 0, null, cfg);
      if (p.cr.total == null || p.cr.total <= 0)
        throw MpakRangeError(ERR.RANGE, "object size undeterminable");
      size = p.cr.total;
      d = await rangedGet(url, 0, DISCOVERY_BYTES - 1, null, cfg);
    }
    {
      const expectEnd = Math.min(DISCOVERY_BYTES - 1, size - 1);
      if (d.cr.end !== expectEnd || d.body.length !== expectEnd + 1)
        throw MpakRangeError(ERR.TRUNCATED,
                             "discovery: bytes " + d.cr.end + ", " +
                             d.body.length + " bytes");
    }

    /* ---- W1: bound the container-sized allocation BEFORE allocating —
       a lying Content-Range total must never drive memory sizing ---- */
    if (!Number.isSafeInteger(size) || size > cfg.maxBytes)
      throw MpakRangeError(ERR.SIZE,
                           "total " + size + " exceeds maxBytes " +
                           cfg.maxBytes);

    /* ---- remaining 64 KiB blocks, each fully validated ---- */
    let buffer;
    try {
      buffer = new Uint8Array(size);
    } catch (e) {
      throw MpakRangeError(ERR.SIZE, "allocation failed for " + size +
                                       " bytes");
    }
    buffer.set(d.body.subarray(0, Math.min(size, DISCOVERY_BYTES)), 0);
    const ifTag = cfg.ifRange ? etag : null;
    for (let base = DISCOVERY_BYTES; base < size; base += BLOCK) {
      const end = Math.min(base + BLOCK - 1, size - 1);
      const b = await rangedGet(url, base, end, ifTag, cfg);
      validateRange(b.cr, b.body, base, end, size, "block " + base);
      buffer.set(b.body, base);
    }

    /* ---- synchronous serve phase ---- */
    let destroyed = false;
    let installedModule = null;   /* one-active-source-per-Module */
    function checked(offset, len) {
      if (destroyed) throw MpakRangeError(ERR.CLOSED, "source destroyed");
      if (!Number.isSafeInteger(offset) || offset < 0 ||
          !Number.isSafeInteger(len) || len < 0 ||
          offset + len > size)
        throw MpakRangeError(ERR.RANGE, "read out of bounds");
      return buffer.subarray(offset, offset + len);
    }
    const api = {
      url,
      size,
      etag,
      blocks: Math.ceil(size / BLOCK),
      readBytes(offset, len) {
        return checked(offset, len);
      },
      /* installs the synchronous WASM imports (mpak_range_library.js).
         One active source per Module: a second install while this (or any)
         source is installed fails with ERR.INSTALLED — the shared imports
         would otherwise silently redirect another package's reads. Reads
         copy exactly `len` bytes into WASM memory; the C side treats
         anything but a full fill as a failed read. */
      install(module) {
        if (destroyed) throw MpakRangeError(ERR.CLOSED, "source destroyed");
        if (module.__mpakRangeSource &&
            module.__mpakRangeSource !== api)
          throw MpakRangeError(ERR.INSTALLED,
                               "another range source is installed on " +
                               "this Module");
        installedModule = module;
        module.__mpakRangeSource = api;
        module.mpakRangeSize = () => size;
        module.mpakRangeRead = (offset, ptr, len) => {
          const bytes = checked(offset, len);
          module.HEAPU8.set(bytes, ptr);
          return len;
        };
        module.mpakRangeDestroy = () => api.destroy();
      },
      destroy() {
        destroyed = true;            /* drop the reference; no JS handles */
        if (installedModule) {
          if (installedModule.__mpakRangeSource === api)
            installedModule.__mpakRangeSource = null;
          installedModule = null;   /* release the installation slot */
        }
      },
    };
    return api;
  }

  return {
    ERR,
    MpakRangeError,
    parseContentRange,
    acquire,
    DISCOVERY_BYTES,
    BLOCK,
    DEFAULT_MAX_BYTES,
  };
});
