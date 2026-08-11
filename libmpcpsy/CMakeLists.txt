add_library(mpcpsy STATIC
    ans.c
    cvd.c
    fft4g.c
    fft_routines.c
    profile.c
    psy.c
    psy_tab.c
    ${PROJECT_SOURCE_DIR}/common/fastmath.c)

target_link_libraries(mpcpsy PUBLIC mpc_headers)

# Optimized math tables + CVD fast log. Must stay consistent with mpcenc,
# which also uses the FAST_MATH path.
target_compile_definitions(mpcpsy PUBLIC FAST_MATH CVD_FASTLOG)

# Bit-exact encoder: disable FP contraction (see CMakeLists.txt helper).
mpc_pin_encoder_fp(mpcpsy)

install(TARGETS mpcpsy ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
