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
# ac3kernelbench - per-kernel micro-benchmarks, one level below ac3bench's
# whole-frame numbers. Where ac3bench answers "how fast is a frame", this
# answers "which stage of that frame" - the observability slice of the
# performance programme these three both belong to (Tracy zones give the
# real-time breakdown, this gives an offline one that needs no profiler
# attached). Not ctest-registered, same reasoning as ac3bench above: nothing
# here asserts anything, and its JSON is for local/manual trend-watching
# rather than a CI gate.
# ---------------------------------------------------------------------------
add_executable(ac3kernelbench kernel_bench.cpp)

target_link_libraries(ac3kernelbench PRIVATE ac3::forge ac3::warnings)

# So the binary can find tests/golden/audio's real-audio fixtures regardless
# of the working directory it is launched from - kernel inputs synthesized
# from silence or a single tone give misleadingly tidy bit-allocation/
# mantissa-cost numbers real program material never produces.
target_compile_definitions(ac3kernelbench PRIVATE
    "AC3FORGE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
