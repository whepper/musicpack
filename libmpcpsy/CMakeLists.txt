add_library(mpcpsy STATIC
    ans.c
    cvd.c
    fft4g.c
    fft_routines.c
    profile.c
    psy.c
    psy_profile.c
    psy_tab.c
    ${PROJECT_SOURCE_DIR}/common/fastmath.c)

# Bit-exact psychoacoustic SIMD kernels (Phase 3): fft_routines_simd.c
# (windowing/power lane=bin) and fft4g_simd.c (lane-parallel FFT). Supported
# on aarch64 (NEON) and x86-64 (SSE2 baseline); wasm encoder is out of
# scope. MPC_ENABLE_SIMD is the master switch; the scalar kernels always stay
# compiled as the reference.
set(MPC_PSY_SIMD_SUPPORTED FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|AARCH64|ARM64")
  set(MPC_PSY_SIMD_SUPPORTED TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|x64|amd64|AMD64|X86_64")
  set(MPC_PSY_SIMD_SUPPORTED TRUE)
endif()

if(MPC_ENABLE_SIMD AND MPC_PSY_SIMD_SUPPORTED)
  target_sources(mpcpsy PRIVATE fft_routines_simd.c fft4g_simd.c)
  target_compile_definitions(mpcpsy PRIVATE MPC_ENABLE_PSY_SIMD_KERNEL)
  set(MPC_PSY_SIMD_ENABLED TRUE)
else()
  set(MPC_PSY_SIMD_ENABLED FALSE)
endif()
set(MPC_PSY_SIMD_ENABLED ${MPC_PSY_SIMD_ENABLED} CACHE INTERNAL
    "Compiled psychoacoustic SIMD backend")

target_link_libraries(mpcpsy PUBLIC mpc_headers)

# profile.c: the leading entries of the static Profiles[16] lookup table use
# the canonical `{ 0 }` aggregate initializer, which -Wmissing-field-
# initializers (from -Wextra) flags even though `{ 0 }` fully zero-initializes
# the struct. The table is read-only static data, so suppress the diagnostic.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/profile.c
    PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/W0>;$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wno-missing-field-initializers>")

# fft_routines_simd.c: the magnitude/power loops index a[2*i] with `i` an int
# bounded by a small constant (<=1024). -Waggressive-loop-optimizations
# assumes the int bound can make 2*i overflow and warns; the loop is provably
# bounded at runtime. Changing the loop index types would risk the bit-exact
# SIMD output, so suppress the diagnostic for this file.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/fft_routines_simd.c
    PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/W0>;$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wno-aggressive-loop-optimizations>")

# Optimized math tables + CVD fast log. Must stay consistent with mpcenc,
# which also uses the FAST_MATH path.
target_compile_definitions(mpcpsy PUBLIC FAST_MATH CVD_FASTLOG)
if(MPC_ENABLE_PSY_PROFILE)
  target_compile_definitions(mpcpsy PUBLIC MPC_ENABLE_PSY_PROFILE)
endif()

# Bit-exact encoder: disable FP contraction (see CMakeLists.txt helper).
mpc_pin_encoder_fp(mpcpsy)

install(TARGETS mpcpsy ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
