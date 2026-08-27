# musicpack-sonic: the profile-driven Sonic analysis analyzer.
#
# sonic_core (static, model-free): DSP frontend, decode, cache, job parsing.
# musicpack-sonic (executable): adds ONNX Runtime + model acquisition.
#
# The analyzer links libmusicpack (sha256, path, sonic validation, Musepack
# handoff) and libmusepack. It never reimplements Sonic semantics.

set(SONIC_CORE_SOURCES
    frontend.c
    decode.c
    cache.c
    model_output.c)

add_library(sonic_core STATIC ${SONIC_CORE_SOURCES})
target_include_directories(sonic_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${PROJECT_SOURCE_DIR}/core/libmusicpack/vendor/cjson
        ${PROJECT_SOURCE_DIR}/core/libmusicpack/vendor)
target_link_libraries(sonic_core PUBLIC musicpack)

# ---- the analyzer executable (requires ONNX Runtime) ------------------------
set(SONIC_ONNXRUNTIME_DIR "" CACHE PATH
    "Directory containing an ONNX Runtime install (include/ + lib/)")

if(MPC_BUILD_TESTS)
  add_executable(mpc_sonic_analyzer_tests tests/test_sonic.c)
  target_include_directories(mpc_sonic_analyzer_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(mpc_sonic_analyzer_tests PRIVATE sonic_core musicpack musepack)
  if(UNIX AND NOT APPLE)
    target_link_libraries(mpc_sonic_analyzer_tests PRIVATE m)
  elseif(MSVC)
    # test_sonic.c pins the large resampler reference table and currently
    # allocates it on the test function's stack. MSVC's default 1 MiB stack is
    # insufficient (the two double arrays alone are several MiB), while the
    # same test is below the default Linux stack limit. Give this test an
    # appropriately sized Windows stack rather than making the CTest result
    # depend on the platform's linker default.
    target_link_options(mpc_sonic_analyzer_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME sonic_analyzer COMMAND mpc_sonic_analyzer_tests
      $<TARGET_FILE:mpcenc>)
  if(WIN32)
    set_tests_properties(sonic_analyzer PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:musicpack>$<SEMICOLON>$<TARGET_FILE_DIR:mpcenc>$<SEMICOLON>$ENV{PATH}")
  endif()
endif()

if(SONIC_ONNXRUNTIME_DIR)
  add_executable(musicpack_sonic_cmd main.c model.c acquire.c
      ${PROJECT_SOURCE_DIR}/core/libmusicpack/vendor/cjson/cJSON.c)
  target_include_directories(musicpack_sonic_cmd PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/core/libmusicpack/vendor/cjson
      ${SONIC_ONNXRUNTIME_DIR}/include)
  target_link_libraries(musicpack_sonic_cmd PRIVATE sonic_core musicpack musepack)
  if(APPLE)
    target_link_libraries(musicpack_sonic_cmd PRIVATE
        ${SONIC_ONNXRUNTIME_DIR}/lib/libonnxruntime.dylib)
    target_compile_options(musicpack_sonic_cmd PRIVATE
        -Wl,-rpath,${SONIC_ONNXRUNTIME_DIR}/lib)
  elseif(UNIX)
    target_link_libraries(musicpack_sonic_cmd PRIVATE
        ${SONIC_ONNXRUNTIME_DIR}/lib/libonnxruntime.so)
  elseif(WIN32)
    target_link_libraries(musicpack_sonic_cmd PRIVATE
        ${SONIC_ONNXRUNTIME_DIR}/lib/onnxruntime.lib)
  endif()
  set_target_properties(musicpack_sonic_cmd PROPERTIES OUTPUT_NAME musicpack-sonic)
  install(TARGETS musicpack_sonic_cmd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
