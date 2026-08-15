# ---------------------------------------------------------------------------
# The examples from docs/library/, as buildable programs.
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
    station_broadcast
    mux_mkv
    mux_mp4
    mux_fmp4
    wav_roundtrip
    custom_layout
    multi_source_assignment
    scripted_object_motion
    object_signing
    level_metering
    decode_robustness
    atmos_fallback)

foreach(example IN LISTS AC3FORGE_EXAMPLES)
    add_executable(${example} "${example}.cpp")
    target_link_libraries(${example} PRIVATE ac3::forge ac3::warnings)
    set_target_properties(${example} PROPERTIES
        FOLDER examples
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/examples")
endforeach()

# Only these need a container writer.
target_link_libraries(mux_mkv PRIVATE matroska::matroska)
target_link_libraries(mux_mp4 PRIVATE mp4::mp4)
target_link_libraries(mux_fmp4 PRIVATE mp4::mp4)

# Only this one needs the object signer.
target_link_libraries(object_signing PRIVATE ac3::signing)

# A shared ac3::forge/matroska::matroska/mp4::mp4 (-DBUILD_SHARED_LIBS=ON)
# lands its .dll in ${CMAKE_BINARY_DIR}/bin, one level up from where the loop
# above puts every example - Windows' same-directory DLL search never finds
# it there, and there is no rpath-equivalent to point at a parent directory
# instead. Copy the runtime DLLs once, from mux_mkv's and mux_mp4's own
# TARGET_RUNTIME_DLLS - together they're a strict superset of what every
# other example needs (mux_mkv covers ac3::forge/matroska::matroska, mux_mp4
# adds mp4::mp4), so this single copy covers all of them. (Giving each
# example its own POST_BUILD copy into the same shared bin/examples/
# directory was tried first and failed intermittently with "permission
# denied": ninja runs several examples' links in parallel, and multiple
# processes copying the identical destination file at once race under
# Windows' file locking. A single custom target every other example depends
# on avoids that without serializing the examples' own compiles against each
# other.)
#
# Gated on BUILD_SHARED_LIBS AND WIN32, not left unconditional and relying
# on TARGET_RUNTIME_DLLS expanding to nothing when it doesn't apply:
# `cmake -E copy_if_different` with zero source arguments is a hard error
# ("Usage: ..."), not the graceful no-op that would make an always-on custom
# target safe either way - confirmed twice over by actually configuring,
# not assumed either time. A static build is the first case (nothing to
# copy at all). A shared *Linux* build is the second, found only by
# actually building config-linux-llvm-shared in WSL: TARGET_RUNTIME_DLLS is
# a Windows-only generator expression (CMake docs: "may not be used on
# other platforms") and is empty there too, for a real reason rather than
# an oversight - Linux resolves a build-tree .so via an automatically
# computed RPATH pointing straight at wherever it actually sits, so
# mux_mkv/ac3cli/ac3tests/etc. all just find libac3forge.so and
# libmatroska.so without any of this copying, unlike Windows' same-
# directory-only DLL search. ac3cli/ac3gui/tests don't need any of this
# either, on any platform - they already share ${CMAKE_BINARY_DIR}/bin
# with the DLLs directly (Windows), or already work via RPATH (Linux/macOS).
if(BUILD_SHARED_LIBS AND WIN32)
    add_custom_target(examples_copy_runtime_dlls
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:mux_mkv>"
            "$<TARGET_RUNTIME_DLLS:mux_mp4>"
            "${CMAKE_BINARY_DIR}/bin/examples"
        COMMAND_EXPAND_LISTS
        # TARGET_RUNTIME_DLLS in a *custom command*'s own COMMAND line does
        # not imply a build-order dependency on the DLLs it names the way it
        # does when used inside a target's own link/compile step - without
        # this, the copy has run before ac3forge.dll exists at all ("No such
        # file or directory"). Depending on mux_mkv/mux_mp4 themselves is
        # sufficient: linking them already requires
        # ac3forge_shared/matroska_shared/mp4_shared to exist first.
        DEPENDS mux_mkv mux_mp4)
    # Every other example also depends on the copy - not mux_mkv/mux_mp4
    # themselves, which would make them depend on
    # examples_copy_runtime_dlls depend on mux_mkv/mux_mp4 depend on
    # examples_copy_runtime_dlls, a cycle. Whether mux_mkv/mux_mp4's own
    # .exe existing waits on the copy or not doesn't matter: all are
    # ordinary members of the default ALL target, so a plain
    # `cmake --build .` builds everything before ctest ever runs anything
    # either way.
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        if(NOT example STREQUAL "mux_mkv" AND NOT example STREQUAL "mux_mp4")
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
