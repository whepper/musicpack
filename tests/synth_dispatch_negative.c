#include <stdio.h>

#include "decoder.h"
#include "internal.h"

int main(void)
{
    mpc_decoder decoder;
    mpc_decoder_synth_fn before;

    mpc_decoder_setup(&decoder);
    before = decoder.synth;
    if (mpc_decoder_has_synth_simd()) {
        fprintf(stderr, "synth_simd_unavailable requires a scalar-only build\n");
        return 77;
    }
    if (mpc_decoder_set_synth_impl(&decoder, MPC_SYNTH_SIMD)) {
        fprintf(stderr, "forced SIMD unexpectedly succeeded\n");
        return 1;
    }
    if (decoder.synth != before || decoder.synth != mpc_synthese_filter_float_scalar) {
        fprintf(stderr, "failed SIMD request changed scalar dispatch\n");
        return 1;
    }
    puts("PASS forced SIMD rejected without fallback");
    return 0;
}
