# ---------------------------------------------------------------------------
# Sanitizers.cmake
#
# AC3FORGE_SANITIZERS is a comma-separated -fsanitize= value (e.g.
# "address,undefined") applied to every target via global compile and link
# flags. Empty (the default) compiles nothing differently. Clang/GCC only -
# MSVC's /fsanitize=address has no UBSan equivalent, so a sanitizer build
# belongs on the Linux presets (see CMakePresets.json's *-asan-ubsan legs),
# not bolted onto the MSVC ones.
#
# -fno-sanitize-recover=all is deliberate: a sanitizer build exists to fail
# the build/test run on the first violation, not to log and keep going. A
# continue-on-violation run defeats the point of a CI gate.
# ---------------------------------------------------------------------------

if(NOT AC3FORGE_SANITIZERS)
    return()
endif()

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
        CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "AC3FORGE_SANITIZERS='${AC3FORGE_SANITIZERS}' needs Clang or GCC; "
        "the active compiler is ${CMAKE_CXX_COMPILER_ID}.")
endif()

set(_AC3_SANITIZE_FLAGS
    "-fsanitize=${AC3FORGE_SANITIZERS}"
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer)

add_compile_options(${_AC3_SANITIZE_FLAGS})
add_link_options(${_AC3_SANITIZE_FLAGS})

message(STATUS "Sanitizers enabled: ${AC3FORGE_SANITIZERS}")
unset(_AC3_SANITIZE_FLAGS)
