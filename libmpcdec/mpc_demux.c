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

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <mpc/streaminfo.h>
#include <mpc/mpcdec.h>
#include <mpc/minimax.h>
#include "internal.h"
#include "decoder.h"
#include "huffman.h"
#include "mpc_bits_reader.h"

/// maximum number of seek points in the table. The distance between points will
/// be adapted so this value is never exceeded.
#define MAX_SEEK_TABLE_SIZE	65536

// streaminfo.c
mpc_status streaminfo_read_header_sv8(mpc_streaminfo* si,
									  const mpc_bits_reader * r_in,
									  mpc_size_t block_size);
mpc_status streaminfo_read_header_sv7(mpc_streaminfo* si, mpc_bits_reader * r_in);
void  streaminfo_encoder_info(mpc_streaminfo* si, const mpc_bits_reader * r_in);
void  streaminfo_gain(mpc_streaminfo* si, const mpc_bits_reader * r_in);

// mpc_decoder.c
void mpc_decoder_reset_scf(mpc_decoder * d, int value);

enum {
	MPC_BUFFER_SWAP = 1,
	MPC_BUFFER_FULL = 2,
};

static mpc_bool_t
mpc_u64_lshift(mpc_uint64_t value, mpc_uint_t shift, mpc_uint64_t *result)
{
	if (shift >= 64 || value > UINT64_MAX >> shift)
		return MPC_FALSE;
	*result = value << shift;
	return MPC_TRUE;
}

static mpc_status
mpc_seek_spacing(const mpc_demux *d, mpc_uint64_t *spacing)
{
	return mpc_u64_lshift((mpc_uint64_t) MPC_FRAME_LENGTH, d->seek_pwr,
	                     spacing) ? MPC_STATUS_OK : MPC_STATUS_FAIL;
}

static mpc_status
mpc_seek_table_entries(const mpc_demux *d, mpc_uint64_t *entries)
{
	mpc_uint64_t spacing;
	MPC_AUTO_FAIL(mpc_seek_spacing(d, &spacing));
	*entries = 2 + d->si.samples / spacing;
	return MPC_STATUS_OK;
}

static mpc_bool_t
mpc_seek_table_due(const mpc_demux *d)
{
	mpc_uint64_t spacing;
	return d->seek_table_size < d->seek_table_capacity &&
	       mpc_seek_spacing(d, &spacing) == MPC_STATUS_OK &&
	       d->seek_table_size <= UINT64_MAX / spacing &&
	       d->d->decoded_samples == (mpc_uint64_t) d->seek_table_size * spacing;
}

static void
mpc_seek_table_append(mpc_demux *d, mpc_seek_t position)
{
	if (position != (mpc_seek_t) -1 && d->seek_table_size < d->seek_table_capacity)
		d->seek_table[d->seek_table_size++] = position;
}

static mpc_status
mpc_bit_position_add(mpc_seek_t position, mpc_uint64_t bytes,
	                 mpc_seek_t *result)
{
	if (bytes > (UINT64_MAX - position) >> 3)
		return MPC_STATUS_FAIL;
	*result = position + (bytes << 3);
	return MPC_STATUS_OK;
}

static void mpc_demux_clear_buff(mpc_demux * d)
{
	d->bytes_total = 0;
	d->bits_reader.buff = d->buffer;
	d->bits_reader.count = 8;
	d->read_error = MPC_FALSE;
	d->block_bits = 0;
	d->block_frames = 0;
}

// Returns the amount of unread bytes in the demux buffer.
// Unchecked version - may return a negative value when we've been reading
// past the end of the valid data as a result of some problem with the file.
static mpc_int32_t mpc_unread_bytes_unchecked(mpc_demux * d) {
	return d->bytes_total + d->buffer - d->bits_reader.buff - ((8 - d->bits_reader.count) >> 3);
}



// Returns the number of bytes available in the buffer.
static mpc_uint32_t
mpc_demux_fill(mpc_demux * d, mpc_uint32_t min_bytes, int flags)
{
	mpc_uint32_t unread_bytes = (mpc_uint32_t) mpc_unread_bytes_unchecked(d);
	mpc_uint32_t target, bytes2read, bytesread = 0, write_start;
	int offset = 0;

	if ((mpc_int32_t)
		unread_bytes < 0) return 0; // Error - we've been reading past the end of the buffer - abort

	target = min_bytes == 0 || min_bytes > DEMUX_BUFFER_SIZE ?
	         DEMUX_BUFFER_SIZE : min_bytes;
	if ((flags & MPC_BUFFER_FULL) != 0 && unread_bytes < target &&
	    d->r->canseek(d->r))
		target = DEMUX_BUFFER_SIZE;
	if (unread_bytes >= target)
		return unread_bytes;

	bytes2read = target - unread_bytes;
	if (flags & MPC_BUFFER_SWAP) {
		bytes2read &= ~3u;
		offset = (unread_bytes + 3) & ~3u;
		offset -= unread_bytes;
	}
	if (bytes2read == 0)
		return unread_bytes;
	if (bytes2read + (mpc_uint32_t) offset > DEMUX_BUFFER_SIZE - d->bytes_total) {
		if (d->bits_reader.count == 0) {
			d->bits_reader.count = 8;
			d->bits_reader.buff++;
		}
		memmove(d->buffer + offset, d->bits_reader.buff, unread_bytes);
		d->bits_reader.buff = d->buffer + offset;
		d->bytes_total = unread_bytes + offset;
	}
	write_start = d->bytes_total;
	while (bytesread < bytes2read) {
		mpc_uint32_t request = bytes2read - bytesread;
		mpc_int32_t read_result = d->r->read(d->r,
				d->buffer + d->bytes_total, (mpc_int32_t) request);
		if (read_result < 0 || (mpc_uint32_t) read_result > request) {
			d->read_error = MPC_TRUE;
			break;
		}
		if (read_result == 0)
			break;
		d->bytes_total += (mpc_uint32_t) read_result;
		unread_bytes += (mpc_uint32_t) read_result;
		bytesread += (mpc_uint32_t) read_result;
	}
	memset(d->buffer + d->bytes_total, 0, bytes2read - bytesread);
	if (flags & MPC_BUFFER_SWAP) {
		unsigned char *tmp = d->buffer + write_start;
		mpc_uint32_t i;
		/* write_start is word-aligned after compaction; swap complete words,
		   including zero padding after a short final read. */
		for (i = 0; i < (bytes2read >> 2); i++, tmp += 4) {
			unsigned char b0 = tmp[0], b1 = tmp[1], b2 = tmp[2];
			tmp[0] = tmp[3]; tmp[1] = b2; tmp[2] = b1; tmp[3] = b0;
		}
	}

	return unread_bytes;
}

/**
 * seek to a bit position in the stream
 * @param d demuxer context
 * @param fpos position in the stream in bits from the beginning of mpc datas
 * @param min_bytes number of bytes to load after seeking
 */
static mpc_status
mpc_demux_seek(mpc_demux * d, mpc_seek_t fpos, mpc_uint32_t min_bytes) {
	mpc_seek_t start_pos, end_pos;
	mpc_seek_t tell_pos;
	mpc_uint32_t required;
	mpc_int_t bit_offset;

	// get current buffer position
	tell_pos = d->r->tell(d->r);
	if (tell_pos == (mpc_seek_t) -1 || !mpc_u64_lshift(tell_pos, 3, &end_pos) ||
	    !mpc_u64_lshift(d->bytes_total, 3, &start_pos) || start_pos > end_pos)
		return MPC_STATUS_FAIL;
	start_pos = end_pos - start_pos;

	if (fpos >= start_pos && fpos < end_pos) {
		d->bits_reader.buff = d->buffer + ((fpos - start_pos) >> 3);
		bit_offset = fpos & 7;
		d->block_bits = 0;
		d->block_frames = 0;
	} else {
		mpc_seek_t next_pos = fpos >> 3;
		if (!d->r->canseek(d->r))
			return MPC_STATUS_FAIL;
		if (d->si.stream_version == 7) {
			if (next_pos < (mpc_seek_t) d->si.header_position)
				return MPC_STATUS_FAIL;
			next_pos = ((next_pos - d->si.header_position) & ~3u) + d->si.header_position;
		}
		bit_offset = (int) (fpos - (next_pos << 3));

		if (!d->r->seek(d->r, next_pos))
			return MPC_STATUS_FAIL;
		mpc_demux_clear_buff(d);
	}

	if (min_bytes > DEMUX_BUFFER_SIZE - 4)
		return MPC_STATUS_FAIL;
	required = min_bytes + (mpc_uint32_t) ((bit_offset + 7) >> 3);
	d->read_error = MPC_FALSE;
	if (d->si.stream_version == 7) {
		required = (required + 3) & ~3u;
		mpc_demux_fill(d, required, MPC_BUFFER_SWAP);
	} else {
		mpc_demux_fill(d, required, 0);
	}
	if (d->read_error)
		return MPC_STATUS_FAIL;
	d->bits_reader.buff += bit_offset >> 3;
	d->bits_reader.count = 8 - (bit_offset & 7);

	return MPC_STATUS_OK;
}

/**
 * return the current position in the stream (in bits) from the beginning
 * of the file
 * @param d demuxer context
 * @return current stream position in bits
 */
mpc_seek_t mpc_demux_pos(mpc_demux * d)
{
	mpc_seek_t pos = d->r->tell(d->r);
	mpc_seek_t byte_pos;
	if (pos == (mpc_seek_t) -1 || pos < d->bytes_total)
		return (mpc_seek_t) -1;
	byte_pos = pos - d->bytes_total + (mpc_seek_t) (d->bits_reader.buff - d->buffer);
	if (byte_pos > (UINT64_MAX - 8) >> 3)
		return (mpc_seek_t) -1;
	return (byte_pos << 3) + 8 - d->bits_reader.count;
}

static mpc_status
mpc_demux_forward_to(mpc_demux *d, mpc_seek_t target)
{
	mpc_seek_t current = d->r->tell(d->r);
	mpc_uint8_t discard[4096];

	if (current == (mpc_seek_t) -1 || current > target)
		return MPC_STATUS_FAIL;
	mpc_demux_clear_buff(d);
	while (current < target) {
		mpc_seek_t left = target - current;
		mpc_int32_t request = left < sizeof discard ? (mpc_int32_t) left :
		                                                  (mpc_int32_t) sizeof discard;
		mpc_int32_t n = d->r->read(d->r, discard, request);
		if (n <= 0 || n > request)
			return MPC_STATUS_FAIL;
		current += (mpc_uint32_t) n;
	}
	return MPC_STATUS_OK;
}

/**
 * Searches for a ID3v2-tag and reads the length (in bytes) of it.
 *
 * @param d demuxer context
 * @return size of tag, in bytes
 * @return MPC_STATUS_FAIL on errors of any kind
 */
static mpc_seek_t mpc_demux_skip_id3v2(mpc_demux * d)
{
	mpc_uint8_t  tmp [4];
	mpc_bool_t footerPresent;     // ID3v2.4-flag
	mpc_seek_t size;

    // we must be at the beginning of the stream
	mpc_demux_fill(d, 3, 0);

    // check id3-tag
	if ( 0 != memcmp( d->bits_reader.buff, "ID3", 3 ) )
		return 0;

	mpc_demux_fill(d, 10, 0);

	mpc_bits_read(&d->bits_reader, 24); // read ID3
	mpc_bits_read(&d->bits_reader, 16); // read tag version

	tmp[0] = mpc_bits_read(&d->bits_reader, 8); // read flags
	footerPresent = tmp[0] & 0x10;
	if ( tmp[0] & 0x0F )
		return MPC_STATUS_FAIL; // not (yet???) allowed

	tmp[0] = mpc_bits_read(&d->bits_reader, 8); // read size
	tmp[1] = mpc_bits_read(&d->bits_reader, 8); // read size
	tmp[2] = mpc_bits_read(&d->bits_reader, 8); // read size
	tmp[3] = mpc_bits_read(&d->bits_reader, 8); // read size

	if ( (tmp[0] | tmp[1] | tmp[2] | tmp[3]) & 0x80 )
		return MPC_STATUS_FAIL; // not allowed

    // read headerSize (syncsave: 4 * $0xxxxxxx = 28 significant bits)
	size = tmp[0] << 21;
	size |= tmp[1] << 14;
	size |= tmp[2] << 7;
	size |= tmp[3];

	size += 10; //header

	if ( footerPresent ) size += 10;

	// This is called before file headers get read, streamversion etc isn't yet known, demuxing isn't properly initialized and we can't call mpc_demux_seek() from here.
	if (d->r->canseek(d->r)) {
		if (!d->r->seek(d->r, size))
			return MPC_STATUS_FAIL;
		mpc_demux_clear_buff(d);
	} else {
		MPC_AUTO_FAIL(mpc_demux_forward_to(d, size));
	}

	return size;
}

static mpc_status mpc_demux_seek_init(mpc_demux * d)
{
	mpc_uint64_t seek_table_size;
	if (d->seek_table != 0)
		return MPC_STATUS_OK;

	d->seek_pwr = 6;
	if (d->si.block_pwr > d->seek_pwr)
		d->seek_pwr = d->si.block_pwr;
	MPC_AUTO_FAIL(mpc_seek_table_entries(d, &seek_table_size));
	while (seek_table_size > MAX_SEEK_TABLE_SIZE) {
		if (d->seek_pwr >= 63)
			return MPC_STATUS_FAIL;
		d->seek_pwr++;
		MPC_AUTO_FAIL(mpc_seek_table_entries(d, &seek_table_size));
	}
	d->seek_table = malloc((size_t)(seek_table_size * sizeof(mpc_seek_t)));
	if (d->seek_table == 0)
		return MPC_STATUS_FAIL;
	d->seek_table[0] = (mpc_seek_t)mpc_demux_pos(d);
	if (d->seek_table[0] == (mpc_seek_t) -1) {
		free(d->seek_table);
		d->seek_table = 0;
		return MPC_STATUS_FAIL;
	}
	d->seek_table_size = 1;
	d->seek_table_capacity = (mpc_uint32_t) seek_table_size;

	return MPC_STATUS_OK;
}

typedef struct mpc_st_reader_t {
	const mpc_uint8_t *data;
	mpc_uint64_t bit_pos;
	mpc_uint64_t bit_size;
} mpc_st_reader;

static mpc_status
mpc_st_read(mpc_st_reader *r, mpc_uint_t bits, mpc_uint32_t *value)
{
	mpc_uint32_t result = 0;
	mpc_uint_t i;
	if (bits > 32 || r->bit_pos > r->bit_size || bits > r->bit_size - r->bit_pos)
		return MPC_STATUS_FAIL;
	for (i = 0; i < bits; i++, r->bit_pos++)
		result = (result << 1) |
		         ((r->data[r->bit_pos >> 3] >> (7 - (r->bit_pos & 7))) & 1u);
	*value = result;
	return MPC_STATUS_OK;
}

static mpc_status
mpc_st_read_size(mpc_st_reader *r, mpc_uint64_t *value)
{
	mpc_uint64_t result = 0;
	mpc_uint_t count;
	for (count = 0; count < 10; count++) {
		mpc_uint32_t byte;
		MPC_AUTO_FAIL(mpc_st_read(r, 8, &byte));
		if (result > (UINT64_MAX >> 7))
			return MPC_STATUS_FAIL;
		result = (result << 7) | (byte & 0x7Fu);
		if ((byte & 0x80u) == 0) {
			*value = result;
			return MPC_STATUS_OK;
		}
	}
	return MPC_STATUS_FAIL;
}

static mpc_status
mpc_st_read_golomb(mpc_st_reader *r, mpc_uint_t k, mpc_int32_t *value)
{
	mpc_uint32_t bit, suffix;
	mpc_uint64_t prefix = 0;
	do {
		MPC_AUTO_FAIL(mpc_st_read(r, 1, &bit));
		if (bit == 0 && ++prefix > ((mpc_uint64_t) INT32_MAX >> k))
			return MPC_STATUS_FAIL;
	} while (bit == 0);
	MPC_AUTO_FAIL(mpc_st_read(r, k, &suffix));
	*value = (mpc_int32_t) ((prefix << k) | suffix);
	return MPC_STATUS_OK;
}

static mpc_status mpc_demux_ST(mpc_demux * d, mpc_uint64_t block_size)
{
	mpc_uint64_t tmp;
	mpc_seek_t * table, last[2];
	mpc_st_reader r;
	mpc_uint_t i, diff_pwr = 0, mask;
	mpc_uint32_t file_table_size;
	mpc_uint32_t value;

	if (d->seek_table != 0)
		return MPC_STATUS_OK;
	if (d->bits_reader.count != 0 || block_size > UINT64_MAX / 8)
		return MPC_STATUS_FAIL;
	r.data = d->bits_reader.buff + 1;
	r.bit_pos = 0;
	r.bit_size = block_size * 8;

	MPC_AUTO_FAIL(mpc_st_read_size(&r, &tmp));
	if (tmp == 0 || tmp > UINT32_MAX)
		return MPC_STATUS_FAIL;
	file_table_size = (mpc_uint32_t) tmp;
	MPC_AUTO_FAIL(mpc_st_read(&r, 4, &value));
	d->seek_pwr = d->si.block_pwr + value;

	MPC_AUTO_FAIL(mpc_seek_table_entries(d, &tmp));
	while (tmp > MAX_SEEK_TABLE_SIZE) {
		if (d->seek_pwr >= 63 || diff_pwr >= 31)
			return MPC_STATUS_FAIL;
		d->seek_pwr++;
		diff_pwr++;
		MPC_AUTO_FAIL(mpc_seek_table_entries(d, &tmp));
	}
	if ((file_table_size >> diff_pwr) > tmp)
		file_table_size = (mpc_uint32_t) (tmp << diff_pwr);
	d->seek_table = malloc((size_t) (tmp * sizeof(mpc_seek_t)));
	if (d->seek_table == 0)
		return MPC_STATUS_FAIL;
	d->seek_table_capacity = (mpc_uint32_t) tmp;
	d->seek_table_size = (file_table_size + ((1u << diff_pwr) - 1)) >> diff_pwr;
	if (d->seek_table_size == 0 || d->seek_table_size > d->seek_table_capacity)
		return MPC_STATUS_FAIL;

	table = d->seek_table;
	MPC_AUTO_FAIL(mpc_st_read_size(&r, &tmp));
	if (d->si.header_position < 0 || tmp > UINT64_MAX - (mpc_uint64_t) d->si.header_position ||
	    !mpc_u64_lshift(tmp + (mpc_uint64_t) d->si.header_position, 3, &last[0]))
		return MPC_STATUS_FAIL;
	table[0] = last[0];

	if (d->seek_table_size == 1)
		return MPC_STATUS_OK;

	MPC_AUTO_FAIL(mpc_st_read_size(&r, &tmp));
	if (tmp > UINT64_MAX - (mpc_uint64_t) d->si.header_position ||
	    !mpc_u64_lshift(tmp + (mpc_uint64_t) d->si.header_position, 3, &last[1]))
		return MPC_STATUS_FAIL;
	if (diff_pwr == 0) table[1] = last[1];

	mask = (1u << diff_pwr) - 1;
	for (i = 2; i < file_table_size; i++) {
		mpc_int32_t code;
		int64_t delta;
		mpc_uint64_t value;
		MPC_AUTO_FAIL(mpc_st_read_golomb(&r, 12, &code));
		if (code & 1)
			delta = -(int64_t) (code & ~1u);
		else
			delta = code;
		delta *= 4;
		if (last[(i-1) & 1] > UINT64_MAX / 2)
			return MPC_STATUS_FAIL;
		value = 2 * last[(i-1) & 1];
		if (value < last[i & 1])
			return MPC_STATUS_FAIL;
		value -= last[i & 1];
		if ((delta < 0 && value < (mpc_uint64_t) -delta) ||
		    (delta >= 0 && value > UINT64_MAX - (mpc_uint64_t) delta))
			return MPC_STATUS_FAIL;
		last[i & 1] = delta < 0 ? value - (mpc_uint64_t) -delta :
		                                  value + (mpc_uint64_t) delta;
		if ((i & mask) == 0)
			table[i >> diff_pwr] = last[i & 1];
	}
	return MPC_STATUS_OK;
}

static mpc_status mpc_demux_SP(mpc_demux * d, int size, int block_size)
{
	mpc_seek_t cur;
	mpc_uint64_t ptr;
	mpc_block b;
	int st_head_size;

	cur = mpc_demux_pos(d);
	if (cur == (mpc_seek_t) -1)
		return MPC_STATUS_FAIL;
	mpc_bits_get_size(&d->bits_reader, &ptr);
	if (ptr < (mpc_uint32_t) size || ptr - (mpc_uint32_t) size > (UINT64_MAX - cur) >> 3)
		return MPC_STATUS_FAIL;
	MPC_AUTO_FAIL( mpc_demux_seek(d, ((ptr - (mpc_uint32_t) size) << 3) + cur, 11) );
	st_head_size = mpc_bits_get_block(&d->bits_reader, &b);
	if (memcmp(b.key, "ST", 2) == 0) {
		mpc_uint64_t chap_offset = ptr - (mpc_uint32_t) size;
		if (b.size > DEMUX_BUFFER_SIZE)
			return MPC_STATUS_FAIL;
		if (b.size > UINT64_MAX - chap_offset - (mpc_uint32_t) st_head_size ||
		    chap_offset + b.size + (mpc_uint32_t) st_head_size > (UINT64_MAX - cur) >> 3)
			return MPC_STATUS_FAIL;
		d->chap_pos = ((chap_offset + b.size + (mpc_uint32_t) st_head_size) << 3) + cur;
		d->chap_nb = -1;
		if (mpc_demux_fill(d, (mpc_uint32_t) b.size, 0) < b.size)
			return MPC_STATUS_FAIL;
		MPC_AUTO_FAIL( mpc_demux_ST(d, b.size) );
	}
	return mpc_demux_seek(d, cur, 11 + block_size);
}

static void mpc_demux_chap_empty(mpc_demux * d) {
	free(d->chap); d->chap = 0;
	d->chap_nb = 0; // -1 for undefined, 0 for no chapters
	d->chap_pos = 0;
}

static mpc_status mpc_demux_chap_find_inner(mpc_demux * d)
{
	mpc_block b;
	mpc_uint64_t tag_size = 0, chap_size = 0;
	int size, i = 0;

	d->chap_nb = 0;

	if (d->si.stream_version < 8)
		return MPC_STATUS_OK;

	if (d->chap_pos == 0) {
		mpc_uint64_t cur_pos;
		if (d->si.header_position < 0)
			return MPC_STATUS_FAIL;
		MPC_AUTO_FAIL(mpc_u64_lshift((mpc_uint64_t) d->si.header_position + 4,
		                             3, &cur_pos) ? MPC_STATUS_OK : MPC_STATUS_FAIL);
		MPC_AUTO_FAIL( mpc_demux_seek(d, cur_pos, 11) ); // seek to the beginning of the stream
		size = mpc_bits_get_block(&d->bits_reader, &b);
		while (memcmp(b.key, "SE", 2) != 0) {
			mpc_uint64_t new_pos;
			MPC_AUTO_FAIL(mpc_check_key(b.key));
			if (size < 0 || b.size > UINT64_MAX - (mpc_uint32_t) size)
				return MPC_STATUS_FAIL;
			MPC_AUTO_FAIL(mpc_bit_position_add(cur_pos,
			             b.size + (mpc_uint32_t) size, &new_pos));

			if (memcmp(b.key, "CT", 2) == 0) {
				if (d->chap_pos == 0) d->chap_pos = cur_pos;
			} else {
				d->chap_pos = 0;
			}
			if (new_pos <= cur_pos)
				return MPC_STATUS_FAIL;
			cur_pos = new_pos;
			
			MPC_AUTO_FAIL( mpc_demux_seek(d, cur_pos, 11) );
			size = mpc_bits_get_block(&d->bits_reader, &b);
		}
		if (d->chap_pos == 0)
			d->chap_pos = cur_pos;
	}

	MPC_AUTO_FAIL(mpc_demux_seek(d, d->chap_pos, 20));
	size = mpc_bits_get_block(&d->bits_reader, &b);
	while (memcmp(b.key, "CT", 2) == 0) {
		mpc_uint64_t chap_sample;
		mpc_seek_t next_pos;
		if (d->chap_nb == INT_MAX || size < 0 ||
		    chap_size > UINT64_MAX - (mpc_uint32_t) size)
			return MPC_STATUS_FAIL;
		d->chap_nb++;
		chap_size += (mpc_uint32_t) size;
		size = mpc_bits_get_size(&d->bits_reader, &chap_sample) + 4;
		if (chap_size > UINT64_MAX - (mpc_uint32_t) size || b.size < (mpc_uint32_t) size ||
		    tag_size > UINT64_MAX - (b.size - (mpc_uint32_t) size))
			return MPC_STATUS_FAIL;
		chap_size += (mpc_uint32_t) size;
		tag_size += b.size - (mpc_uint32_t) size;
		if (chap_size > UINT64_MAX - tag_size)
			return MPC_STATUS_FAIL;
		MPC_AUTO_FAIL(mpc_bit_position_add(d->chap_pos, chap_size + tag_size,
		                                    &next_pos));
		MPC_AUTO_FAIL( mpc_demux_seek(d, next_pos, 20) );
		size = mpc_bits_get_block(&d->bits_reader, &b);
	}

	if (d->chap_nb > 0) {
		char * ptag;
		if (tag_size > SIZE_MAX ||
		    (size_t) d->chap_nb > (SIZE_MAX - (size_t) tag_size) / sizeof(mpc_chap_info))
			return MPC_STATUS_FAIL;
		d->chap = malloc(sizeof(mpc_chap_info) * (size_t) d->chap_nb + (size_t) tag_size);
		if (d->chap == 0)
			return MPC_STATUS_FAIL;

		ptag = (char*)(d->chap + d->chap_nb);

		MPC_AUTO_FAIL( mpc_demux_seek(d, d->chap_pos, 11) );
		size = mpc_bits_get_block(&d->bits_reader, &b);
		while (memcmp(b.key, "CT", 2) == 0) {
			mpc_uint_t tmp_size;
			char * tmp_ptag = ptag;
			if (mpc_demux_fill(d, 11 + (mpc_uint32_t) b.size, 0) < b.size)
				return MPC_STATUS_FAIL;
			size = mpc_bits_get_size(&d->bits_reader, &d->chap[i].sample) + 4;
			d->chap[i].gain = (mpc_uint16_t) mpc_bits_read(&d->bits_reader, 16);
			d->chap[i].peak = (mpc_uint16_t) mpc_bits_read(&d->bits_reader, 16);

			tmp_size = b.size - size;
			do {
				mpc_uint_t rd_size = tmp_size;
				mpc_uint8_t * tmp_buff = d->bits_reader.buff + ((8 - d->bits_reader.count) >> 3);
				mpc_uint32_t avail_bytes = d->bytes_total + d->buffer - tmp_buff;
				rd_size = mini(rd_size, avail_bytes);
				memcpy(tmp_ptag, tmp_buff, rd_size);
				tmp_size -= rd_size;
				tmp_ptag += rd_size;
				d->bits_reader.buff += rd_size;
				mpc_demux_fill(d, tmp_size, 0);
			} while (tmp_size > 0);

			d->chap[i].tag_size = b.size - size;
			d->chap[i].tag = ptag;
			ptag += b.size - size;
			i++;
			size = mpc_bits_get_block(&d->bits_reader, &b);
		}
	}

	d->bits_reader.buff -= size;
	return MPC_STATUS_OK;
}

static mpc_status mpc_demux_chap_find(mpc_demux * d) {
	mpc_status s = mpc_demux_chap_find_inner(d);
	if (MPC_IS_FAILURE(s))
		mpc_demux_chap_empty(d);
	return s;
}

/**
 * Gets the number of chapters in the stream
 * @param d pointer to a musepack demuxer
 * @return the number of chapters found in the stream
 */
mpc_int_t mpc_demux_chap_nb(mpc_demux * d)
{
	if (d->chap_nb == -1)
		mpc_demux_chap_find(d);
	return d->chap_nb;
}

/**
 * Gets datas associated to a given chapter
 * The chapter tag is an APEv2 tag without the preamble
 * @param d pointer to a musepack demuxer
 * @param chap_nb chapter number you want datas (from 0 to mpc_demux_chap_nb(d) - 1)
 * @return the chapter information structure
 */
mpc_chap_info const * mpc_demux_chap(mpc_demux * d, int chap_nb)
{
	if (d->chap_nb == -1)
		mpc_demux_chap_find(d);
	if (chap_nb >= d->chap_nb || chap_nb < 0)
		return 0;
	return &d->chap[chap_nb];
}

static mpc_status mpc_demux_header(mpc_demux * d)
{
	char magic[4];

	d->si.pns = 0xFF;
	d->si.profile_name = "n.a.";

    // get header position
	d->si.header_position = mpc_demux_skip_id3v2(d);
	if(d->si.header_position < 0)
		return MPC_STATUS_FAIL;

	d->si.tag_offset = d->si.total_file_length = d->r->get_size(d->r);

	mpc_demux_fill(d, 4, 0);
	magic[0] = mpc_bits_read(&d->bits_reader, 8);
	magic[1] = mpc_bits_read(&d->bits_reader, 8);
	magic[2] = mpc_bits_read(&d->bits_reader, 8);
	magic[3] = mpc_bits_read(&d->bits_reader, 8);

	if (memcmp(magic, "MP+", 3) == 0) {
		d->si.stream_version = magic[3] & 15;
		d->si.pns = magic[3] >> 4;
		if (d->si.stream_version != 7)
			return MPC_STATUS_FAIL;
		if (mpc_demux_fill(d, 6 * 4, MPC_BUFFER_SWAP) < 6 * 4) // header block size + endian convertion
			return MPC_STATUS_FAIL;
		MPC_AUTO_FAIL( streaminfo_read_header_sv7(&d->si, &d->bits_reader) );
	} else if (memcmp(magic, "MPCK", 4) == 0) {
		mpc_block b;
		int size;
		mpc_demux_fill(d, 11, 0); // max header block size
		size = mpc_bits_get_block(&d->bits_reader, &b);
		while( memcmp(b.key, "AP", 2) != 0 ){ // scan all blocks until audio
			if (mpc_check_key(b.key) != MPC_STATUS_OK)
				return MPC_STATUS_FAIL;
			if (b.size > (mpc_uint64_t) DEMUX_BUFFER_SIZE - 11)
				return MPC_STATUS_FAIL;
			
			if (mpc_demux_fill(d, 11 + (mpc_uint32_t) b.size, 0) <= b.size) 
				return MPC_STATUS_FAIL;

			if (memcmp(b.key, "SH", 2) == 0) {
				MPC_AUTO_FAIL( streaminfo_read_header_sv8(&d->si, &d->bits_reader, (mpc_uint32_t) b.size) );
			} else if (memcmp(b.key, "RG", 2) == 0) {
				streaminfo_gain(&d->si, &d->bits_reader);
			} else if (memcmp(b.key, "EI", 2) == 0) {
				streaminfo_encoder_info(&d->si, &d->bits_reader);
			} else if (memcmp(b.key, "SO", 2) == 0) {
				if (d->r->canseek(d->r))
					MPC_AUTO_FAIL( mpc_demux_SP(d, size, (mpc_uint32_t) b.size) );
			} else if (memcmp(b.key, "ST", 2) == 0) {
				MPC_AUTO_FAIL( mpc_demux_ST(d, b.size) );
			}
			d->bits_reader.buff += b.size;
			size = mpc_bits_get_block(&d->bits_reader, &b);
		}
		d->bits_reader.buff -= size;
		if (d->si.stream_version == 0) // si not initialized !!!
			return MPC_STATUS_FAIL;
	} else {
		return MPC_STATUS_FAIL;
	}

	return MPC_STATUS_OK;
}

mpc_demux * mpc_demux_init(mpc_reader * p_reader)
{
	mpc_demux* p_tmp;

	if (p_reader == 0 || p_reader->read == 0 || p_reader->seek == 0 ||
	    p_reader->tell == 0 || p_reader->get_size == 0 || p_reader->canseek == 0)
		return 0;
	p_tmp = malloc(sizeof(mpc_demux));

	if (p_tmp != 0) {
		memset(p_tmp, 0, sizeof(mpc_demux));
		p_tmp->r = p_reader;
		p_tmp->chap_nb = -1;
		mpc_demux_clear_buff(p_tmp);
		if (mpc_demux_header(p_tmp) == MPC_STATUS_OK &&
				  mpc_demux_seek_init(p_tmp) == MPC_STATUS_OK) {
			p_tmp->d = mpc_decoder_init(&p_tmp->si);
		} else {
			if (p_tmp->seek_table)
				free(p_tmp->seek_table);
			free(p_tmp);
			p_tmp = 0;
		}
	}

	return p_tmp;
}

void mpc_demux_exit(mpc_demux * d)
{
	mpc_decoder_exit(d->d);
	free(d->seek_table);
	free(d->chap);
	free(d);
}

void mpc_demux_get_info(mpc_demux * d, mpc_streaminfo * i)
{
	memcpy(i, &d->si, sizeof d->si);
}

void mpc_demux_set_samples_to_skip(mpc_demux * d, mpc_uint32_t samples)
{
	d->d->samples_to_skip = samples;
}

mpc_seek_t mpc_demux_chap_pos(mpc_demux * d)
{
	return d->chap_pos;
}

static mpc_status mpc_demux_decode_inner(mpc_demux * d, mpc_frame_info * i)
{
	mpc_bits_reader r;
	if (d->reader_sync_lost)
		return MPC_STATUS_FAIL;
	if (d->si.stream_version >= 8) {
		i->is_key_frame = MPC_FALSE;

		if (d->block_frames == 0) {
			mpc_block b = {{0,0},0};
			d->bits_reader.count &= -8;
			if (mpc_seek_table_due(d))
				mpc_seek_table_append(d, (mpc_seek_t) mpc_demux_pos(d));
			mpc_demux_fill(d, 11, MPC_BUFFER_FULL); // max header block size
			mpc_bits_get_block(&d->bits_reader, &b);
			while( memcmp(b.key, "AP", 2) != 0 ) { // scan all blocks until audio
				MPC_AUTO_FAIL( mpc_check_key(b.key) );

				if (memcmp(b.key, "SE", 2) == 0) { // end block
					i->bits = -1;
					return MPC_STATUS_OK;
				}

				if (b.size > DEMUX_BUFFER_SIZE - 11 ||
				    mpc_demux_fill(d, 11 + (mpc_uint32_t) b.size, MPC_BUFFER_FULL) < b.size)
					return MPC_STATUS_FAIL;

				d->bits_reader.buff += b.size;
				mpc_bits_get_block(&d->bits_reader, &b);
			}
			if (b.size > INT32_MAX / 8)
				return MPC_STATUS_FAIL;
			d->block_bits = (mpc_int32_t) (b.size * 8);
			if (d->si.block_pwr >= 32)
				return MPC_STATUS_FAIL;
			d->block_frames = 1u << d->si.block_pwr;
			i->is_key_frame = MPC_TRUE;
		}
		if (d->buffer + d->bytes_total - d->bits_reader.buff <= MAX_FRAME_SIZE)
			mpc_demux_fill(d, (d->block_bits >> 3) + 1, MPC_BUFFER_FULL);
		r = d->bits_reader;
		mpc_decoder_decode_frame(d->d, &d->bits_reader, i);
		d->block_bits -= ((d->bits_reader.buff - r.buff) << 3) + r.count - d->bits_reader.count;
		d->block_frames--;
		if (d->block_bits < 0 || (d->block_frames == 0 && d->block_bits > 7))
			return MPC_STATUS_FAIL;
	} else {
		if (mpc_seek_table_due(d))
			mpc_seek_table_append(d, (mpc_seek_t) mpc_demux_pos(d));
		mpc_demux_fill(d, MAX_FRAME_SIZE, MPC_BUFFER_FULL | MPC_BUFFER_SWAP);
		d->block_bits = (mpc_int_t) mpc_bits_read(&d->bits_reader, 20); // read frame size
		if (d->d->decoded_samples < d->d->samples &&
		    MPC_FRAME_LENGTH > d->d->samples - d->d->decoded_samples - 1)
			d->block_bits += 11; // we will read last frame size
		r = d->bits_reader;
		mpc_decoder_decode_frame(d->d, &d->bits_reader, i);
		if (i->bits != -1 && d->block_bits != ((d->bits_reader.buff - r.buff) << 3) + r.count - d->bits_reader.count)
			return MPC_STATUS_FAIL;
	}
	if (i->bits != -1 && d->buffer + d->bytes_total < d->bits_reader.buff + ((8 - d->bits_reader.count) >> 3))
		return MPC_STATUS_FAIL;

	return MPC_STATUS_OK;
}

mpc_status mpc_demux_decode(mpc_demux * d, mpc_frame_info * i) {
	mpc_status s = mpc_demux_decode_inner(d, i);
	if (MPC_IS_FAILURE(s))
		i->bits = -1; // we pretend it's end of file
	return s;
}

mpc_status mpc_demux_seek_second(mpc_demux * d, double seconds)
{
	double sample;
	if (!isfinite(seconds) || seconds < 0)
		return MPC_STATUS_FAIL;
	sample = seconds * (double) d->si.sample_freq;
	if (sample >= (double) d->si.samples)
		return mpc_demux_seek_sample(d, d->si.samples);
	return mpc_demux_seek_sample(d, (mpc_uint64_t)(sample + 0.5));
}

static mpc_status
mpc_demux_seek_sample_inner(mpc_demux * d, mpc_uint64_t destsample)
{
	mpc_uint64_t fwd, i, block_samples;
	mpc_uint32_t samples_to_skip;
	mpc_uint_t table_shift;
	mpc_seek_t fpos;

	if (!d->r->canseek(d->r) || d->seek_table_size == 0 ||
	    !mpc_u64_lshift((mpc_uint64_t) MPC_FRAME_LENGTH, d->si.block_pwr,
	                    &block_samples) || d->si.beg_silence > d->si.samples)
		return MPC_STATUS_FAIL;
	if (destsample > d->si.samples - d->si.beg_silence)
		destsample = d->si.samples;
	else
		destsample += d->si.beg_silence;
	fwd = destsample / block_samples;
	samples_to_skip = MPC_DECODER_SYNTH_DELAY +
			(mpc_uint32_t) (destsample % block_samples);
	if (d->si.stream_version == 7) {
		if (fwd > 32) {
			fwd -= 32;
			samples_to_skip += MPC_FRAME_LENGTH * 32;
		} else {
			samples_to_skip += MPC_FRAME_LENGTH * fwd;
			fwd = 0;
		}
	}

	if (d->seek_pwr < d->si.block_pwr)
		return MPC_STATUS_FAIL;
	table_shift = d->seek_pwr - d->si.block_pwr;
	i = fwd >> table_shift;
	if (i >= d->seek_table_size)
		i = d->seek_table_size - 1;
	fpos = d->seek_table[(mpc_uint32_t) i];
	if (!mpc_u64_lshift(i, table_shift, &i) || i > UINT64_MAX / block_samples)
		return MPC_STATUS_FAIL;

	if (d->si.stream_version >= 8) {
		mpc_block b;
		int size;
		MPC_AUTO_FAIL(mpc_demux_seek(d, fpos, 11));
		d->d->decoded_samples = i * block_samples;
		size = mpc_bits_get_block(&d->bits_reader, &b);
		while(i < fwd) {
			if (memcmp(b.key, "AP", 2) == 0) {
				if (mpc_seek_table_due(d))
					mpc_seek_table_append(d, (mpc_seek_t) mpc_demux_pos(d) - 8u * (mpc_uint32_t) size);
				if (d->d->decoded_samples > UINT64_MAX - block_samples)
					return MPC_STATUS_FAIL;
				d->d->decoded_samples += block_samples;
				i++;
			}
			if (b.size > (UINT64_MAX >> 3) - (mpc_uint32_t) size ||
			    fpos > UINT64_MAX - ((b.size + (mpc_uint32_t) size) << 3))
				return MPC_STATUS_FAIL;
			fpos += (b.size + (mpc_uint32_t) size) << 3;
			MPC_AUTO_FAIL(mpc_demux_seek(d, fpos, 11));
			size = mpc_bits_get_block(&d->bits_reader, &b);
		}
		d->bits_reader.buff -= size;
	} else {
		mpc_decoder_reset_scf(d->d, fwd != 0);
		MPC_AUTO_FAIL(mpc_demux_seek(d, fpos, 4));
		d->d->decoded_samples = i * block_samples;
		for( ; i < fwd; i++){
			mpc_uint64_t frame_bits;
			if (mpc_seek_table_due(d))
				mpc_seek_table_append(d, (mpc_seek_t) mpc_demux_pos(d));
			if (d->d->decoded_samples > UINT64_MAX - block_samples)
				return MPC_STATUS_FAIL;
			d->d->decoded_samples += block_samples;
			frame_bits = (mpc_uint64_t) mpc_bits_read(&d->bits_reader, 20) + 20;
			if (fpos > UINT64_MAX - frame_bits)
				return MPC_STATUS_FAIL;
			fpos += frame_bits;
			MPC_AUTO_FAIL(mpc_demux_seek(d, fpos, 4));
		}
	}
	d->d->samples_to_skip = samples_to_skip;
	return MPC_STATUS_OK;
}

mpc_status mpc_demux_seek_sample(mpc_demux * d, mpc_uint64_t destsample)
{
	typedef struct mpc_seek_transaction_t {
		mpc_demux demux;
		mpc_decoder decoder;
	} mpc_seek_transaction;
	mpc_seek_transaction *snapshot;
	mpc_seek_t reader_position;
	mpc_status status;

	if (d == 0 || d->d == 0 || !d->r->canseek(d->r))
		return MPC_STATUS_FAIL;
	snapshot = malloc(sizeof *snapshot);
	if (snapshot == 0)
		return MPC_STATUS_FAIL;
	reader_position = d->r->tell(d->r);
	if (reader_position == (mpc_seek_t) -1) {
		free(snapshot);
		return MPC_STATUS_FAIL;
	}
	if (d->reader_sync_lost)
		mpc_demux_clear_buff(d);
	/* Seeking traverses blocks and mutates decoder history. Restore both state
	   machines and the read-ahead position if any later operation fails. */
	snapshot->demux = *d;
	snapshot->decoder = *d->d;
	status = mpc_demux_seek_sample_inner(d, destsample);
	if (status == MPC_STATUS_OK) {
		d->reader_sync_lost = MPC_FALSE;
		free(snapshot);
		return MPC_STATUS_OK;
	}

	if (!d->r->seek(d->r, reader_position)) {
		*d = snapshot->demux;
		*d->d = snapshot->decoder;
		d->reader_sync_lost = MPC_TRUE;
		free(snapshot);
		return status;
	}
	*d = snapshot->demux;
	*d->d = snapshot->decoder;
	free(snapshot);
	return status;
}

void mpc_set_replay_level(mpc_demux * d, float level, mpc_bool_t use_gain,
						  mpc_bool_t use_title, mpc_bool_t clip_prevention)
{
	float peak = (float) ( use_title ? d->si.peak_title : d->si.peak_album );
	float gain = (float) ( use_title ? d->si.gain_title : d->si.gain_album );

	if(!use_gain && !clip_prevention)
		return;

	if(!peak)
		peak = 1.;
	else
		peak = (float) ( (1 << 15) / pow(10, peak / (20 * 256)) );

	if(!gain)
		gain = 1.;
	else
		gain = (float) pow(10, (level - gain / 256) / 20);

	if(clip_prevention && (peak < gain || !use_gain))
		gain = peak;

	mpc_decoder_scale_output(d->d, gain);
}
