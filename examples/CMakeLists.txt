# ---------------------------------------------------------------------------
# The examples from docs/LIBRARY.md, as buildable programs.
#
# They exist to be compiled: a snippet in a document rots silently, and one
# that is a build target cannot. Each is also runnable and prints what it
# produced, so the documented API is exercised and not merely parsed. They link
# ac3::warnings like every other first-party target, so a documented call that
# would make a caller's build noisy fails here first.
# ---------------------------------------------------------------------------

set(AC3FORGE_EXAMPLES
    encode_ac3
    encode_eac3
    decode_stream
    metadata
    spatial_objects
    atmos_objects
    mux_mkv)

foreach(example IN LISTS AC3FORGE_EXAMPLES)
    add_executable(${example} "${example}.cpp")
    target_link_libraries(${example} PRIVATE ac3::forge ac3::warnings)
    set_target_properties(${example} PROPERTIES
        FOLDER examples
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/examples")
endforeach()

# Only this one needs the container writer.
target_link_libraries(mux_mkv PRIVATE matroska::matroska)

# Run them under ctest when tests are on, so "the examples still work" is
# checked by the same command as everything else.
if(AC3FORGE_BUILD_TESTS)
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        add_test(NAME "example.${example}" COMMAND ${example})
    endforeach()
endif()
