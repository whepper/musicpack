// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

/*
 * JS imports backing the WASM MPAK range source (mpak_wasm_open_range).
 *
 * The host installs SYNCHRONOUS implementations on the module before calling
 * mpak_wasm_open_range (see demo/mpakrange.js — the acquire-then-serve
 * bridge: the complete container is downloaded and validated over HTTP
 * Range requests first, then served from memory):
 *
 *   Module.mpakRangeSize()                  -> container size in bytes
 *   Module.mpakRangeRead(off, ptr, len)     -> copy len bytes into HEAPU8
 *   Module.mpakRangeDestroy()               -> release the JS-side source
 *
 * These back a musicpack_range_source inside the WASM module, so
 * musicpack_package_open_range() runs unchanged: all reads are exact
 * absolute-offset requests issued by the MPAK block cache.
 */

var LibraryMpakRange = {
  mpak_range_size: function () {
    return Module.mpakRangeSize();
  },
  mpak_range_read: function (offset, ptr, len) {
    return Module.mpakRangeRead(offset, ptr, len);
  },
  mpak_range_destroy: function () {
    Module.mpakRangeDestroy();
  },
};

mergeInto(LibraryManager.library, LibraryMpakRange);
