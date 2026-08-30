# mpakhttp: optional MPAK HTTP Range transport adapter.
#
# Embedding-layer component (specs/mpak-http-range-design.md): turns an
# HTTP(S) URL into a transport-neutral musicpack_range_source for
# musicpack_package_open_range(). Lives OUTSIDE libmusicpack — the core
# library performs no networking and gains no dependency from this
# component.
#
# Uses libcurl as the HTTP client. Built only when libcurl is available
# (pkg-config), mirroring how musicpack-server is gated on
# libmicrohttpd; the normal build and all existing tests work without
# it.

option(MPC_BUILD_MPAKHTTP
    "Build the MPAK HTTP range transport adapter (needs libcurl)" ON)

if(NOT MPC_BUILD_MPAKHTTP)
  return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(CURL QUIET IMPORTED_TARGET libcurl)
endif()

if(NOT CURL_FOUND)
  if(NOT WIN32)
    message(WARNING
        "libcurl not found; MPAK HTTP range adapter disabled "
        "(install libcurl development files or set MPC_BUILD_MPAKHTTP=OFF "
        "to silence this warning)")
  endif()
  return()
endif()

add_library(musicpack_mpakhttp STATIC src/http_source.c)
set_target_properties(musicpack_mpakhttp PROPERTIES
    OUTPUT_NAME musicpack_mpakhttp
    C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(musicpack_mpakhttp
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(musicpack_mpakhttp
    PUBLIC musicpack PkgConfig::CURL)
if(NOT MSVC)
  target_compile_options(musicpack_mpakhttp PRIVATE -Wall -Wextra)
endif()
