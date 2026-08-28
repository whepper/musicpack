/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the MusicPack Development Team nor the names of
  its contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/// \file mpak.h
/// MPAK v1 single-file `.mpak` container (specs/mpak-v1.md).
///
/// MPAK is a *storage backend* for the same logical MusicPack model as the
/// directory bundle: the MANF block carries the exact canonical
/// `manifest.json` bytes and members are stored byte-exact. Opening a
/// `.mpak` file yields the same \c musicpack_package handle via
/// musicpack_package_open(); this header adds packing, unpacking and
/// recovery entry points.
///
/// Layout (writer order): fixed 16-byte header, INDX (derived
/// acceleration, recommended), MANF (exact manifest bytes), DATA blocks in
/// manifest canonical traversal order, optional TAIL (fixity/identity).
/// INDX is never a prerequisite: a sequential scan of DATA blocks
/// reconstructs all object paths, offsets and lengths.
#ifndef MUSICPACK_MPAK_H_
#define MUSICPACK_MPAK_H_
#pragma once

#include <musicpack/error.h>
#include <musicpack/export.h>
#include <musicpack/package.h>
#include <musicpack/range.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Container major version implemented by this library.
#define MUSICPACK_MPAK_MAJOR 1
/// Header flag bit 0: an INDX block is present (hint only).
#define MUSICPACK_MPAK_FLAG_INDX_PRESENT 0x0001u

/// Opens a `.mpak` container through a caller-provided byte-range
/// source (e.g. a remote HTTP Range adapter) and returns the same
/// `musicpack_package` handle as local opens: manifest access, member
/// lookup, verification, extra-file enumeration,
/// musicpack_package_track_open_reader() and
/// musicpack_package_read_member() all behave identically; only the
/// byte transport differs. Directory-only mutation APIs
/// (musicpack_package_save_manifest(), resolve_path) fail for
/// range-backed packages exactly as for local ones.
///
/// The container is read with the same hardened scanner used for local
/// files: framing is validated before lengths are trusted, INDX is
/// used only after reconciliation against the sequential DATA scan,
/// and INDX/TAIL remain optional. Normal (strict) reader semantics
/// apply — recovery-mode preamble tolerance is an unpack-only policy.
///
/// The source is read during opening. On success the package takes
/// ownership of `src->ctx` (`src->destroy` is called at
/// musicpack_package_close()). On failure the package does NOT destroy
/// the source — ownership stays with the caller, who must clean it up
/// (the source may have been read from during the attempt).
///
/// The library applies no transport policy: an adapter may cache,
/// read-ahead, or fetch eagerly behind the exact-read contract.
///
/// \param src    initialized range source (non-NULL `size` and `read`)
/// \param status optional error out
/// \return an owned handle, or NULL on failure
MUSICPACK_API musicpack_package *
musicpack_package_open_range(const musicpack_range_source *src,
                             musicpack_status *status);


/// Packs a directory-form `.mpack` package into a single-file `.mpak`.
///
/// The directory package is opened and verified first; any verification
/// error aborts the pack (warnings are allowed). The output is
/// deterministic: identical logical inputs produce byte-identical files.
/// MANF carries the directory's `manifest.json` bytes unchanged and every
/// manifest-referenced member is stored byte-exact, so
/// directory -> MPAK -> directory is a pure repack.
///
/// \param dir      package directory (e.g. "Album.mpack")
/// \param out_file output path (parent directory must exist)
/// \param status   optional error out
/// \return MUSICPACK_OK or an error
MUSICPACK_API musicpack_status musicpack_mpak_pack_dir(const char *dir,
                                                       const char *out_file,
                                                       musicpack_status *status);

/// Extracts (and recovers) a `.mpak` file into a directory-form package.
///
/// Extraction is physical: members are located by sequential scan (INDX
/// hashes are used to verify members when the index is intact), so it
/// works even when MANF is missing or corrupt and when INDX or TAIL are
/// damaged. The manifest is written from MANF bytes when present and
/// parseable-manifest availability is not required. Findings are reported
/// through \p rep / \p fn exactly like musicpack_package_verify().
///
/// \param in_file `.mpak` file
/// \param out_dir destination directory (created; must not exist)
/// \param rep     optional error/warning counts
/// \param report  optional per-finding callback
/// \param ctx     opaque context for \p report
/// \return MUSICPACK_OK when there are no errors (warnings allowed)
MUSICPACK_API musicpack_status musicpack_mpak_unpack(const char *in_file,
                                                     const char *out_dir,
                                                     musicpack_report *rep,
                                                     musicpack_report_fn report,
                                                     void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* MUSICPACK_MPAK_H_ */
