# ---------------------------------------------------------------------------
# The examples from docs/library/, as buildable programs.
#
# They exist to be compiled: a snippet in a document rots silently, and one
# that is a build target cannot. Each is also runnable and prints what it
# produced, so the documented API is exercised and not merely parsed. They link
# ac3::warnings like every other first-party target, so a documented call that
# would make a caller's build noisy fails here first.
#
# Under Emscripten this whole flat, desktop-console-program list is skipped in
# favour of wasm_decode_demo/ alone (see the bottom of this file): none of
# these 19 examples are meaningful compiled to WASM (nothing loads them in a
# browser, none is written expecting Emscripten's virtual filesystem for its
# CLI-style file arguments), and they add nothing to a roadmap-F3 configure
# that only wants the one browser-facing target.
# ---------------------------------------------------------------------------

if(NOT EMSCRIPTEN)

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
    mux_ts
    wav_roundtrip
    custom_layout
    multi_source_assignment
    scripted_object_motion
    object_signing
    level_metering
    decode_robustness
    atmos_fallback
    qc_report)

# read_adm demonstrates ac3adm::ac3adm (src/ac3adm), the standalone BW64/RF64 + ADM
# parser - "standalone" meaning zero dependency on ac3::forge, unlike every other
# example in the list above. encode_adm (roadmap B1 phase 3) is the end-to-end
# counterpart: ac3adm::parse_bw64 -> ac3::admbridge::build -> ac3::oba::AtmosEncoder, so unlike
# every other entry here (including read_adm) it needs BOTH ac3::forge and the opt-in ADM modules
# together - see this file's own DLL-copy comments below for the Windows shared-build consequence
# of that. Both are only added when AC3FORGE_BUILD_ADM turned those modules on (default OFF - see
# root CMakeLists.txt's own option() for why), so the foreach below cannot assume either is always
# present the way it can for e.g. mux_mkv.
if(AC3FORGE_BUILD_ADM)
    list(APPEND AC3FORGE_EXAMPLES read_adm encode_adm)
endif()

foreach(example IN LISTS AC3FORGE_EXAMPLES)
    add_executable(${example} "${example}.cpp")
    if(example STREQUAL "read_adm")
        # No ac3::forge here - src/ac3adm is deliberately independent of it (see
        # src/ac3adm/CMakeLists.txt's header comment), so this example only needs
        # the parser itself plus the same ac3::warnings every other example gets.
        target_link_libraries(${example} PRIVATE ac3adm::ac3adm ac3::warnings)
    elseif(example STREQUAL "encode_adm")
        # The one example (and roadmap B1's own phase-3 deliverable) needing ac3::forge
        # (AtmosEncoder) and the ADM modules (parse/bridge) at the same time.
        target_link_libraries(${example} PRIVATE ac3::forge ac3adm::ac3adm ac3::admbridge
                                                  ac3::warnings)
    else()
        target_link_libraries(${example} PRIVATE ac3::forge ac3::warnings)
    endif()
    set_target_properties(${example} PROPERTIES
        FOLDER examples
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/examples")
endforeach()

# Only these need a container writer; mux_fmp4 reuses mp4::mp4 the same way mux_mp4 does.
target_link_libraries(mux_mkv PRIVATE matroska::matroska)
target_link_libraries(mux_mp4 PRIVATE mp4::mp4)
target_link_libraries(mux_fmp4 PRIVATE mp4::mp4)
target_link_libraries(mux_ts PRIVATE mpegts::mpegts)

# Only this one needs the object signer.
target_link_libraries(object_signing PRIVATE ac3::signing)

# A shared ac3::forge/matroska::matroska/mp4::mp4/mpegts::mpegts (-DBUILD_SHARED_LIBS=ON)
# lands its .dll in ${CMAKE_BINARY_DIR}/bin, one level up from where the loop
# above puts every example - Windows' same-directory DLL search never finds
# it there, and there is no rpath-equivalent to point at a parent directory
# instead. Copy the runtime DLLs once, from mux_mkv's, mux_mp4's and mux_ts's own
# TARGET_RUNTIME_DLLS - together they're a strict superset of what every
# other example needs (mux_mkv covers ac3::forge/matroska::matroska, mux_mp4
# adds mp4::mp4, mux_ts adds mpegts::mpegts), so this single copy covers all of
# them. (Giving each example its own POST_BUILD copy into the same shared
# bin/examples/ directory was tried first and failed intermittently with
# "permission denied": ninja runs several examples' links in parallel, and
# multiple processes copying the identical destination file at once race under
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
# mux_mkv/mux_ts/ac3cli/ac3tests/etc. all just find libac3forge.so,
# libmatroska.so and libmpegts.so without any of this copying, unlike
# Windows' same-directory-only DLL search. ac3cli/ac3gui/tests don't need any
# of this either, on any platform - they already share ${CMAKE_BINARY_DIR}/bin
# with the DLLs directly (Windows), or already work via RPATH (Linux/macOS).
if(BUILD_SHARED_LIBS AND WIN32)
    add_custom_target(examples_copy_runtime_dlls
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:mux_mkv>"
            "$<TARGET_RUNTIME_DLLS:mux_mp4>"
            "$<TARGET_RUNTIME_DLLS:mux_ts>"
            "${CMAKE_BINARY_DIR}/bin/examples"
        COMMAND_EXPAND_LISTS
        # TARGET_RUNTIME_DLLS in a *custom command*'s own COMMAND line does
        # not imply a build-order dependency on the DLLs it names the way it
        # does when used inside a target's own link/compile step - without
        # this, the copy has run before ac3forge.dll exists at all ("No such
        # file or directory"). Depending on mux_mkv, mux_mp4 and mux_ts themselves is
        # sufficient: linking them already requires ac3forge_shared/
        # matroska_shared/mp4_shared/mpegts_shared to exist first.
        DEPENDS mux_mkv mux_mp4 mux_ts)
    # Every other example also depends on the copy - not mux_mkv/mux_mp4/mux_ts
    # themselves, which would make any of them depend on
    # examples_copy_runtime_dlls depend on mux_mkv/mux_mp4/mux_ts depend on
    # examples_copy_runtime_dlls, a cycle. Whether their own .exe existing
    # waits on the copy or not doesn't matter: all are ordinary members of the
    # default ALL target, so a plain `cmake --build .` builds everything
    # before ctest ever runs anything either way.
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        if(NOT example STREQUAL "mux_mkv" AND NOT example STREQUAL "mux_mp4"
                AND NOT example STREQUAL "mux_ts" AND NOT example STREQUAL "read_adm"
                AND NOT example STREQUAL "encode_adm")
            add_dependencies(${example} examples_copy_runtime_dlls)
        endif()
    endforeach()
endif()

# read_adm and encode_adm need their own copy step, separate from the one above: read_adm links
# ac3adm::ac3adm only, and encode_adm links ac3::forge ALONGSIDE ac3adm::ac3adm/ac3::admbridge (see
# examples/CMakeLists.txt's own foreach above) - neither combination is the
# ac3::forge/matroska::matroska/mp4::mp4/mpegts::mpegts set mux_mkv/mux_mp4/mux_ts's own
# TARGET_RUNTIME_DLLS cover, and both only exist at all when AC3FORGE_BUILD_ADM is ON (see the
# AC3FORGE_EXAMPLES list above) - folding either into the block above would make that block depend
# on a target that may not exist. Combined into one target rather than two separate ones (the same
# "list every DLL source once, copy together" shape the block above already uses for
# mux_mkv/mux_mp4/mux_ts): TARGET_RUNTIME_DLLS resolves each target's own full transitive DLL
# closure regardless of which libraries it links, so listing both here needs no reasoning about
# whether one target's DLLs are a superset of the other's.
#
# Neither read_adm nor encode_adm is made to depend on this copy target - each is among its own
# consumers, so doing so would make it depend on a target that itself DEPENDS on it, a cycle. As
# with mux_mkv/mux_mp4/mux_ts above, whether either .exe existing waits on the copy or not doesn't
# matter: all are ordinary members of the default ALL target, so a plain `cmake --build .` builds
# everything before ctest ever runs anything either way.
if(BUILD_SHARED_LIBS AND WIN32 AND AC3FORGE_BUILD_ADM)
    add_custom_target(examples_copy_adm_runtime_dlls
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:read_adm>"
            "$<TARGET_RUNTIME_DLLS:encode_adm>"
            "${CMAKE_BINARY_DIR}/bin/examples"
        COMMAND_EXPAND_LISTS
        DEPENDS read_adm encode_adm)
endif()

# Run them under ctest when tests are on, so "the examples still work" is
# checked by the same command as everything else.
if(AC3FORGE_BUILD_TESTS)
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        add_test(NAME "example.${example}" COMMAND ${example})
    endforeach()
endif()

else()  # EMSCRIPTEN

# The browser decode demo (roadmap F3). Its own CMakeLists.txt produces a
# .wasm/.js/.html triple via Embind rather than a ctest-registered native
# executable, so it does not belong in AC3FORGE_EXAMPLES above - nothing here
# can run it as a native process, and AC3FORGE_BUILD_TESTS is always OFF on
# the wasm-emscripten preset anyway (see CMakePresets.json).
add_subdirectory(wasm_decode_demo)

endif()  # EMSCRIPTEN
