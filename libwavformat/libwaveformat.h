/*
  Copyright (c) 2005-2009, The Musepack Development Team
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

  * Neither the name of the Musepack Development Team nor the
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
#ifndef __LIBWAVEFORMAT_H__
#define __LIBWAVEFORMAT_H__
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//general declarations

#include <stdint.h>
typedef int8_t t_wav_int8;
typedef uint8_t t_wav_uint8;
typedef int16_t t_wav_int16;
typedef uint16_t t_wav_uint16;
typedef int32_t t_wav_int32;
typedef uint32_t t_wav_uint32;
typedef int64_t t_wav_int64;
typedef uint64_t t_wav_uint64;
typedef float t_wav_float32;
typedef double t_wav_float64;

typedef union
{
	t_wav_float32 f;
	t_wav_uint32 n;
} t_wav_conv;

#define waveformat_tag_int 1
#define waveformat_tag_float 3

//WAV file reader

typedef struct
{
	t_wav_uint32 (*m_read)(void * p_user_data,void * p_buffer,t_wav_uint32 p_bytes);
	void * m_user_data;
} t_wav_input_file_callback;

typedef struct
{
	void (*m_convert_float32)(t_wav_uint8 const * p_input,t_wav_float32 * p_sample_buffer,t_wav_uint32 p_sample_count);
	void (*m_convert_int16)(t_wav_uint8 const * p_input,t_wav_int16 * p_sample_buffer,t_wav_uint32 p_sample_count);
} t_wav_input_handler;

typedef struct
{
	t_wav_input_file_callback m_callback;

	t_wav_input_handler m_input_handler;

	t_wav_uint16 m_format_tag;
	t_wav_uint16 m_channels;
	t_wav_uint32 m_samples_per_sec;
	t_wav_uint32 m_avg_bytes_per_sec;
	t_wav_uint16 m_block_align;
	t_wav_uint16 m_bits_per_sample;

	t_wav_uint32 m_bytes_per_sample, m_buffer_size;

	t_wav_uint32 m_data_size;
	t_wav_uint32 m_data_position;



	t_wav_uint8 m_workbuffer[512];
} t_wav_input_file;

t_wav_uint32 waveformat_input_open(t_wav_input_file * p_file,t_wav_input_file_callback p_callback);

t_wav_uint32 waveformat_input_process_float32(t_wav_input_file * p_file,t_wav_float32 * p_sample_buffer,t_wav_uint32 p_sample_count);
t_wav_uint32 waveformat_input_process_int16(t_wav_input_file * p_file,t_wav_int16 * p_sample_buffer,t_wav_uint32 p_sample_count);

void waveformat_input_close(t_wav_input_file * p_file);

t_wav_uint32 waveformat_input_query_sample_rate(t_wav_input_file * p_file);
t_wav_uint32 waveformat_input_query_channels(t_wav_input_file * p_file);
t_wav_uint32 waveformat_input_query_length(t_wav_input_file * p_file);

//WAV file writer

typedef struct
{
	t_wav_uint32 (*m_write)(void * p_user_data,void const * p_buffer,t_wav_uint32 p_bytes);
	t_wav_uint32 (*m_seek)(void * p_user_data,t_wav_uint32 p_position);
	void * m_user_data;
} t_wav_output_file_callback;

typedef struct
{
	void (*m_convert_float32)(t_wav_float32 const * p_sample_buffer,t_wav_uint8 * p_output,t_wav_uint32 p_sample_count);
	void (*m_convert_int16)(t_wav_int16 const * p_sample_buffer,t_wav_uint8 * p_output,t_wav_uint32 p_sample_count);
} t_wav_output_handler;

typedef struct
{
	t_wav_output_file_callback m_callback;

	t_wav_output_handler m_output_handler;

	t_wav_uint32 m_channels;
	t_wav_uint32 m_bits_per_sample;
	t_wav_uint32 m_float;
	t_wav_uint32 m_sample_rate;
	t_wav_uint32 m_samples_written,m_samples_written_expected;

	t_wav_uint32 m_bytes_per_sample, m_buffer_size;

	t_wav_uint8 m_workbuffer[512];
} t_wav_output_file;

t_wav_uint32 waveformat_output_open(t_wav_output_file * p_file,t_wav_output_file_callback p_callback,t_wav_uint32 p_channels,t_wav_uint32 p_bits_per_sample,t_wav_uint32 p_float,t_wav_uint32 p_sample_rate,t_wav_uint32 p_expected_samples);

t_wav_uint32 waveformat_output_process_float32(t_wav_output_file * p_file,t_wav_float32 const * p_sample_buffer,t_wav_uint32 p_sample_count);
t_wav_uint32 waveformat_output_process_int16(t_wav_output_file * p_file,t_wav_int16 const * p_sample_buffer,t_wav_uint32 p_sample_count);

t_wav_uint32 waveformat_output_close(t_wav_output_file * p_file);

#ifdef __cplusplus
} //extern "C"
#endif

#endif //__LIBWAVEFORMAT_H__

