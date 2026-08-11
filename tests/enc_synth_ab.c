/*
 * Encoder scalar-vs-SIMD differential test (Phase 2, white-box).
 *
 * Runs the analysis filterbank (Analyse_Filter / Analyse_Init) for the same
 * deterministic input under the forced scalar and forced SIMD implementations
 * and asserts the subband output is bit-identical. Both runs start from the
 * same (zero) analyser window and evolve it frame by frame, so any divergence
 * anywhere also shows up as a state divergence in later frames. This is the
 * internal "first divergence" tool: it reports the first (frame, band,
 * subframe) whose output differs.
 *
 * The end-to-end byte-identity gate is the enc_compat CTest; this test
 * pinpoints where a hypothetical future divergence occurs.
 *
 * Usage: enc_synth_ab   (no arguments; deterministic)
 * Exit:  0 if every frame is bit-identical, 1 otherwise.
 */

#if defined(_WIN32)
# define _USE_MATH_DEFINES /* must precede <math.h> for M_PI on MSVC */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpc/datatypes.h>

#include "libmpcenc.h"

#define FRAMES 8

static void
fill_main ( PCMDataTyp* main )
{
    int i;
    for ( i = 0; i < ANABUFFER; i++ ) {
        float v = (float) sin (2.0 * M_PI * 440.0 * (double) i / 44100.0)
                + 0.3f * (float) cos (2.0 * M_PI * 2000.0 * (double) i / 44100.0);
        main->L [i] = v;
        main->R [i] = 0.8f * v;
        main->M [i] = v;
        main->S [i] = 0.2f * v;
    }
}

int
main ( void )
{
    PCMDataTyp Main;
    static SubbandFloatTyp Xa [FRAMES] [32];
    static SubbandFloatTyp Xb [FRAMES] [32];
    mpc_encoder_t enc;
    int f, band, n, bad = 0;

    fill_main (&Main);
    // Real init path: builds the coefficient tables and selects the analyser
    // (Klemm() runs inside mpc_encoder_init). mpc_enc_set_impl then forces
    // each implementation for the A/B.
    mpc_encoder_init (&enc, 0, 6, 1);

    mpc_enc_set_impl (MPC_ENC_SCALAR);
    mpc_enc_reset_filter ();
    for ( f = 0; f < FRAMES; f++ )
        Analyse_Filter (&Main, Xa [f], 31);

    mpc_enc_set_impl (MPC_ENC_SIMD);
    mpc_enc_reset_filter ();
    for ( f = 0; f < FRAMES; f++ )
        Analyse_Filter (&Main, Xb [f], 31);

    for ( f = 0; f < FRAMES && !bad; f++ ) {
        for ( band = 0; band < 32; band++ ) {
            for ( n = 0; n < 36; n++ ) {
                if ( Xa [f] [band].L [n] != Xb [f] [band].L [n] ) {
                    printf ( "FAIL frame %d band %d subframe %d L: scalar=%.9g simd=%.9g\n",
                             f, band, n, Xa [f] [band].L [n], Xb [f] [band].L [n] );
                    bad = 1;
                    break;
                }
                if ( Xa [f] [band].R [n] != Xb [f] [band].R [n] ) {
                    printf ( "FAIL frame %d band %d subframe %d R: scalar=%.9g simd=%.9g\n",
                             f, band, n, Xa [f] [band].R [n], Xb [f] [band].R [n] );
                    bad = 1;
                    break;
                }
            }
            if ( bad ) break;
        }
    }

    /* Analyse_Init (the silence/DC fill path) must also agree. */
    {
        SubbandFloatTyp Ia [32], Ib [32];
        mpc_enc_set_impl (MPC_ENC_SCALAR); mpc_enc_reset_filter ();
        Analyse_Init (0.5f, -0.25f, Ia, 31);
        mpc_enc_set_impl (MPC_ENC_SIMD);   mpc_enc_reset_filter ();
        Analyse_Init (0.5f, -0.25f, Ib, 31);
        for ( band = 0; band < 32; band++ ) {
            for ( n = 0; n < 36; n++ ) {
                if ( Ia [band].L [n] != Ib [band].L [n] || Ia [band].R [n] != Ib [band].R [n] ) {
                    printf ( "FAIL Analyse_Init band %d subframe %d\n", band, n );
                    bad = 1;
                    break;
                }
            }
            if ( bad ) break;
        }
    }

    if ( bad ) {
        printf ( "enc_synth_ab: scalar/SIMD analysis filterbank diverged\n" );
        mpc_encoder_exit (&enc);
        return 1;
    }
    mpc_encoder_exit (&enc);
    printf ( "enc_synth_ab: %d frames x 32 bands x 36 subframes bit-identical (scalar vs SIMD)\n", FRAMES );
    return 0;
}
