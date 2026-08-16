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
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <musepack/musepack.h>
#include <libwaveformat.h>
#include <getopt.h>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifdef _WIN32
# include <fcntl.h>
# include <io.h>
# define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
# define SET_BINARY_MODE(file)
#endif

#define MPCDEC_MAJOR 1
#define MPCDEC_MINOR 0
#define MPCDEC_BUILD 0

#define _cat(a,b,c) #a"."#b"."#c
#define cat(a,b,c) _cat(a,b,c)
#define MPCDEC_VERSION cat(MPCDEC_MAJOR,MPCDEC_MINOR,MPCDEC_BUILD)

const char    About []        = "mpcdec - Musepack (MPC) decoder v" MPCDEC_VERSION " (C) 2006-2009 MDT\nBuilt " __DATE__ " " __TIME__ "\n";


t_wav_uint32 mpc_wav_output_write(void* p_user_data, void const* p_buffer, t_wav_uint32 p_bytes)
{
    FILE* p_handle = (FILE*) p_user_data;
    return (t_wav_uint32) fwrite(p_buffer, 1, p_bytes, p_handle);
}

t_wav_uint32 mpc_wav_output_seek(void* p_user_data, t_wav_uint32 p_position)
{
    FILE* p_handle = (FILE*) p_user_data;
    return (t_wav_uint32) !fseek(p_handle, p_position, SEEK_SET);
}

static void print_info(mpc_streaminfo * info, char * filename)
{
	int time = (int) mpc_streaminfo_get_length(info);
	int minutes = time / 60;
	int seconds = time % 60;

	fprintf(stderr, "file: %s\n", filename);
	fprintf(stderr, "stream version %d\n", info->stream_version);
	fprintf(stderr, "encoder: %s\n", info->encoder);
	fprintf(stderr, "profile: %s (q=%0.2f)\n", info->profile_name, info->profile - 5);
	fprintf(stderr, "PNS: %s\n", info->pns == 0xFF ? "unknow" : info->pns ? "on" : "off");
	fprintf(stderr, "mid/side stereo: %s\n", info->ms ? "on" : "off");
	fprintf(stderr, "gapless: %s\n", info->is_true_gapless ? "on" : "off");
	fprintf(stderr, "average bitrate: %6.1f kbps\n", info->average_bitrate * 1.e-3);
	fprintf(stderr, "samplerate: %d Hz\n", info->sample_freq);
	fprintf(stderr, "channels: %d\n", info->channels);
	fprintf(stderr, "length: %d:%.2d (%" PRIu64 " samples)\n", minutes, seconds,
	        (uint64_t) mpc_streaminfo_get_length_samples(info));
	fprintf(stderr, "file size: %" PRId64 " Bytes\n", info->total_file_length);
	fprintf(stderr, "track peak: %2.2f dB\n", info->peak_title / 256.f);
	fprintf(stderr, "track gain: %2.2f dB / %2.2f dB\n", info->gain_title / 256.f, info->gain_title == 0 ? 0 : 64.82f - info->gain_title / 256.f);
	fprintf(stderr, "album peak: %2.2f dB\n", info->peak_album / 256.f);
	fprintf(stderr, "album gain: %2.2f dB / %2.2f dB\n", info->gain_album / 256.f, info->gain_album == 0 ? 0 : 64.82f - info->gain_album / 256.f);
	fprintf(stderr, "\n");

}

static void
usage(const char *exename)
{
    fprintf(stderr, "Usage: %s [-i] [-h] <infile.mpc> [<outfile.wav>]\n"
			"-i : print file information on stdout\n"
			"-c : check the file for stream errors\n"
			"     (doesn't fully decode, outfile will be ignored)\n"
			"-h : print this help\n"
            "you can use stdin and stdout as resp. <infile.mpc> and\n"
            "<outfile.wav> replacing the file name by \"-\"\n", exename);
}

int
main(int argc, char **argv)
{
	mpc_reader reader;
	musepack_decoder* decoder = NULL;
	mpc_streaminfo si;
	musepack_error err = MUSEPACK_OK;
	mpc_status reader_err;
	mpc_bool_t info = MPC_FALSE, is_wav_output = MPC_FALSE, check = MPC_FALSE;
	float sample_buffer[MUSEPACK_FRAME_MAX * 2];
	clock_t begin, end;
	uint64_t sum = 0, total_samples = 0;
	t_wav_output_file wav_output;
	FILE *output = NULL;
	int reader_open = 0, wav_open = 0, exit_code = 1;
	int c;

    fprintf(stderr, About);

#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#endif

	while ((c = getopt(argc , argv, "ihc")) != -1) {
		switch (c) {
			case 'i':
				info = MPC_TRUE;
				break;
			case 'c':
				check = MPC_TRUE;
				break;
			case 'h':
				usage(argv[0]);
				return 0;
		}
	}

	if(2 < argc - optind || argc - optind < 1) {
        usage(argv[0]);
        return 0;
    }

	if (strcmp(argv[optind], "-") == 0) {
		SET_BINARY_MODE(stdin);
		reader_err = mpc_reader_init_stdio_stream(&reader, stdin);
	} else
		reader_err = mpc_reader_init_stdio(&reader, argv[optind]);
	if(reader_err < 0) {
		fprintf(stderr, "Unable to open input\n");
		goto cleanup;
	}
	reader_open = 1;

	decoder = musepack_decoder_open(&reader, &err);
	if(!decoder) {
		fprintf(stderr, "Invalid or unreadable Musepack stream\n");
		goto cleanup;
	}
	if (musepack_decoder_get_info(decoder, &si) != MUSEPACK_OK)
		goto cleanup;

	if (info == MPC_TRUE) {
		print_info(&si, argv[optind]);
		exit_code = 0;
		goto cleanup;
	}

	if (!check)
		is_wav_output = argc - optind > 1;
	if(is_wav_output) {
		t_wav_output_file_callback wavo_fc;
		uint64_t playable = musepack_decoder_length_samples(decoder);
		uint64_t expected_samples;
		memset(&wav_output, 0, sizeof wav_output);
		wavo_fc.m_seek = mpc_wav_output_seek;
		wavo_fc.m_write = mpc_wav_output_write;
		if (si.channels == 0 || playable > UINT64_MAX / si.channels) {
			fprintf(stderr, "Stream length is too large\n");
			goto cleanup;
		}
		expected_samples = playable * si.channels;
		if (expected_samples > (UINT32_MAX - 36u) / 2u) {
			fprintf(stderr, "Output is too large for a RIFF/WAV file\n");
			goto cleanup;
		}
		if (strcmp(argv[optind + 1], "-") == 0) {
			SET_BINARY_MODE(stdout);
			output = stdout;
		} else
			output = fopen(argv[optind + 1], "wb");
		if(!output) {
			fprintf(stderr, "Unable to open output\n");
			goto cleanup;
		}
		wavo_fc.m_user_data = output;
		if(!waveformat_output_open(&wav_output, wavo_fc, si.channels, 16, 0,
		                           si.sample_freq, (t_wav_uint32) expected_samples)) {
			fprintf(stderr, "Unable to write WAV header\n");
			err = MUSEPACK_ERR_IO;
			goto cleanup;
		}
		wav_open = 1;
	}

    if (check) {
        err = musepack_decoder_check_stream(decoder);
        if (err != MUSEPACK_OK)
            fprintf(stderr, "An error occured while decoding\n");
        else
            fprintf(stderr, "No error found\n");
		exit_code = err == MUSEPACK_OK ? 0 : 1;
		goto cleanup;
	}

	while(MPC_TRUE) {
		uint64_t frames;
		begin = clock();
		err = musepack_decoder_read(decoder, sample_buffer, MUSEPACK_FRAME_MAX, &frames);
		end = clock();
		if (end >= begin)
			sum += (uint64_t) (end - begin);
		if (err == MUSEPACK_ERR_EOF || err != MUSEPACK_OK)
			break;

		total_samples += frames;

		if(is_wav_output) {
			uint64_t n = frames * si.channels;
#ifdef MPC_FIXED_POINT
			mpc_int16_t tmp_buff[MUSEPACK_FRAME_MAX * 2];
			uint64_t i;
			for( i = 0; i < n; i++) {
				float tmp = sample_buffer[i] * 32768.0f;
				if (tmp > 32767.0f) tmp = 32767.0f;
				if (tmp < -32768.0f) tmp = -32768.0f;
				tmp_buff[i] = (mpc_int16_t) tmp;
			}
			if(waveformat_output_process_int16(&wav_output, tmp_buff, (t_wav_uint32) n) != (t_wav_uint32) n)
#else
			if(waveformat_output_process_float32(&wav_output, sample_buffer, (t_wav_uint32) n) != (t_wav_uint32) n)
#endif
			{
				err = MUSEPACK_ERR_IO;
				break;
			}
		}
	}

	if (err != MUSEPACK_OK && err != MUSEPACK_ERR_EOF)
		fprintf(stderr, "An error occured while decoding\n");

	{
		uint64_t speed;
		if (sum == 0) sum = 1;
		speed = (uint64_t) ((long double) total_samples * CLOCKS_PER_SEC * 100 /
		                   ((long double) si.sample_freq * sum));
		fprintf(stderr, "%" PRIu64 " samples decoded in %" PRIu64
		        " ms (%" PRIu64 ".%02" PRIu64 "x)\n", total_samples,
		        sum * 1000 / CLOCKS_PER_SEC, speed / 100, speed % 100);
	}
	exit_code = err == MUSEPACK_OK || err == MUSEPACK_ERR_EOF ? 0 : 1;

cleanup:
	if (wav_open && !waveformat_output_close(&wav_output)) {
		fprintf(stderr, "Unable to finalize WAV output\n");
		exit_code = 1;
	}
	if (output != NULL && fclose(output) != 0) {
		fprintf(stderr, "Unable to close WAV output\n");
		exit_code = 1;
	}
	if (decoder != NULL)
		musepack_decoder_close(decoder);
	if (reader_open)
		mpc_reader_exit_stdio(&reader);

#ifdef _MSC_VER
    assert(_CrtCheckMemory());
    _CrtDumpMemoryLeaks();
#endif
	return exit_code;
}
