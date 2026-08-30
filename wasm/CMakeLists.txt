# WebAssembly build of the libmusepack decoder wrapper.
#
# This is only included when building with the Emscripten toolchain
# (emcmake cmake -S . -B build-wasm). It compiles the same decoder core
# (the `musepack` static library) plus this thin wrapper into a single
# browser/node-usable module: musepack.js + musepack.wasm.

add_executable(musepack_wasm musepack_wasm.c mpak_wasm.c)
target_link_libraries(musepack_wasm PRIVATE musepack musicpack)
target_include_directories(musepack_wasm PRIVATE
    ${PROJECT_SOURCE_DIR}/codec/libmpcdec
    ${PROJECT_SOURCE_DIR}/core/libmusicpack/include)

set_target_properties(musepack_wasm PROPERTIES OUTPUT_NAME musepack)

# MPAK-over-HTTP exports (mpak_wasm.c): musicpack_package_open_range(),
# verification and track decode over the JS-import-backed range source.
set(MPAK_WASM_EXPORTS
    ,_mpak_wasm_open_range,_mpak_wasm_verify,_mpak_wasm_track_count,_mpak_wasm_track_open,_mpak_wasm_track_read,_mpak_wasm_track_seek_sample,_mpak_wasm_track_position,_mpak_wasm_track_sample_rate,_mpak_wasm_track_destroy,_mpak_wasm_close)

set(MPC_WASM_EXPORTS
    _mpc_wasm_create,_mpc_wasm_open,_mpc_wasm_open_range,_mpc_wasm_sample_rate,_mpc_wasm_channels,_mpc_wasm_stream_version,_mpc_wasm_length_samples,_mpc_wasm_position,_mpc_wasm_read,_mpc_wasm_seek_sample,_mpc_wasm_seek_seconds,_mpc_wasm_check,_mpc_wasm_destroy,_malloc,_free${MPAK_WASM_EXPORTS})
if(MPC_WASM_TEST_HOOKS)
  target_compile_definitions(musepack_wasm PRIVATE MPC_WASM_TEST_HOOKS)
  set(MPC_WASM_EXPORTS
      _mpc_wasm_set_synth_impl,_mpc_wasm_has_synth_simd,${MPC_WASM_EXPORTS})
endif()

# Single-threaded, no SharedArrayBuffer, no experimental extensions.
# The range reader (mpc_wasm_open_range) reads through the declared imports
# in range_library.js; the Phase 4 demo serves them synchronously from a
# buffer fetched from the server over HTTP Range, so no Asyncify is needed.
# The MPAK container path (mpak_wasm_open_range) uses the same model: the
# JS host (demo/mpakrange.js) acquires and validates the whole container
# over HTTP Range requests, then serves mpak_range_* imports synchronously
# while musicpack_package_open_range() runs unchanged over the seam.
target_link_options(musepack_wasm PRIVATE
    "-O3"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sFILESYSTEM=0"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createMusepackModule"
    "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/range_library.js"
    "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/mpak_range_library.js"
    "-sEXPORTED_FUNCTIONS=${MPC_WASM_EXPORTS}"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32"
    "-sENVIRONMENT=web,worker,node")
if(MPC_WASM_SIMD)
  target_link_options(musepack_wasm PRIVATE "-msimd128")
endif()
