#ifndef MPC_PSY_PROFILE_H_
#define MPC_PSY_PROFILE_H_

#include <stdint.h>

typedef struct {
    uint64_t model_ns;
    uint64_t raise_smr_ns;
    uint64_t ns_analyse_ns;
    uint64_t fft_ns;
    uint64_t spectrum_ns;
    uint64_t spectrum_fft_ns;
    uint64_t model_calls;
    uint64_t raise_smr_calls;
    uint64_t ns_analyse_calls;
    uint64_t fft_calls;
    uint64_t spectrum_calls;
    uint64_t total_start_ns;
} mpc_psy_profile_t;

#ifdef MPC_ENABLE_PSY_PROFILE
uint64_t mpc_psy_profile_now ( void );
void mpc_psy_profile_reset ( void );
void mpc_psy_profile_set_total_start ( uint64_t ns );
void mpc_psy_profile_add_model ( uint64_t ns );
void mpc_psy_profile_add_raise_smr ( uint64_t ns );
void mpc_psy_profile_add_ns_analyse ( uint64_t ns );
void mpc_psy_profile_add_fft ( uint64_t ns );
void mpc_psy_profile_spectrum_enter ( void );
void mpc_psy_profile_spectrum_leave ( uint64_t ns );
mpc_psy_profile_t mpc_psy_profile_get ( void );
#endif

#endif
