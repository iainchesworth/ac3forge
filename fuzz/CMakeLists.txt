# ---------------------------------------------------------------------------
# libFuzzer harnesses over ac3::forge's untrusted-input entry points.
#
# libFuzzer (-fsanitize=fuzzer) is an LLVM built-in, unavailable under GCC or
# MSVC, so this whole directory requires upstream Clang - not clang-cl, whose
# libFuzzer support this project has never exercised. Configure through
# fuzz/run.sh rather than by hand; it sets up a dedicated build/fuzz directory
# with the right compiler and none of AC3FORGE_BUILD_CLI/GUI/TESTS, which this
# needs neither of and which would otherwise drag in vcpkg/Qt/Catch2 for
# nothing this directory uses.
# ---------------------------------------------------------------------------

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "AC3FORGE_BUILD_FUZZERS needs upstream Clang: libFuzzer (-fsanitize=fuzzer) is an "
        "LLVM built-in, not available under GCC or MSVC. Configure with "
        "-DCMAKE_CXX_COMPILER=clang++, or run fuzz/run.sh, which does this for you.")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "AC3FORGE_BUILD_FUZZERS targets upstream Clang on Linux/macOS (the linux-llvm / "
        "macos-llvm toolchain), not clang-cl: this project has never verified libFuzzer "
        "under the MSVC-ABI frontend, and the fuzzing task this serves asks for Linux-or-"
        "macOS specifically.")
endif()

# ASan+UBSan+SanitizerCoverage on the LIBRARY, not only the harness main: a
# fuzzer only earns its keep against undefined behaviour (the class of bug
# 8386c8f fixed - a malformed differential exponent chain walking the
# reconstruction outside the range it assumed) if the code being mutated is
# instrumented, not just the driver calling it. fuzzer-no-link adds the
# coverage counters libFuzzer's mutation engine reads without pulling in
# libFuzzer's own main, which a library must not carry.
#
# PUBLIC so every harness executable below inherits the same compile AND link
# flags automatically through target_link_libraries(... ac3::forge). -fno-
# sanitize-recover=undefined turns a UBSan report into a process abort
# libFuzzer can catch and minimize, rather than a warning it would otherwise
# print and run straight past.
#
# Set on forge_objects, not the forge_static/forge_shared wrapper
# targets: the actual codec sources compile there (see src/lib/CMakeLists.txt),
# and PUBLIC on an OBJECT library's usage requirements still propagates
# through PUBLIC target_link_libraries(forge_static PUBLIC forge_objects)
# to whatever finally links ac3::forge - here, each fuzz harness below.
target_compile_options(forge_objects PUBLIC
    -fsanitize=address,undefined,fuzzer-no-link
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -g)
target_link_options(forge_objects PUBLIC -fsanitize=address,undefined,fuzzer-no-link)

add_custom_target(ac3forge_fuzzers)

# One executable per untrusted-input entry point. Each adds -fsanitize=fuzzer
# on top of what it already inherited from ac3::forge above: that flag links
# libFuzzer's runtime and its LLVMFuzzerTestOneInput-driven main, which must
# exist exactly once per executable and never inside the library itself.
#
# ac3::warnings is named explicitly rather than inherited: ac3forge links it
# PRIVATE, as every first-party target does, so it governs that library's own
# translation units and stops there. Without this the harness sources would be
# the one corner of the project compiling without the strict set - and a
# harness is exactly where a sloppy size_t-to-int narrowing would quietly
# reshape the input a fuzzer thinks it is passing in.
function(ac3forge_add_fuzzer name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE ac3::forge ac3::warnings)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(ac3forge_fuzzers ${name})
endfunction()

# ac3::io::scan: format-sniffing over an unidentified byte stream, before any
# decoder commits to a layout (src/lib/src/io/elementary.cpp).
ac3forge_add_fuzzer(fuzz_scan)

# split_frames + FrameDecoder::decode_frame, driven the way ac3cli's 'decode'
# drives them (src/lib/src/decoder/decoder.cpp).
ac3forge_add_fuzzer(fuzz_ac3_decode)

# split_access_units + Eac3Decoder::decode_access_unit (which calls
# decode_substream internally), driven the way ac3cli's 'decode' drives them
# for E-AC-3 (src/lib/src/decoder/eac3_decoder.cpp).
ac3forge_add_fuzzer(fuzz_eac3_decode)

# ac3::io::read_wav: a realistic input too, not only an adversarial one - a
# truncated or hand-edited WAV is exactly what a user hands the CLI
# (src/lib/src/io/wav.cpp).
ac3forge_add_fuzzer(fuzz_wav_read)
