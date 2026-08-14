# ---------------------------------------------------------------------------
# ac3perf - real-time throughput regression guards (hard pass/fail). See
# test_performance.cpp for why this is a separate target from ac3tests, and
# why the whole thing exists at all (a ~28x, zero-test-coverage regression in
# the forward MDCT, found profiling the Shield Android app with Tracy).
#
# Catch2 is already resolved by the parent tests/CMakeLists.txt before this
# subdirectory is added, so this file only defines the target.
# ---------------------------------------------------------------------------
add_executable(ac3perf test_performance.cpp)

target_link_libraries(ac3perf
    PRIVATE
        ac3::forge
        ac3::warnings
        Catch2::Catch2WithMain)

# TEST_PREFIX so `ctest -R '^Performance::'`/`-E` can target this target's
# cases specifically without a fragile name-based guess, and
# `-L Performance` for the same reason via PROPERTIES below - see
# CMakePresets.json's test-linux-llvm-asan-ubsan preset, which excludes this
# exact label (an instrumented build's timing is not a signal worth having
# an opinion about, at any slack factor).
catch_discover_tests(ac3perf
    TEST_PREFIX "Performance::"
    PROPERTIES LABELS "Performance")

# ---------------------------------------------------------------------------
# ac3bench - the trend-tracking half (numbers, not a threshold). Not a
# ctest-registered test at all: see bench_encoder.cpp's own header comment
# and docs/performance-trend.md for how its --json-out output feeds
# scripts/append-performance-history.py, the same relationship
# scripts/verify-gold-reference.sh's --json-out has to
# append-quality-history.py.
# ---------------------------------------------------------------------------
add_executable(ac3bench bench_encoder.cpp)

target_link_libraries(ac3bench PRIVATE ac3::forge ac3::warnings)

# ---------------------------------------------------------------------------
# ac3kernelbench - per-kernel micro-benchmarks (Phase 2 of the performance-
# observability programme this trio of targets belongs to): the individual
# DSP/bitstream kernels Tracy's frame-level zones name, timed in isolation
# rather than as part of a whole encode_frame() call. Same non-gating,
# --json-out {name, iters, ns_per_call} shape as ac3bench above - see
# bench_kernels.cpp's own header comment.
# ---------------------------------------------------------------------------
add_executable(ac3kernelbench bench_kernels.cpp)

target_link_libraries(ac3kernelbench PRIVATE ac3::forge ac3::warnings)
