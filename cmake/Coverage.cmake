# ---------------------------------------------------------------------------
# Coverage.cmake
#
# Defines an INTERFACE target `ac3::coverage` that, when AC3FORGE_ENABLE_COVERAGE
# is on, turns on gcov-style source-based coverage instrumentation (GCC/Clang)
# via --coverage. Off by default: only the dedicated linux-gcc-coverage preset
# turns it on, so normal dev/CI builds pay no instrumentation cost.
#
# Link it PRIVATE into every first-party target whose coverage should be
# measured - today that is every library component (forge, audio, signing,
# matroska, mp4, mpegts, the C API, ac3adm, admbridge) plus ac3tests.
# Executables that merely LINK an instrumented library need nothing wired in:
# a PRIVATE link of this target lands in the library's INTERFACE_LINK_LIBRARIES
# as $<LINK_ONLY:ac3::coverage>, so --coverage and the gcov runtime propagate
# to every downstream link line automatically (ac3perf/ac3bench link the
# instrumented ac3::forge with no ac3::coverage of their own and link fine).
# The coverage preset still turns AC3FORGE_BUILD_CLI/EXAMPLES off, but as a
# pure build-time saving: those targets' coverage is filtered out of
# tools/checks/coverage_report.sh's report anyway, so building them instrumented
# buys nothing - see CMakePresets.json. Vendored third-party code
# (src/ac3adm's FetchContent'd libbw64/libadm) is deliberately NOT
# instrumented: these flags are target-scoped and nothing links ac3::coverage
# into those targets, and tools/checks/coverage_report.sh's filters are
# first-party-only anyway.
# ---------------------------------------------------------------------------

option(AC3FORGE_ENABLE_COVERAGE "Enable gcov/llvm-cov source coverage instrumentation" OFF)

add_library(ac3_coverage INTERFACE)
add_library(ac3::coverage ALIAS ac3_coverage)

if(AC3FORGE_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # -fno-inline keeps line/branch attribution accurate for a Debug
        # build's already-unoptimized code; --coverage covers both -fprofile-
        # arcs and -ftest-coverage plus linking the gcov runtime.
        target_compile_options(ac3_coverage INTERFACE --coverage -fno-inline)
        target_link_options(ac3_coverage INTERFACE --coverage)
    else()
        message(WARNING
            "AC3FORGE_ENABLE_COVERAGE is on but ${CMAKE_CXX_COMPILER_ID} is not "
            "GCC/Clang; coverage instrumentation is not supported on this "
            "compiler and will be skipped.")
    endif()
endif()
