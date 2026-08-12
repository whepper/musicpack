#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "libmpcpsy.h"

#ifdef FAST_MATH
void Init_FastMath ( void );
#endif
void Init_Psychoakustik ( PsyModel* );
void Init_Psychoakustiktabellen ( PsyModel* );
void SetQualityParams ( PsyModel*, float );

static float input[2176];
static float e256[4][128];
static float e1024[2][512];
static float e2048[2][1024];
static float phase[2][512];
static volatile float sink;

static uint64_t
now_ns ( void )
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter (&counter);
    QueryPerformanceFrequency (&frequency);
    return (uint64_t) counter.QuadPart * 1000000000u / (uint64_t) frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
#endif
}

static void
run_mix ( void )
{
    PowSpec2048_2 (input, input, e2048[0], e2048[1]);
    PolarSpec1024_2 (input, input + 576, e1024[0], e1024[1], phase[0], phase[1]);
    PolarSpec1024_2 (input + 576, input, e1024[0], e1024[1], phase[0], phase[1]);
    PowSpec256_4 (input, input + 576, input, input + 576,
                  e256[0], e256[1], e256[2], e256[3]);
    PowSpec256_4 (input + 144, input + 720, input + 144, input + 720,
                  e256[0], e256[1], e256[2], e256[3]);
    PowSpec256_4 (input + 288, input + 864, input + 288, input + 864,
                  e256[0], e256[1], e256[2], e256[3]);
    PowSpec256_4 (input + 432, input + 1008, input + 432, input + 1008,
                  e256[0], e256[1], e256[2], e256[3]);
    PowSpec1024_2 (input, input + 576, e1024[0], e1024[1]);
    PowSpec1024_2 (input + 576, input, e1024[0], e1024[1]);
    sink = e256[0][17] + e1024[0][101] + e2048[0][509] + phase[0][211];
}

static void
run_case ( const char* name, int iterations, void (*fn)(void) )
{
    uint64_t start;
    uint64_t elapsed;
    int i;

    fn ();
    start = now_ns ();
    for ( i = 0; i < iterations; i++ )
        fn ();
    elapsed = now_ns () - start;
    printf ("%s\t%d\t%.3f\t%.1f\n", name, iterations,
            elapsed / 1000000.0,
            iterations * 1000000000.0 / elapsed);
}

static void run_256 ( void )
{
    PowSpec256_4 (input, input + 576, input, input + 576,
                  e256[0], e256[1], e256[2], e256[3]);
    sink = e256[0][17];
}

static void run_1024 ( void )
{
    PowSpec1024_2 (input, input + 576, e1024[0], e1024[1]);
    sink = e1024[0][101];
}

static void run_polar ( void )
{
    PolarSpec1024_2 (input, input + 576, e1024[0], e1024[1], phase[0], phase[1]);
    sink = phase[0][211];
}

static void run_2048 ( void )
{
    PowSpec2048_2 (input, input, e2048[0], e2048[1]);
    sink = e2048[0][509];
}

int
main ( int argc, char** argv )
{
    PsyModel model;
    int impl = MPC_PSY_AUTO;
    int iterations = 1000;
    int quality = 6;
    unsigned int state = 12345u;
    int i;

    for ( i = 1; i < argc; i++ ) {
        if ( strcmp (argv[i], "--impl") == 0 && ++i < argc ) {
            if ( strcmp (argv[i], "scalar") == 0 ) impl = MPC_PSY_SCALAR;
            else if ( strcmp (argv[i], "simd") == 0 ) impl = MPC_PSY_SIMD;
            else { fprintf (stderr, "unknown implementation\n"); return 2; }
        } else if ( strcmp (argv[i], "--iterations") == 0 && ++i < argc ) {
            iterations = atoi (argv[i]);
        } else if ( strcmp (argv[i], "--quality") == 0 && ++i < argc ) {
            quality = atoi (argv[i]);
        } else {
            fprintf (stderr, "usage: psy_fft_bench --impl scalar|simd [--iterations N] [--quality 5|6|7]\n");
            return 2;
        }
    }
    if ( impl == MPC_PSY_AUTO ) { fprintf (stderr, "--impl is required\n"); return 2; }
    if ( impl == MPC_PSY_SIMD && !mpc_psy_has_simd () ) {
        fprintf (stderr, "psychoacoustic SIMD implementation is unavailable\n");
        return 1;
    }
    if ( iterations < 1 || quality < 5 || quality > 7 ) return 2;

    for ( i = 0; i < (int)(sizeof input / sizeof input[0]); i++ ) {
        state = state * 1664525u + 1013904223u;
        input[i] = ((float)(state >> 8) / 16777216.0f) - 0.5f;
    }
#ifdef FAST_MATH
    Init_FastMath ();
#endif
    memset (&model, 0, sizeof model);
    SetQualityParams (&model, (float) quality);
    Init_Psychoakustik (&model);
    model.SampleFreq = 44100.f;
    SetQualityParams (&model, (float) quality);
    Init_Psychoakustiktabellen (&model);
    mpc_psy_set_impl (impl);

    printf ("# quality=%d impl=%s columns=case iterations wall_ms calls_per_s\n",
            quality, impl == MPC_PSY_SIMD ? "simd" : "scalar");
    run_case ("PowSpec256_4", iterations, run_256);
    run_case ("PowSpec1024_2", iterations, run_1024);
    run_case ("PolarSpec1024_2", iterations, run_polar);
    run_case ("PowSpec2048_2", iterations, run_2048);
    run_case ("production_mix", iterations, run_mix);
    return 0;
}
