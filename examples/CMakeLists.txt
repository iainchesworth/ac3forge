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
# together. Both are only added when AC3FORGE_BUILD_ADM turned those modules on (default OFF - see
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
    set_target_properties(${example} PROPERTIES FOLDER examples)
endforeach()

# Only these need a container writer; mux_fmp4 reuses mp4::mp4 the same way mux_mp4 does.
target_link_libraries(mux_mkv PRIVATE matroska::matroska)
target_link_libraries(mux_mp4 PRIVATE mp4::mp4)
target_link_libraries(mux_fmp4 PRIVATE mp4::mp4)
target_link_libraries(mux_ts PRIVATE mpegts::mpegts)

# Only this one needs the object signer.
target_link_libraries(object_signing PRIVATE ac3::signing)

# capi_encode_decode (roadmap F1) is a plain C source, not C++ - the .c extension is what actually
# proves ac3forge_c/ac3forge.h compiles under a C toolchain rather than merely under a C++ one
# parsing valid-C syntax, so it cannot join the foreach()/AC3FORGE_EXAMPLES list above (that loop
# hardcodes add_executable(${example} "${example}.cpp")) and gets its own block instead. It links
# ac3::forge_c only, never ac3::forge - the whole point of the C API is that a consumer needs
# nothing else.
if(AC3FORGE_BUILD_CAPI)
    # The only C (not C++) source anywhere in this tree - the project() call in the root
    # CMakeLists.txt only declares CXX, so the C compiler has to be brought in explicitly before
    # CMake will generate a compile rule for a .c file at all.
    enable_language(C)
    add_executable(capi_encode_decode capi_encode_decode.c)
    target_link_libraries(capi_encode_decode PRIVATE ac3::forge_c ac3::warnings)
    set_target_properties(capi_encode_decode PROPERTIES FOLDER examples)

    if(AC3FORGE_BUILD_TESTS)
        add_test(NAME "example.capi_encode_decode" COMMAND capi_encode_decode)
    endif()
endif()

# Run them under ctest when tests are on, so "the examples still work" is
# checked by the same command as everything else.
if(AC3FORGE_BUILD_TESTS)
    foreach(example IN LISTS AC3FORGE_EXAMPLES)
        add_test(NAME "example.${example}" COMMAND ${example})
    endforeach()
endif()
