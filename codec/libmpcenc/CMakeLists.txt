set(MPCENC_SOURCES
    analy_filter.c
    bitstream.c
    encode_sv7.c
    huffsv7.c
    quant.c
    ${PROJECT_SOURCE_DIR}/codec/common/crc32.c)

# Bit-exact encoder SIMD analysis filterbank (analy_filter_simd.c) via the
# shared common/mpc_simd.h. Supported on aarch64 (NEON) and x86-64 (SSE2
# baseline). WebAssembly is deliberately out of scope for the encoder
# milestone. MPC_ENABLE_SIMD is the master switch (top-level); the scalar
# kernels always stay compiled as the reference.
set(MPC_ENC_SIMD_SUPPORTED FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|AARCH64|ARM64")
  set(MPC_ENC_SIMD_SUPPORTED TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|x64|amd64|AMD64|X86_64")
  set(MPC_ENC_SIMD_SUPPORTED TRUE)
endif()

set(MPC_ENC_USE_SIMD FALSE)
if(MPC_ENABLE_SIMD AND MPC_ENC_SIMD_SUPPORTED)
  set(MPC_ENC_USE_SIMD TRUE)
endif()

if(MPC_ENC_USE_SIMD)
  list(APPEND MPCENC_SOURCES analy_filter_simd.c)
endif()
set(MPC_ENCODER_SIMD_ENABLED ${MPC_ENC_USE_SIMD} CACHE INTERNAL
    "Compiled encoder analysis SIMD backend")

add_library(mpcenc_static STATIC ${MPCENC_SOURCES})

target_link_libraries(mpcenc_static PUBLIC mpc_headers)

# Bit-exact encoder: disable FP contraction (see CMakeLists.txt helper).
mpc_pin_encoder_fp(mpcenc_static)

# The analyser dispatch (libmpcenc.h) compiles the SIMD kernels in only when
# this is set; the CLI links mpcenc_static so it inherits the dispatch but
# does not need the define itself.
if(MPC_ENC_USE_SIMD)
  target_compile_definitions(mpcenc_static PRIVATE MPC_ENABLE_ENC_SIMD_KERNEL)
endif()

install(TARGETS mpcenc_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
