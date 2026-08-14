#include "psy_profile.h"

#ifdef MPC_ENABLE_PSY_PROFILE

#include <string.h>
#include <time.h>

static mpc_psy_profile_t profile;
static unsigned int spectrum_depth;

const char* mpc_psy_sub_names[MPC_PSY_SUB_COUNT] = {
    "SubbandEnergy",
    "PartitionEnergy",
    "WeightedPartitionEnergy",
    "CalcUnpred",
    "SpreadingSignal",
    "ApplyTonalityOffset",
    "AdaptLtq",
    "CalcTemporalThreshold",
    "CalcMSThreshold",
    "ApplyLtq",
    "CalculateSMR",
    "CalcShortThreshold",
    "PreechoControl",
    "AdaptThresholds",
    "RaiseSMR_Signal",
    "CVD2048",
    "FindOptimalANS",
    "CEP_Analyse2048",
    "CEP_correlation",
    "CEP_maxsearch",
    "logfast",
};

uint64_t
mpc_psy_profile_now ( void )
{
    struct timespec ts;
    clock_gettime (CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
}

void
mpc_psy_profile_reset ( void )
{
    memset (&profile, 0, sizeof profile);
    spectrum_depth = 0;
}

void
mpc_psy_profile_set_total_start ( uint64_t ns )
{
    profile.total_start_ns = ns;
}

#define ADD_PROFILE(name) \
    void mpc_psy_profile_add_##name ( uint64_t ns ) \
    { \
        profile.name##_ns += ns; \
        profile.name##_calls++; \
    }

ADD_PROFILE(model)
ADD_PROFILE(raise_smr)
ADD_PROFILE(ns_analyse)
void
mpc_psy_profile_add_sub ( mpc_psy_sub_kind kind, uint64_t ns )
{
    profile.sub_ns[kind] += ns;
    profile.sub_calls[kind]++;
}
void
mpc_psy_profile_add_fft ( uint64_t ns )
{
    profile.fft_ns += ns;
    profile.fft_calls++;
    if ( spectrum_depth != 0 )
        profile.spectrum_fft_ns += ns;
}

void
mpc_psy_profile_spectrum_enter ( void )
{
    spectrum_depth++;
}

void
mpc_psy_profile_spectrum_leave ( uint64_t ns )
{
    spectrum_depth--;
    profile.spectrum_ns += ns;
    profile.spectrum_calls++;
}

mpc_psy_profile_t
mpc_psy_profile_get ( void )
{
    return profile;
}

#endif
