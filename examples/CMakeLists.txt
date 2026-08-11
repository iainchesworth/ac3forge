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

# A shared ac3::forge/matroska::matroska (-DBUILD_SHARED_LIBS=ON) lands its
# .dll in ${CMAKE_BINARY_DIR}/bin, one level up from where the loop above
# puts every example - Windows' same-directory DLL search never finds it
# there, and there is no rpath-equivalent to point at a parent directory
# instead. Copy the runtime DLLs once, from mux_mkv's own TARGET_RUNTIME_DLLS
# specifically - it's the one example that links both ac3::forge and
# matroska::matroska, a strict superset of what every other example needs,
# so this single copy covers all of them. (Giving each of the 7 examples its
# own POST_BUILD copy into the same shared bin/examples/ directory was tried
# first and failed intermittently with "permission denied": ninja runs
# several examples' links in parallel, and multiple processes copying the
# identical destination file at once race under Windows' file locking. A
# single custom target every other example depends on avoids that without
# serializing the examples' own compiles against each other.)
#
# Gated on the real BUILD_SHARED_LIBS variable, not left unconditional and
# relying on TARGET_RUNTIME_DLLS expanding to nothing for a static build:
# `cmake -E copy_if_different` with zero source arguments is a hard error
# ("Usage: ..."), not the graceful no-op that would make an always-on custom
# target safe either way - confirmed by actually running a static configure
# with this unconditional, not assumed. ac3cli/ac3gui/tests don't need any
# of this regardless of BUILD_SHARED_LIBS - they already share
# ${CMAKE_BINARY_DIR}/bin with the DLLs directly.
if(BUILD_SHARED_LIBS)
    add_custom_target(examples_copy_runtime_dlls
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:mux_mkv>"
            "${CMAKE_BINARY_DIR}/bin/examples"
        COMMAND_EXPAND_LISTS
        # TARGET_RUNTIME_DLLS in a *custom command*'s own COMMAND line does
        # not imply a build-order dependency on the DLLs it names the way it
        # does when used inside a target's own link/compile step - without
        # this, the copy has run before ac3forge.dll exists at all ("No such
        # file or directory"). Depending on mux_mkv itself is sufficient:
        # linking it already requires ac3forge_shared/matroska_shared to
        # exist first.
        DEPENDS mux_mkv)
    # Every other example also depends on the copy - not mux_mkv itself,
    # which would make it depend on examples_copy_runtime_dlls depend on
    # mux_mkv depend on examples_copy_runtime_dlls, a cycle. Whether
    # mux_mkv's own .exe existing waits on the copy or not doesn't matter:
    # both are ordinary members of the default ALL target, so a plain
    # `cmake --build .` builds both before ctest ever runs anything either way.
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        if(NOT example STREQUAL "mux_mkv")
            add_dependencies(${example} examples_copy_runtime_dlls)
        endif()
    endforeach()
endif()

# Run them under ctest when tests are on, so "the examples still work" is
# checked by the same command as everything else.
if(AC3FORGE_BUILD_TESTS)
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        add_test(NAME "example.${example}" COMMAND ${example})
    endforeach()
endif()
