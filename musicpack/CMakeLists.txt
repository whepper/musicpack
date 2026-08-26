set(MUSICPACK_TOOL_SOURCES main.c draft.c tool_path.c)

# Vendored cJSON (also compiled into libmusicpack, but that copy is hidden
# when the library is built shared). Compiling it into the CLI target keeps
# the tool self-contained for both shared and static library builds.
set(MUSICPACK_TOOL_SOURCES ${MUSICPACK_TOOL_SOURCES}
    ${PROJECT_SOURCE_DIR}/libmusicpack/vendor/cjson/cJSON.c)

if(MSVC)
  list(APPEND MUSICPACK_TOOL_SOURCES ${PROJECT_SOURCE_DIR}/win32/attgetopt.c)
endif()

add_executable(musicpack_cmd ${MUSICPACK_TOOL_SOURCES})
if(MSVC)
  target_include_directories(musicpack_cmd PRIVATE ${PROJECT_SOURCE_DIR}/win32)
endif()
target_include_directories(musicpack_cmd PRIVATE
    ${PROJECT_SOURCE_DIR}/libmusicpack/vendor/cjson)
target_link_libraries(musicpack_cmd PRIVATE musicpack musepack)

# Vendored code is third-party: suppress warnings from it entirely.
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/libmusicpack/vendor/cjson/cJSON.c
    PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/W0>;$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>")

# The CLI builds filesystem paths by chained snprintf() of the form
# out_dir + "/subdir" + "/NN - <title>.<ext>". GCC's -Wformat-truncation
# assumes every %s argument can be a full MUSICPACK_PATH_MAX path and so
# concludes the fixed-size buffers could overflow; this is a known false
# positive for path joining where the inputs are already bounded by
# MUSICPACK_PATH_MAX. The buffers are sized to hold a single full path plus
# suffix, so suppress the diagnostic for the tool sources rather than inflate
# every buffer to multiples of MUSICPACK_PATH_MAX.
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/musicpack/main.c
    ${PROJECT_SOURCE_DIR}/musicpack/draft.c
    ${PROJECT_SOURCE_DIR}/musicpack/tool_path.c
    PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/W0>;$<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wno-format-truncation>")

# Historical-ish binary name "musicpack" (target is musicpack_cmd to avoid
# clashing with the musicpack library target).
set_target_properties(musicpack_cmd PROPERTIES OUTPUT_NAME musicpack)

if(NOT MSVC)
  target_link_libraries(musicpack_cmd PRIVATE m)
endif()

install(TARGETS musicpack_cmd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
