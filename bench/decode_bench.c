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
 * Native decoder benchmark over the legacy demux path, white-box linked so
 * it can force the synthesis filter implementation for A/B measurements.
 *
 * Opens each .mpc file, decodes it fully `--iterations` times (re-inits the
 * demux each pass so header/seek state is identical across impls), and
 * reports wall and CPU time plus the realtime decode multiplier
 * (audio_seconds / wall_seconds). One TSV row per input.
 *
 * The `--impl scalar|simd` flag forces the synthesis filter implementation
 * through the white-box hook; both paths are compiled into the bench build.
 *
 * Usage: decode_bench [--iterations N] [--impl scalar|simd] file.mpc [...]
 * Output (tab-separated): file  sample_rate  channels  audio_s  wall_ms
 *                          cpu_ms  realtime_x  impl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpc/mpcdec.h>
#include <mpc/reader.h>

/* White-box: force the synthesis implementation + reach the inner decoder. */
#include "decoder.h"
#include "internal.h"

#define IMPL_AUTO   0
#define IMPL_SCALAR 1
#define IMPL_SIMD   2

static int iterations = 1;
static int impl_forced = IMPL_AUTO;

#if defined(_WIN32)
# include <windows.h>
# include <time.h>
static double now_wall(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart / (double) f.QuadPart;
}
static double now_cpu(void)
{
    return (double) clock() / (double) CLOCKS_PER_SEC;
}
#else
# include <time.h>
static double now_wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}
static double now_cpu(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}
#endif

static int run_file(const char *path)
{
    mpc_reader reader;
    mpc_demux *demux;
    mpc_streaminfo si;
    mpc_frame_info frame;
    MPC_SAMPLE_FORMAT pcm[MPC_DECODER_BUFFER_LENGTH];
    uint64_t total = 0;
    double audio_s = 0.0, w0, w1, c0, c1, wall_s = 0.0, cpu_s = 0.0;
    int i, rc = 0;

    for (i = 0; i < iterations; i++) {
        if (mpc_reader_init_stdio(&reader, path) != MPC_STATUS_OK) {
            fprintf(stderr, "bench: cannot open %s\n", path);
            return 1;
        }
        demux = mpc_demux_init(&reader);
        if (demux == 0) {
            fprintf(stderr, "bench: cannot read header of %s\n", path);
            mpc_reader_exit_stdio(&reader);
            return 1;
        }
        if (impl_forced == IMPL_SIMD && !mpc_decoder_has_synth_simd()) {
            fprintf(stderr, "bench: SIMD kernel is not compiled in\n");
            mpc_demux_exit(demux);
            mpc_reader_exit_stdio(&reader);
            return 1;
        }
        if (impl_forced != IMPL_AUTO &&
            !mpc_decoder_set_synth_impl(demux->d, impl_forced)) {
            fprintf(stderr, "bench: requested synthesis implementation is unavailable\n");
            mpc_demux_exit(demux);
            mpc_reader_exit_stdio(&reader);
            return 1;
        }

        mpc_demux_get_info(demux, &si);
        if (i == 0) {
            audio_s = (double) mpc_streaminfo_get_length_samples(&si)
                    / (double) si.sample_freq;
        }

        w0 = now_wall();
        c0 = now_cpu();
        memset(&frame, 0, sizeof frame);
        frame.buffer = pcm;
        for (;;) {
            mpc_demux_decode(demux, &frame);
            if (frame.bits == -1)
                break;
            if (frame.samples > 0)
                total += frame.samples;
        }
        c1 = now_cpu();
        w1 = now_wall();

        wall_s += w1 - w0;
        cpu_s += c1 - c0;

        mpc_demux_exit(demux);
        mpc_reader_exit_stdio(&reader);
    }
    if (total != (uint64_t) mpc_streaminfo_get_length_samples(&si) * iterations) {
        fprintf(stderr, "bench: incomplete decode of %s (%llu of %llu frames)\n",
                path, (unsigned long long) total,
                (unsigned long long) mpc_streaminfo_get_length_samples(&si) * iterations);
        return 1;
    }

    printf("%s\t%u\t%u\t%.3f\t%.1f\t%.1f\t%.3f\t%s\n",
           path, si.sample_freq, si.channels,
           audio_s * iterations,
           wall_s * 1e3, cpu_s * 1e3,
           audio_s * iterations / (wall_s > 0 ? wall_s : 1e-9),
           impl_forced == IMPL_SCALAR ? "scalar"
             : impl_forced == IMPL_SIMD ? "simd" : "auto");

    return rc;
}

int main(int argc, char **argv)
{
    int rc = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            char *end;
            long value = strtol(argv[++i], &end, 10);
            if (*end != '\0' || value < 1 || value > 1000000) {
                fprintf(stderr, "bench: invalid --iterations value\n");
                return 2;
            }
            iterations = (int) value;
        } else if (strcmp(argv[i], "--impl") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "scalar") == 0) impl_forced = IMPL_SCALAR;
            else if (strcmp(v, "simd") == 0) impl_forced = IMPL_SIMD;
            else {
                fprintf(stderr, "bench: unknown --impl %s (scalar|simd)\n", v);
                return 2;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "bench: unknown option %s\n", argv[i]);
            return 2;
        } else {
            rc |= run_file(argv[i]);
        }
    }

    return rc;
}
