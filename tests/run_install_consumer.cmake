# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause

function(install_packages producer_build install_stage output_prefix)
  file(REMOVE_RECURSE "${install_stage}")
  if(IS_WINDOWS)
    set(installed_prefix "${install_stage}")
  else()
    set(installed_prefix "${install_stage}${INSTALL_PREFIX}")
  endif()

  foreach(install_subdir codec/include codec/libmpcdec core/libmusicpack)
    if(IS_WINDOWS)
      set(install_command "${CMAKE_COMMAND}"
          "-DCMAKE_INSTALL_PREFIX=${install_stage}")
    else()
      set(install_command "${CMAKE_COMMAND}" -E env "DESTDIR=${install_stage}"
          "${CMAKE_COMMAND}")
    endif()
    if(CONFIG)
      list(APPEND install_command "-DCMAKE_INSTALL_CONFIG_NAME=${CONFIG}")
    endif()
    list(APPEND install_command -P
        "${producer_build}/${install_subdir}/cmake_install.cmake")

    execute_process(COMMAND ${install_command} RESULT_VARIABLE result)
    if(result)
      message(FATAL_ERROR "${install_subdir} install failed: ${result}")
    endif()
  endforeach()
  set(${output_prefix} "${installed_prefix}" PARENT_SCOPE)
endfunction()

function(build_and_run_consumer installed_prefix install_stage consumer_build test_pkg_config)
  file(REMOVE_RECURSE "${consumer_build}")
  if(test_pkg_config)
    set(ENV{PKG_CONFIG_PATH} "${installed_prefix}/${INSTALL_LIBDIR}/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${install_stage}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE_DIR}"
            -B "${consumer_build}"
            "-DCMAKE_PREFIX_PATH=${installed_prefix}"
            -DCMAKE_SKIP_RPATH=ON
            "-DTEST_PKG_CONFIG=${test_pkg_config}"
    RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "Installed consumer configure failed: ${result}")
  endif()

  set(build_command "${CMAKE_COMMAND}" --build "${consumer_build}")
  if(CONFIG)
    list(APPEND build_command --config "${CONFIG}")
  endif()
  execute_process(COMMAND ${build_command} RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "Installed consumer build failed: ${result}")
  endif()

  set(old_path "$ENV{PATH}")
  set(old_ld_library_path "$ENV{LD_LIBRARY_PATH}")
  set(old_dyld_library_path "$ENV{DYLD_LIBRARY_PATH}")
  if(IS_WINDOWS)
    set(ENV{PATH} "${installed_prefix}/bin;${old_path}")
  else()
    set(ENV{LD_LIBRARY_PATH}
        "${installed_prefix}/${INSTALL_LIBDIR}:${old_ld_library_path}")
    set(ENV{DYLD_LIBRARY_PATH}
        "${installed_prefix}/${INSTALL_LIBDIR}:${old_dyld_library_path}")
  endif()

  set(test_command "${CTEST_COMMAND}" --test-dir "${consumer_build}"
      --output-on-failure)
  if(CONFIG)
    list(APPEND test_command -C "${CONFIG}")
  endif()
  execute_process(COMMAND ${test_command} RESULT_VARIABLE result)

  set(ENV{PATH} "${old_path}")
  set(ENV{LD_LIBRARY_PATH} "${old_ld_library_path}")
  set(ENV{DYLD_LIBRARY_PATH} "${old_dyld_library_path}")
  if(result)
    message(FATAL_ERROR "Installed consumer execution failed: ${result}")
  endif()
endfunction()

find_program(PKG_CONFIG_EXECUTABLE pkg-config)
if(PKG_CONFIG_EXECUTABLE AND NOT IS_WINDOWS)
  set(test_pkg_config ON)
else()
  set(test_pkg_config OFF)
endif()

install_packages("${BUILD_DIR}" "${INSTALL_STAGE}" installed_prefix)
build_and_run_consumer("${installed_prefix}" "${INSTALL_STAGE}"
    "${CONSUMER_BUILD_DIR}" "${test_pkg_config}")

# A shared install cannot prove that pkg-config's static dependency expansion
# actually links archives. Build a focused archive-only producer for that arm.
if(BUILD_SHARED AND test_pkg_config)
  file(REMOVE_RECURSE "${STATIC_BUILD_DIR}" "${STATIC_INSTALL_STAGE}")
  set(configure_command "${CMAKE_COMMAND}" -S "${SOURCE_DIR}"
      -B "${STATIC_BUILD_DIR}" -G "${GENERATOR}"
      -DMPC_BUILD_SHARED=OFF
      -DMPC_BUILD_TESTS=OFF
      -DMPC_BUILD_MPCGAIN=OFF
      -DMPC_BUILD_MPCCHAP=OFF
      -DMPC_BUILD_SERVER=OFF
      "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
      "-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}")
  if(C_COMPILER)
    list(APPEND configure_command "-DCMAKE_C_COMPILER=${C_COMPILER}")
  endif()
  execute_process(COMMAND ${configure_command} RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "Static producer configure failed: ${result}")
  endif()

  set(static_build_command "${CMAKE_COMMAND}" --build "${STATIC_BUILD_DIR}"
      --target musepack musicpack)
  if(CONFIG)
    list(APPEND static_build_command --config "${CONFIG}")
  endif()
  execute_process(COMMAND ${static_build_command} RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "Static producer build failed: ${result}")
  endif()

  install_packages("${STATIC_BUILD_DIR}" "${STATIC_INSTALL_STAGE}"
      static_installed_prefix)
  build_and_run_consumer("${static_installed_prefix}" "${STATIC_INSTALL_STAGE}"
      "${CONSUMER_BUILD_DIR}-static" ON)
endif()
