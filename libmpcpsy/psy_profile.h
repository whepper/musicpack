#ifndef MPC_PSY_PROFILE_H_
#define MPC_PSY_PROFILE_H_

#include <stdint.h>

// Fine-grained sub-function counters (Phase 4 profiling). Each entry tracks
// one scalar function inside the psychoacoustic model body so the "other psy"
// bucket can be attributed to individual hotspots. Kept additive to the coarse
// counters above so historical Phase 3 comparisons remain valid.
typedef enum {
    MPC_PSY_SUB_SUBBAND_ENERGY = 0,
    MPC_PSY_SUB_PARTITION_ENERGY,
    MPC_PSY_SUB_WEIGHTED_PARTITION_ENERGY,
    MPC_PSY_SUB_CALC_UNPRED,
    MPC_PSY_SUB_SPREADING_SIGNAL,
    MPC_PSY_SUB_APPLY_TONALITY_OFFSET,
    MPC_PSY_SUB_ADAPT_LTQ,
    MPC_PSY_SUB_CALC_TEMPORAL_THRESHOLD,
    MPC_PSY_SUB_CALC_MS_THRESHOLD,
    MPC_PSY_SUB_APPLY_LTQ,
    MPC_PSY_SUB_CALCULATE_SMR,
    MPC_PSY_SUB_CALC_SHORT_THRESHOLD,
    MPC_PSY_SUB_PREECHO_CONTROL,
    MPC_PSY_SUB_ADAPT_THRESHOLDS,
    MPC_PSY_SUB_RAISE_SMR_SIGNAL,
    MPC_PSY_SUB_CVD2048,
    MPC_PSY_SUB_FIND_OPTIMAL_ANS,
    MPC_PSY_SUB_CEP_ANALYSE,
    MPC_PSY_SUB_CEP_CORRELATION,
    MPC_PSY_SUB_CEP_MAXSEARCH,
    MPC_PSY_SUB_LOGFAST,
    MPC_PSY_SUB_COUNT
} mpc_psy_sub_kind;

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
    uint64_t sub_ns[MPC_PSY_SUB_COUNT];
    uint64_t sub_calls[MPC_PSY_SUB_COUNT];
} mpc_psy_profile_t;

#ifdef MPC_ENABLE_PSY_PROFILE
extern const char* mpc_psy_sub_names[MPC_PSY_SUB_COUNT];
uint64_t mpc_psy_profile_now ( void );
void mpc_psy_profile_reset ( void );
void mpc_psy_profile_set_total_start ( uint64_t ns );
void mpc_psy_profile_add_model ( uint64_t ns );
void mpc_psy_profile_add_raise_smr ( uint64_t ns );
void mpc_psy_profile_add_ns_analyse ( uint64_t ns );
void mpc_psy_profile_add_fft ( uint64_t ns );
void mpc_psy_profile_add_sub ( mpc_psy_sub_kind kind, uint64_t ns );
void mpc_psy_profile_spectrum_enter ( void );
void mpc_psy_profile_spectrum_leave ( uint64_t ns );
mpc_psy_profile_t mpc_psy_profile_get ( void );
#endif

#endif
