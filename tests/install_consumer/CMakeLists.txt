# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)
project(musicpack_install_consumer LANGUAGES C)

enable_testing()

find_package(MusicPack CONFIG REQUIRED)

add_executable(cmake_package_consumer consumer.c)
target_link_libraries(cmake_package_consumer PRIVATE MusicPack::Package)
add_test(NAME cmake_package_consumer_run COMMAND cmake_package_consumer)

if(TEST_PKG_CONFIG)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(INSTALLED_MUSICPACK REQUIRED musicpack)
  add_executable(pkg_config_consumer consumer.c)
  target_compile_options(pkg_config_consumer PRIVATE
      ${INSTALLED_MUSICPACK_STATIC_CFLAGS_OTHER})
  target_include_directories(pkg_config_consumer PRIVATE
      ${INSTALLED_MUSICPACK_STATIC_INCLUDE_DIRS})
  target_link_libraries(pkg_config_consumer PRIVATE
      ${INSTALLED_MUSICPACK_STATIC_LDFLAGS})
  add_test(NAME pkg_config_consumer_run COMMAND pkg_config_consumer)
endif()
