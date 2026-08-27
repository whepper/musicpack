set(MPCDEC_SOURCES
    huffman.c
    mpc_bits_reader.c
    mpc_decoder.c
    mpc_demux.c
    mpc_reader.c
    musepack_decoder.c
    requant.c
    streaminfo.c
    synth_filter.c
    ${PROJECT_SOURCE_DIR}/common/crc32.c)

# ---------------------------------------------------------------------------
# SIMD synthesis filter kernel (NEON / SSE2 / wasm SIMD128 via mpc_simd.h).
# Supported where a 128-bit baseline SIMD ISA exists: aarch64 (Advanced SIMD
# is mandatory), x86-64 (SSE2 is the baseline) and wasm SIMD128 (only when
# -msimd128 is enabled). The scalar path always stays compiled as the
# reference/fallback. MPC_ENABLE_SIMD is the master switch (top-level).
# ---------------------------------------------------------------------------
set(MPC_SIMD_SUPPORTED FALSE)
if(EMSCRIPTEN AND MPC_WASM_SIMD)
  set(MPC_SIMD_SUPPORTED TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|AARCH64|ARM64")
  set(MPC_SIMD_SUPPORTED TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|x64|amd64|AMD64|X86_64")
  set(MPC_SIMD_SUPPORTED TRUE)
endif()

set(MPC_USE_SIMD FALSE)
if(MPC_ENABLE_SIMD AND MPC_SIMD_SUPPORTED)
  set(MPC_USE_SIMD TRUE)
endif()
set(MPC_DECODER_SIMD_BACKEND scalar)
if(MPC_USE_SIMD)
  if(EMSCRIPTEN)
    set(MPC_DECODER_SIMD_BACKEND wasm-simd128)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|AARCH64|ARM64")
    set(MPC_DECODER_SIMD_BACKEND neon)
  else()
    set(MPC_DECODER_SIMD_BACKEND sse2)
  endif()
endif()
set(MPC_DECODER_SIMD_ENABLED ${MPC_USE_SIMD} CACHE INTERNAL
    "Whether the decoder SIMD synthesis kernel is compiled")
set(MPC_DECODER_SIMD_BACKEND ${MPC_DECODER_SIMD_BACKEND} CACHE INTERNAL
    "Compiled decoder synthesis SIMD backend")

if(MPC_USE_SIMD)
  list(APPEND MPCDEC_SOURCES synth_filter_simd.c)
endif()

# ---------------------------------------------------------------------------
# Shared variant (default off Windows, on elsewhere). Its public symbols are
# exported via MUSEPACK_API/MPC_API; everything else is hidden with
# -fvisibility=hidden so internal tables and bit readers stay out of the ABI.
# ---------------------------------------------------------------------------
if(MPC_BUILD_SHARED)
  add_library(musepack SHARED ${MPCDEC_SOURCES})
  set_target_properties(musepack PROPERTIES
      VERSION ${PROJECT_VERSION}
      SOVERSION ${PROJECT_VERSION_MAJOR}
      OUTPUT_NAME musepack
      C_STANDARD 11 C_STANDARD_REQUIRED ON
      C_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      EXPORT_NAME Decoder)
  # dllexport for the new API; DLL_EXPORT drives the legacy MPC_API macro.
  target_compile_definitions(musepack PRIVATE MUSEPACK_BUILD_SHARED)
  if(WIN32)
    target_compile_definitions(musepack PRIVATE DLL_EXPORT)
  endif()
  # Consumers of the shared library import its symbols on Windows.
  target_compile_definitions(musepack PUBLIC MUSEPACK_USE_SHARED)
else()
  add_library(musepack STATIC ${MPCDEC_SOURCES})
  set_target_properties(musepack PROPERTIES
      OUTPUT_NAME musepack
      C_STANDARD 11 C_STANDARD_REQUIRED ON
      EXPORT_NAME Decoder)
endif()

# ---------------------------------------------------------------------------
# Static variant: produced alongside the shared library for in-tree
# white-box consumers (the unit test reaches into private headers). The SV8
# block tools only need legacy MPC_API bit readers, which stay exported, so
# they can link the shared library.
# ---------------------------------------------------------------------------
if(MPC_BUILD_SHARED)
  add_library(musepack_static STATIC ${MPCDEC_SOURCES})
  set_target_properties(musepack_static PROPERTIES
      OUTPUT_NAME musepack
      C_STANDARD 11 C_STANDARD_REQUIRED ON
      EXPORT_NAME Static)
endif()

# ---- common interface (headers, threads, libm) ---------------------------
find_package(Threads REQUIRED)
target_link_libraries(musepack PUBLIC mpc_headers Threads::Threads)
if(UNIX AND NOT APPLE)
  # libm provides pow/log10 etc. (macOS provides them in libSystem).
  target_link_libraries(musepack PUBLIC m)
endif()
if(MPC_BUILD_SHARED)
  target_link_libraries(musepack_static PUBLIC mpc_headers Threads::Threads)
  if(UNIX AND NOT APPLE)
    target_link_libraries(musepack_static PUBLIC m)
  endif()
endif()

# ---- SIMD kernel + wasm build configuration ------------------------------
# MPC_ENABLE_SIMD_KERNEL marks the synthesis SIMD implementation as compiled
# in (see decoder.h/internal.h dispatch). The wasm build explicitly gets -O3
# on objects (the historical link-only -O3 left the library unoptimized) and
# -msimd128 when MPC_WASM_SIMD is on.
if(MPC_USE_SIMD)
  target_compile_definitions(musepack PRIVATE MPC_ENABLE_SIMD_KERNEL)
endif()
if(EMSCRIPTEN)
  target_compile_options(musepack PRIVATE -O3)
  if(MPC_WASM_SIMD)
    target_compile_options(musepack PRIVATE -msimd128)
  endif()
endif()
if(MPC_BUILD_SHARED)
  if(MPC_USE_SIMD)
    target_compile_definitions(musepack_static PRIVATE MPC_ENABLE_SIMD_KERNEL)
  endif()
  if(EMSCRIPTEN)
    target_compile_options(musepack_static PRIVATE -O3)
    if(MPC_WASM_SIMD)
      target_compile_options(musepack_static PRIVATE -msimd128)
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
if(MPC_BUILD_SHARED)
  install(TARGETS musepack musepack_static
      EXPORT musepack-targets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  install(TARGETS musepack
      EXPORT musepack-targets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# pkg-config metadata: the new identity plus the historical name.
set(prefix ${CMAKE_INSTALL_PREFIX})
set(MUSEPACK_PC_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
if(UNIX AND NOT APPLE)
  string(APPEND MUSEPACK_PC_PRIVATE_LIBS " -lm")
endif()
string(STRIP "${MUSEPACK_PC_PRIVATE_LIBS}" MUSEPACK_PC_PRIVATE_LIBS)
configure_file(musepack.pc.in ${CMAKE_CURRENT_BINARY_DIR}/musepack.pc @ONLY)
configure_file(libmpcdec.pc.in ${CMAKE_CURRENT_BINARY_DIR}/libmpcdec.pc @ONLY)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/musepack.pc
    ${CMAKE_CURRENT_BINARY_DIR}/libmpcdec.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# CMake package config: find_package(Musepack CONFIG) -> Musepack::Decoder
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MusepackConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/MusepackConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MusepackConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Musepack)
install(EXPORT musepack-targets
    FILE MusepackTargets.cmake
    NAMESPACE Musepack::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Musepack)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MusepackConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MusepackConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Musepack)
