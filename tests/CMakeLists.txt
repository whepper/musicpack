# Tests for the Musepack libraries and tools. Only included when
# MPC_BUILD_TESTS is ON (see the top-level CMakeLists.txt).

# The WebAssembly build is decoder-only (no encoder/tools), so the only
# applicable test is the WASM smoke test.
if(EMSCRIPTEN)
  add_test(NAME wasm_smoke
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_wasm_smoke.sh
                    ${CMAKE_BINARY_DIR})
  if(MPC_DECODER_SIMD_ENABLED AND MPC_WASM_TEST_HOOKS)
    add_test(NAME wasm_synth_ab
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_wasm_synth_ab.sh
                     ${CMAKE_BINARY_DIR})
  endif()
  # Phase 6: the web client's audio guarantees against the real wasm decoder —
  # gapless two-track continuity, exact track-end frames, demand-reader seek
  # accounting (seeking never downloads the whole file). Needs Node 22.6+
  # (the harness is plain .mjs with no TS imports).
  add_test(NAME web_wasm_gapless
           COMMAND node
                   ${CMAKE_CURRENT_SOURCE_DIR}/../web/tests/node/wasm-gapless.mjs
                   ${CMAKE_BINARY_DIR}/wasm/musepack.js
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5.mpc
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q7.mpc
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5-48s.mpc)
  return()
endif()

# ---- C unit tests (link against the real libraries) ------------------------
# The unit suite is white-box: it reaches into private decoder headers, so it
# links the static variant when a shared library is being built.
if(MPC_BUILD_SHARED)
  set(MPC_WHITEBOX_LIB musepack_static)
else()
  set(MPC_WHITEBOX_LIB musepack)
endif()

add_executable(mpc_unit_tests unit_tests.c)
target_link_libraries(mpc_unit_tests PRIVATE ${MPC_WHITEBOX_LIB} mpcenc_static)
target_include_directories(mpc_unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/libmpcdec
    ${PROJECT_SOURCE_DIR}/libmpcenc)

add_test(NAME unit COMMAND mpc_unit_tests)
if(WIN32)
  # The shared libmpcdec DLL must be findable at runtime.
  set_tests_properties(unit PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$ENV{PATH}")
endif()

# ---- stable libmusepack API tests -----------------------------------------
add_executable(mpc_api_tests api_tests.c)
target_link_libraries(mpc_api_tests PRIVATE musepack)
target_include_directories(mpc_api_tests PRIVATE ${PROJECT_SOURCE_DIR}/libmpcdec)

add_test(NAME api COMMAND mpc_api_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5.mpc
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine48-q6.mpc)
if(WIN32)
  set_tests_properties(api PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$ENV{PATH}")
endif()

# ---- musicpack CLI executable discovery -----------------------------------
add_executable(mpc_musicpack_tool_path_tests
    musicpack_tool_path_tests.c
    ${PROJECT_SOURCE_DIR}/musicpack/tool_path.c)
target_include_directories(mpc_musicpack_tool_path_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/musicpack)
add_test(NAME musicpack_tool_path COMMAND mpc_musicpack_tool_path_tests)

# ---- scalar-vs-SIMD synthesis differential test ---------------------------
# Decodes the fixtures once per synthesis path and asserts they agree within
# the fixture tolerance. Requires MPC_ENABLE_SIMD (both kernels compiled in);
# with SIMD off the test fails closed. White-box (links the static lib +
# internal headers).
if(MPC_BUILD_SHARED)
  set(MPC_WHITEBOX_LIB musepack_static)
else()
  set(MPC_WHITEBOX_LIB musepack)
endif()
add_executable(mpc_synth_diff synth_diff.c)
target_link_libraries(mpc_synth_diff PRIVATE ${MPC_WHITEBOX_LIB})
target_include_directories(mpc_synth_diff PRIVATE ${PROJECT_SOURCE_DIR}/libmpcdec)
if(MPC_DECODER_SIMD_ENABLED)
  target_compile_definitions(mpc_synth_diff PRIVATE MPC_ENABLE_SIMD_KERNEL)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_synth_diff PRIVATE m)
endif()

if(MPC_DECODER_SIMD_ENABLED)
  add_test(NAME synth_ab COMMAND mpc_synth_diff
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine32-q8.mpc
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine37-q4.mpc
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5.mpc
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5-48s.mpc
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q7.mpc
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine48-q6.mpc)
endif()

add_executable(mpc_synth_dispatch_negative synth_dispatch_negative.c)
target_link_libraries(mpc_synth_dispatch_negative PRIVATE ${MPC_WHITEBOX_LIB})
target_include_directories(mpc_synth_dispatch_negative PRIVATE
    ${PROJECT_SOURCE_DIR}/libmpcdec)
add_test(NAME synth_simd_unavailable COMMAND mpc_synth_dispatch_negative)
set_tests_properties(synth_simd_unavailable PROPERTIES SKIP_RETURN_CODE 77)

if(MPC_DECODER_SIMD_ENABLED)
  add_executable(mpc_synth_state_ab synth_state_ab.c)
  target_link_libraries(mpc_synth_state_ab PRIVATE ${MPC_WHITEBOX_LIB})
  target_include_directories(mpc_synth_state_ab PRIVATE
      ${PROJECT_SOURCE_DIR}/libmpcdec)
  target_compile_definitions(mpc_synth_state_ab PRIVATE MPC_ENABLE_SIMD_KERNEL)
  add_test(NAME synth_state_ab COMMAND mpc_synth_state_ab)
endif()

# ---- .mpack / libmusicpack tests ------------------------------------------
add_executable(mpc_mpack_tests mpack_tests.c)
target_link_libraries(mpc_mpack_tests PRIVATE musicpack musepack)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_mpack_tests PRIVATE m)
endif()
target_include_directories(mpc_mpack_tests PRIVATE ${PROJECT_SOURCE_DIR}/libmusicpack)

add_test(NAME mpack COMMAND mpc_mpack_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/reference/test-musicpack-album.mpack
    ${CMAKE_CURRENT_SOURCE_DIR}/reference/test-flac-album.mpack
    ${CMAKE_CURRENT_SOURCE_DIR}/reference/meta)

# ---- native source decoding (FLAC/WAV/MPC) via the audio abstraction ------
add_executable(mpc_audio_tests audio_tests.c)
target_link_libraries(mpc_audio_tests PRIVATE musicpack musepack)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_audio_tests PRIVATE m)
endif()
target_include_directories(mpc_audio_tests PRIVATE ${PROJECT_SOURCE_DIR}/libmusicpack)

add_test(NAME audio_decode COMMAND mpc_audio_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5.mpc)
if(WIN32)
  set_tests_properties(audio_decode PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
endif()

# Native APEv2 tag dump used by the encode integration test (run_encode.sh).
add_executable(mpc_ape_tag_dump ape_tags.c)
target_link_libraries(mpc_ape_tag_dump PRIVATE musicpack)
target_include_directories(mpc_ape_tag_dump PRIVATE ${PROJECT_SOURCE_DIR}/libmusicpack)
if(WIN32)
  set_target_properties(mpc_ape_tag_dump PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME mpack_conformance
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_mpack_conformance.py
            $<TARGET_FILE:musicpack_cmd>
            ${CMAKE_CURRENT_SOURCE_DIR}/generate_mpack_conformance.py)
if(WIN32)
  set_tests_properties(mpack PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
  set_tests_properties(mpack_conformance PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
endif()

# ---- Sonic container (musicpack-sonic v1) tests ----------------------------
add_executable(mpc_sonic_tests sonic_tests.c)
target_link_libraries(mpc_sonic_tests PRIVATE musicpack)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_sonic_tests PRIVATE m)
endif()
target_include_directories(mpc_sonic_tests PRIVATE ${PROJECT_SOURCE_DIR}/libmusicpack)

add_test(NAME sonic_unit COMMAND mpc_sonic_tests)
if(WIN32)
  set_tests_properties(sonic_unit PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
endif()

# ---- Waveform envelope (musicpack-waveform v1) tests -----------------------
add_executable(mpc_waveform_tests waveform_tests.c)
target_link_libraries(mpc_waveform_tests PRIVATE musicpack)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_waveform_tests PRIVATE m)
endif()
target_include_directories(mpc_waveform_tests PRIVATE ${PROJECT_SOURCE_DIR}/libmusicpack)

add_test(NAME waveform_unit COMMAND mpc_waveform_tests)
if(WIN32)
  set_tests_properties(waveform_unit PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
endif()

# ---- musicpack-server core unit tests (all platforms) ---------------------
add_executable(mpc_server_tests server_tests.c)
target_link_libraries(mpc_server_tests PRIVATE musicpack_server)
target_include_directories(mpc_server_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/server/vendor/sqlite)
if(WIN32)
  target_include_directories(mpc_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/win32)
  target_sources(mpc_server_tests PRIVATE ${PROJECT_SOURCE_DIR}/win32/dirent.c)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_server_tests PRIVATE m)
endif()

add_test(NAME server_unit COMMAND mpc_server_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/reference/test-musicpack-album.mpack
    ${CMAKE_CURRENT_SOURCE_DIR}/reference/test-flac-album.mpack)
if(WIN32)
  set_tests_properties(server_unit PROPERTIES
      ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musepack>$<SEMICOLON>$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$ENV{PATH}")
endif()

# ---- shell-based integration / robustness tests (UNIX only) ---------------
if(UNIX)
    # Fixture regression: decode(fixture.mpc) must equal the golden .wav.
    add_test(NAME fixtures
             COMMAND ${CMAKE_COMMAND} -E env
                     MPCDEC=$<TARGET_FILE:mpcdec_cmd>
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh)

    # SV8 format invariant: the encoder must always emit Musepack SV8
    # (MPCK magic, SH stream version 8, expected EI encoder version), and the
    # produced stream must decode gaplessly through libmusepack. Inspects the
    # actual encoded file, not source constants.
    add_test(NAME sv8_format
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_sv8.sh
                     $<TARGET_FILE:mpcenc>
                     $<TARGET_FILE:mpcdec_cmd>
                     ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/wav16-44k.wav)

    # End-to-end toolchain integration.
    add_test(NAME integration
             COMMAND ${CMAKE_COMMAND} -E env
                     MPC_PSY_SIMD_ENABLED=$<BOOL:${MPC_PSY_SIMD_ENABLED}>
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_integration.sh
                     $<TARGET_FILE:mpcenc>
                     $<TARGET_FILE:mpcdec_cmd>
                     $<TARGET_FILE:mpccut>
                     $<TARGET_FILE:wavcmp>)

    # Decoder robustness on truncated/corrupted input.
    set(_fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sine44-q5.mpc)
    add_test(NAME fuzz
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_fuzz.sh
                     $<TARGET_FILE:mpcdec_cmd>
                     ${_fixture})

    # .mpack integration: reference packages, negative cases, JSON schema.
    add_test(NAME mpack_integration
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_mpack.sh
                     $<TARGET_FILE:musicpack_cmd>
                     $<TARGET_FILE:mpc_mpack_tests>)

    # MusicPack Author backend: the CLI draft commands (inspect /
    # validate-draft / build-draft / identify-draft) that power the desktop
    # GUI, end to end against the reference tag fixtures.
    add_test(NAME author_backend
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_author.sh
                     $<TARGET_FILE:musicpack_cmd>)

    # MusicPack Author encode stage: encode-draft turns tagged FLAC/WAV albums
    # into staged Musepack SV8 via the native decoder (no ffmpeg) and
    # build-draft assembles a valid .mpack from it.
    add_test(NAME author_encode
             COMMAND ${CMAKE_COMMAND} -E env
                     MPCENC=$<TARGET_FILE:mpcenc>
                     APE_DUMP=$<TARGET_FILE:mpc_ape_tag_dump>
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_encode.sh
                     $<TARGET_FILE:musicpack_cmd>)

    # Standalone .app packaging audit (skips when the app is not built; the
    # authoritative gate runs inside scripts/build-author-macos.sh).
    add_test(NAME author_app_audit
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_author_app_audit.sh)

    # .mpack fuzz-lite: truncated/flipped manifests and path injection.
    add_test(NAME mpack_fuzz
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_mpack_fuzz.sh
                     $<TARGET_FILE:musicpack_cmd>
                     ${CMAKE_CURRENT_SOURCE_DIR}/reference/test-musicpack-album.mpack)

    # .mpack hostile-object regression: special filesystem objects (FIFO,
    # directory, symlink escape) must fail quickly instead of blocking.
    add_test(NAME mpack_hostile
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_mpack_hostile.sh
                     $<TARGET_FILE:musicpack_cmd>)
    set_tests_properties(mpack_hostile PROPERTIES TIMEOUT 60)

    # Phase 3A meta fuzz-lite: truncated/flipped FLAC metadata files parsed
    # through the libmusicpack tag readers (no crash = pass).
    add_test(NAME meta_fuzz
             COMMAND ${CMAKE_COMMAND} -E env
                     ${CMAKE_CURRENT_SOURCE_DIR}/run_meta_fuzz.sh
                     $<TARGET_FILE:mpc_mpack_tests>
                     ${CMAKE_CURRENT_SOURCE_DIR}/reference/meta)

    # Phase 4: musicpack-server HTTP API + direct streaming (needs the server
    # executable, which is only built when libmicrohttpd is available).
    if(TARGET musicpack_server_cmd)
        add_test(NAME server_integration
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_server.sh
                         $<TARGET_FILE:musicpack_server_cmd>)
    endif()
endif()

# ---- installed package consumer -------------------------------------------
add_test(NAME install_consumer
         COMMAND ${CMAKE_COMMAND}
                 -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
                 -DBUILD_DIR=${PROJECT_BINARY_DIR}
                 -DINSTALL_STAGE=${CMAKE_CURRENT_BINARY_DIR}/install-consumer-stage
                 -DSTATIC_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-consumer-static-producer
                 -DSTATIC_INSTALL_STAGE=${CMAKE_CURRENT_BINARY_DIR}/install-consumer-static-stage
                 -DINSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
                 -DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
                 -DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install_consumer
                 -DCONSUMER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-consumer-build
                 -DCONFIG=$<CONFIG>
                 -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
                 -DBUILD_SHARED=${MPC_BUILD_SHARED}
                 -DC_COMPILER=${CMAKE_C_COMPILER}
                 -DGENERATOR=${CMAKE_GENERATOR}
                 -DIS_WINDOWS=${WIN32}
                 -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_install_consumer.cmake)

# ---- Bit-identity with the pristine reference encoder ----------------------
# Encodes the deterministic corpus and compares SHA-256 against either a
# reference encoder passed via REF_MPCENC (live comparison, used by CI so the
# check is toolchain-agnostic) or the committed -O0 manifest
# (tests/reference_manifest.txt). Runs on all platforms; Windows invokes the
# script through Git Bash.
if(UNIX)
    add_test(NAME compat
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_compat.sh
                     $<TARGET_FILE:mpcenc>)
elseif(WIN32)
    find_program(MPC_GIT_BASH bash
                 HINTS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin")
    if(MPC_GIT_BASH)
        add_test(NAME compat
                 COMMAND ${MPC_GIT_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/run_compat.sh
                         $<TARGET_FILE:mpcenc>)
    endif()
endif()
if(TEST compat)
    set_tests_properties(compat PROPERTIES
        ENVIRONMENT "MPC_COMPAT_REQUIRE_REFERENCE=1"
        SKIP_RETURN_CODE 77)
endif()

# ---- Phase 2 bit-exact encoder regression (extended corpus, q5/q6/q7) -----
# Two modes: live REF_MPCENC comparison on CI (the authoritative per-platform
# gate, run in the ci.yml compat step) and a committed -O0 manifest for local
# use. IMPORTANT ground-truth: the scalar encoder is NOT cross-platform
# byte-identical today (verified: arm64 clang -O0 282/282 vs the frozen
# manifest, x86-64 gcc -O0 only 28/282) because of FP codegen + libm in the
# init tables. The manifest therefore only matches the platform/compiler/
# optimization it was frozen for (arm64 clang -O0). CI excludes this test
# from the generic run and gates it live-mode instead.
if(UNIX)
    add_test(NAME enc_compat
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_enc_compat.sh
                     $<TARGET_FILE:mpcenc>)
elseif(WIN32)
    if(MPC_GIT_BASH)
        add_test(NAME enc_compat
                 COMMAND ${MPC_GIT_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/run_enc_compat.sh
                         $<TARGET_FILE:mpcenc>)
    endif()
endif()
if(TEST enc_compat)
    set_tests_properties(enc_compat PROPERTIES
        ENVIRONMENT "MPC_COMPAT_REQUIRE_REFERENCE=1"
        SKIP_RETURN_CODE 77)
endif()

# ---- encoder scalar-vs-SIMD differential test ----------------------------
# White-box: forces the analysis filterbank scalar and SIMD paths and asserts
# bit-identical subband output over several frames (with state evolution).
# Links mpcenc_static + mpcpsy (libm). Only meaningful when the encoder SIMD
# kernel is compiled in (MPC_ENABLE_SIMD on aarch64/x86-64); otherwise the
# test fails closed.
add_executable(mpc_enc_synth_ab enc_synth_ab.c)
target_link_libraries(mpc_enc_synth_ab PRIVATE mpcenc_static mpcpsy)
target_include_directories(mpc_enc_synth_ab PRIVATE
    ${PROJECT_SOURCE_DIR}/libmpcenc
    ${PROJECT_SOURCE_DIR}/libmpcpsy)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_enc_synth_ab PRIVATE m)
endif()

if(MPC_ENCODER_SIMD_ENABLED)
  add_test(NAME enc_ab COMMAND mpc_enc_synth_ab)
endif()

# ---- psychoacoustic scalar-vs-SIMD differential test ----------------------
# White-box: forces the psychoacoustic spectrum/FFT kernels (and the full
# model) scalar vs SIMD and asserts bit-identical outputs, reporting the first
# divergence. Links mpcpsy (libm). Only meaningful with MPC_ENABLE_SIMD;
# otherwise the test fails closed.
add_executable(mpc_psy_ab psy_ab.c)
target_link_libraries(mpc_psy_ab PRIVATE mpcpsy)
target_include_directories(mpc_psy_ab PRIVATE ${PROJECT_SOURCE_DIR}/libmpcpsy)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_psy_ab PRIVATE m)
endif()

if(MPC_PSY_SIMD_ENABLED)
  add_test(NAME psy_ab COMMAND mpc_psy_ab)
endif()
