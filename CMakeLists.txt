cmake_minimum_required(VERSION 3.16)

project(libmpc
        VERSION 7.0.1
        DESCRIPTION "Musepack (MPC) audio codec"
        HOMEPAGE_URL "https://www.musepack.net/"
        LANGUAGES C)

include(TestBigEndian)
test_big_endian(MPC_BIG_ENDIAN_HOST)

# ---- options ----------------------------------------------------------------
option(MPC_BUILD_SHARED "Build libmpcdec as a shared library" ON)
option(MPC_BUILD_TESTS   "Build and run the fixture regression tests" OFF)
option(MPC_BUILD_MPCGAIN "Build mpcgain (requires libreplaygain)" ON)
option(MPC_BUILD_MPCCHAP "Build mpcchap (requires libcuefile)" ON)
# SIMD decoder kernels (128-bit): ARM64 NEON, x86-64 SSE2, WebAssembly
# SIMD128. Both native targets use baseline ISAs only (no -march bumps), so
# this defaults ON wherever a kernel exists. The scalar implementation
# always remains available as the reference/fallback path.
option(MPC_ENABLE_SIMD "Enable SIMD synthesis filter kernels" ON)
# WebAssembly SIMD128 codegen (-msimd128) for the whole wasm decoder.
# Independent of MPC_ENABLE_SIMD so the scalar-wasm, auto-vectorized-wasm
# and explicit-SIMD-wasm configurations can be compared directly.
set(MPC_WASM_SIMD ON CACHE BOOL "Emit WebAssembly SIMD128 (-msimd128) for the wasm decoder")
option(MPC_WASM_TEST_HOOKS "Export internal Wasm decoder test controls" OFF)
# Opt-in native tuning for local builds only; never enabled in releases.
option(MPC_ENABLE_NATIVE_TUNING "Tune native builds for the build host (-march=native, opt-in)" OFF)
option(MPC_ENABLE_PSY_PROFILE "Enable opt-in psychoacoustic CPU-time counters" OFF)

# Testing is enabled up front so subdirectories (e.g. sonic/, tests/) can
# register CTest cases when MPC_BUILD_TESTS is on.
if(MPC_BUILD_TESTS)
  enable_testing()
endif()

if(WIN32)
  # Static by default on Windows: the MSVC DLL/import-lib build is more
  # fragile and the historical Windows build was static. Shared can still be
  # enabled explicitly with -DMPC_BUILD_SHARED=ON.
  set(MPC_BUILD_SHARED OFF)
endif()

# ---- global settings --------------------------------------------------------
include(GNUInstallDirs)

# ---- encoder FP-semantics pin (Phase 2) -------------------------------------
# The encoder must be bit-reproducible against its scalar reference, so FP
# contraction (FMA) is disabled in every encoder translation unit whose
# results can affect output decisions (libmpcenc, libmpcpsy, mpcenc). This
# makes the scalar and SIMD paths provably identical (both do separate
# multiply + add). Rationale: FMA contraction is a compiler optimization not
# specified by the encoder algorithm; allowing it makes encoded output
# depend on target/compiler behavior.
#
# Controls: GCC/Clang -ffp-contract=off. MSVC: /fp:precise is sufficient
# because Visual Studio 2022's /fp:precise defaults to fp_contract(off)
# (no contractions); /fp:strict is reserved as a fallback only if an older
# MSVC toolchain (VS2019 and earlier, where /fp:precise may contract) is
# ever used. The decoder intentionally keeps its default (it allows
# contraction; see milestone 1).
function(mpc_pin_encoder_fp target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /fp:precise)
  else()
    target_compile_options(${target} PRIVATE -ffp-contract=off)
  endif()
endfunction()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The codebase uses POSIX APIs throughout (fseeko/ftello via common/fileio.h,
# popen in the CLI, mkdtemp in tests, ftruncate in mpcchap, M_PI in vendored
# libebur128). Strict ISO C11 (-std=c11) hides those on glibc; compiler
# extensions (gnu11, which also enables the feature-test macros) match what
# macOS already exposes and what the reference encoder is built with, without
# changing codec behavior.
set(CMAKE_C_EXTENSIONS ON)

if(NOT MSVC)
  add_compile_options(-Wall -Wextra)
endif()

# Opt-in local build tuning only; never part of a release (see README).
if(MPC_ENABLE_NATIVE_TUNING AND NOT MSVC AND NOT EMSCRIPTEN)
  add_compile_options(-march=native)
endif()

# 64-bit file offsets for POSIX builds (needed by common/fileio.h). Must be
# defined before any system header that pulls in off_t.
if(NOT WIN32)
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()

# ---- subdirectories ---------------------------------------------------------
if(EMSCRIPTEN)
  # WebAssembly target: the decoder core and its JS wrapper only. The
  # encoder, tools and WAV helpers are not needed (or usable) under wasm.
  set(MPC_BUILD_SHARED OFF)
  # Emscripten's musl libc hides fseeko/ftello without a feature macro.
  add_compile_definitions(_POSIX_C_SOURCE=200112L)
  add_subdirectory(codec/include)
  add_subdirectory(codec/libmpcdec)
  add_subdirectory(wasm)
  if(MPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()
  return()
endif()

add_subdirectory(codec/include)
add_subdirectory(codec/libmpcdec)
add_subdirectory(codec/libmpcenc)
add_subdirectory(codec/libmpcpsy)
add_subdirectory(codec/libwavformat)
add_subdirectory(core/libmusicpack)
add_subdirectory(core/musicpack)
add_subdirectory(core/sonic)
add_subdirectory(mpakhttp)
add_subdirectory(server)
add_subdirectory(codec/mpcdec)
add_subdirectory(bench)
add_subdirectory(codec/mpcenc)
add_subdirectory(codec/mpc2sv8)
add_subdirectory(codec/mpccut)
add_subdirectory(codec/wavcmp)
add_subdirectory(codec/mpcgain)
add_subdirectory(codec/mpcchap)

if(MPC_BUILD_TESTS)
  add_subdirectory(tests)
endif()
