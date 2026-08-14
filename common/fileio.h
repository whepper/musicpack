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

  * Neither the name of the MusicPack Development Team nor the
  names of its contributors may be used to endorse or promote
  products derived from this software without specific prior
  written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Portable 64-bit file positioning.
 *
 * fseeko/ftello/off_t are POSIX-only and, on glibc, require feature-test
 * macros; MSVC has none of them. This header provides a single pair of
 * functions that work everywhere:
 *
 *   mpc_file_seek(FILE *, mpc_seek_t offset, int whence)
 *   mpc_file_tell(FILE *)
 *
 * They map to _fseeki64/_ftelli64 on Windows and fseeko/ftello elsewhere.
 */
#ifndef MPC_FILEIO_H
#define MPC_FILEIO_H

/*
 * 64-bit file positioning on POSIX requires off_t to be 64-bit wide, which
 * glibc only guarantees when _FILE_OFFSET_BITS=64 is in effect before any
 * system header is included. That define is applied project-wide via CMake
 * (see the top-level CMakeLists.txt); the guard here documents the contract
 * and ensures consistency for direct compiles.
 */
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
# define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#if !defined(_WIN32)
# include <sys/types.h>
#endif

#include <mpc/mpc_types.h>

#if defined(_WIN32)
# define mpc_file_seek(fp, offset, whence) _fseeki64((fp), (__int64) (offset), (whence))
# define mpc_file_tell(fp)                 ((mpc_seek_t) _ftelli64(fp))
#else
# define mpc_file_seek(fp, offset, whence) fseeko((fp), (off_t) (offset), (whence))
# define mpc_file_tell(fp)                 ((mpc_seek_t) ftello(fp))
#endif

#endif /* MPC_FILEIO_H */
