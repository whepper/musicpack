# Decoder benchmark infrastructure. Only built/registered when
# MPC_BUILD_TESTS is on; the bench test is excluded from the default CTest
# suites (it is a measurement tool, not a pass/fail check).
if(NOT MPC_BUILD_TESTS)
  return()
endif()

if(MPC_BUILD_SHARED)
  set(MPC_WHITEBOX_LIB musepack_static)
else()
  set(MPC_WHITEBOX_LIB musepack)
endif()

add_executable(mpc_bench decode_bench.c)
set_target_properties(mpc_bench PROPERTIES OUTPUT_NAME decode_bench)
target_link_libraries(mpc_bench PRIVATE ${MPC_WHITEBOX_LIB})
target_include_directories(mpc_bench PRIVATE ${PROJECT_SOURCE_DIR}/codec/libmpcdec)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_bench PRIVATE m)
endif()

# Informational only; run the real benchmark via bench/run_bench.sh.
add_test(NAME bench
         COMMAND ${CMAKE_COMMAND} -E echo
                 "bench: use bench/run_bench.sh with $<TARGET_FILE:mpc_bench>")
set_tests_properties(bench PROPERTIES RUN_SERIAL TRUE)

add_executable(mpc_psy_fft_bench psy_fft_bench.c)
set_target_properties(mpc_psy_fft_bench PROPERTIES OUTPUT_NAME psy_fft_bench)
target_link_libraries(mpc_psy_fft_bench PRIVATE mpcpsy)
target_include_directories(mpc_psy_fft_bench PRIVATE ${PROJECT_SOURCE_DIR}/codec/libmpcpsy)
if(UNIX AND NOT APPLE)
  target_link_libraries(mpc_psy_fft_bench PRIVATE m)
endif()
