/*
  Copyright (c) 2005-2009, The Musepack Development Team
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the The Musepack Development Team nor the
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
/// \file internal.h
/// Definitions and structures used only internally by the libmpcdec.
#ifndef _MPCDEC_INTERNAL_H_
#define _MPCDEC_INTERNAL_H_
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <mpc/mpcdec.h>

typedef struct mpc_block_t {
	char key[2];	// block key
	mpc_uint64_t size;	// block size minus the block header size
} mpc_block;

#define MAX_FRAME_SIZE 4352
#define DEMUX_BUFFER_SIZE (65536 - MAX_FRAME_SIZE) // need some space as sand box

struct mpc_demux_t {
	mpc_reader * r;
	mpc_decoder * d;
	mpc_streaminfo si;

	// buffer
	mpc_uint8_t buffer[DEMUX_BUFFER_SIZE + MAX_FRAME_SIZE];
	mpc_size_t bytes_total;
	mpc_bits_reader bits_reader;
	mpc_bool_t read_error;
	mpc_int32_t block_bits; /// bits remaining in current audio block
	mpc_uint_t block_frames; /// frames remaining in current audio block

	// seeking
	mpc_seek_t * seek_table;
	mpc_uint_t seek_pwr; /// distance between 2 frames in seek_table = 2^seek_pwr
	mpc_uint32_t seek_table_size; /// used size in seek_table
	mpc_uint32_t seek_table_capacity; /// allocated entries in seek_table
	mpc_bool_t reader_sync_lost; /// failed seek could not restore reader position

	// chapters
	mpc_seek_t chap_pos; /// supposed position of the first chapter block
	mpc_int_t chap_nb; /// number of chapters (-1 if unknown, 0 if no chapter)
	mpc_chap_info * chap; /// chapters position and tag

};

/**
 * checks if a block key is valid
 * @param key the two caracters key to check
 * @return MPC_STATUS_FAIL if the key is invalid, MPC_STATUS_OK else
 */
static mpc_inline mpc_status mpc_check_key(char * key)
{
	if (key[0] < 65 || key[0] > 90 || key[1] < 65 || key[1] > 90)
		return MPC_STATUS_FAIL;
	return MPC_STATUS_OK;
}

/// helper functions used by multiple files
typedef struct musepack_decoder musepack_decoder;
mpc_uint32_t mpc_random_int(mpc_decoder *d); // in synth_filter.c
void mpc_decoder_setup(mpc_decoder *d);
void mpc_decoder_init_quant(mpc_decoder *d, double scale_factor);

// Synthesis filter entry points. mpc_decoder_synthese_filter_float is the
// per-frame dispatcher (mpc_decoder.c); the *_scalar/*_simd implementations
// are the actual kernels (synth_filter.c / synth_filter_simd.c).
void mpc_decoder_synthese_filter_float(mpc_decoder *d, MPC_SAMPLE_FORMAT* OutData, mpc_int_t channels);
void mpc_synthese_filter_float_scalar(mpc_decoder *d, MPC_SAMPLE_FORMAT* OutData, mpc_int_t channels);
#ifdef MPC_ENABLE_SIMD_KERNEL
void mpc_synthese_filter_float_simd(mpc_decoder *d, MPC_SAMPLE_FORMAT* OutData, mpc_int_t channels);
#endif

// Synthesis implementation selection (white-box; used by the bench and the
// scalar-vs-SIMD differential test).
enum {
    MPC_SYNTH_AUTO   = 0, ///< best available (default)
    MPC_SYNTH_SCALAR = 1, ///< force the scalar reference path
    MPC_SYNTH_SIMD   = 2, ///< force the compiled SIMD path
};
// Returns nonzero only when the requested implementation is available.
int mpc_decoder_set_synth_impl(mpc_decoder *d, int impl);
int mpc_decoder_has_synth_simd(void);
mpc_decoder *musepack_decoder_internal(musepack_decoder *d);

#define MPC_IS_FAILURE(X) ((int)(X) < (int)MPC_STATUS_OK)
#define MPC_AUTO_FAIL(X) { mpc_status s = (X); if (MPC_IS_FAILURE(s)) return s; }


#ifdef __cplusplus
}
#endif
#endif
