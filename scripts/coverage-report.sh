#!/usr/bin/env bash
#
# Library coverage report + per-component statement/branch gate.
#
# One gcov extraction pass over an AC3FORGE_ENABLE_COVERAGE build (the
# config-linux-gcc-coverage preset - see CMakePresets.json), then one cheap
# gate pass per library component off the shared JSON trace. Line and branch
# coverage are gated PER COMPONENT rather than as one blended number:
# src/forge is an order of magnitude larger than any container writer, so a
# blend would let a real regression in src/mpegts or src/capi hide inside
# ordinary drift in src/forge - and "which module is thin" is exactly the
# question a per-component table exists to answer.
#
# Run by .github/workflows/ci.yml's coverage job after `ctest`; runnable
# locally the same way, from the repository root (see docs/building.md):
#
#   cmake --preset config-linux-gcc-coverage
#   cmake --build --preset build-linux-gcc-coverage -- -k 0
#   ctest --preset test-linux-gcc-coverage -LE Performance
#   ./scripts/coverage-report.sh -g gcov-15
#
# Thresholds sit a few points under each component's measured baseline (the
# table below records the measurement each floor was set against) so ordinary
# in-flight churn does not trip the gate while a real regression still fails
# the job. Raise them as the suite grows rather than leaving the headroom in
# place indefinitely - see ci.yml's coverage job comment for the calibration
# history and why hosted-runner numbers are the calibration authority.
#
# Usage:  ./scripts/coverage-report.sh [-b <build-dir>] [-g <gcov-executable>]
# Exit:   0 = every gate met, 1 = at least one gate missed or a component had
#         no coverage data at all. Every component is reported before the
#         failure exit, so the log always shows the whole table rather than
#         just the first miss.

set -euo pipefail

build_dir="build/config-linux-gcc-coverage"
gcov_exe="gcov"
while getopts "b:g:" opt; do
    case "$opt" in
        b) build_dir="$OPTARG" ;;
        g) gcov_exe="$OPTARG" ;;
        *) echo "Usage: $0 [-b <build-dir>] [-g <gcov-executable>]" >&2; exit 2 ;;
    esac
done

if [ ! -f CMakePresets.json ]; then
    echo "::error::coverage: run this from the repository root (CMakePresets.json not found)" >&2
    exit 2
fi

# Component floors, one row per library component: <dir under src/> <line%> <branch%>.
#
# Calibrated 2026-08-20 against a WSL2 run on the CI toolchain pins (gcc/gcov
# 15.2.0, gcovr 8.6), measured per component as:
#
#   forge 92.0/83.7 audio 31.2/19.9   signing 86.9/61.4   matroska 93.3/91.9
#   mp4 95.0/90.2   mpegts 93.5/92.5  capi 48.4/27.1      ac3adm 87.2/82.6
#   admbridge 90.3/85.0               (aggregate 85.2/75.1)
#
# Each floor sits ~4-8 points under its measurement: a couple of points for
# the known WSL-reads-higher-than-hosted effect (see ci.yml's coverage job
# comment), the rest as ordinary in-flight-churn headroom. Re-check against
# the first hosted run and tighten if the margin proves generous.
#
# src/audio's and src/capi's floors are low because their MEASUREMENTS are
# low, deliberately not rounded up to look respectable: no test opens an
# audio device, so the ALSA capture/monitor/passthrough device paths (the
# bulk of src/audio's lines) never execute headless - only the device-naming/
# format logic does - and test_capi.cpp exercises the AC-3 encoder/decoder/
# Atmos surface but barely touches the E-AC-3 half (src/capi/src/eac3.cpp
# measured 31% line). These floors hold the line while that is true; raising
# them is a matter of writing the missing tests, not of editing this table.
components="
forge      88 78
audio      25 15
signing    82 55
matroska   88 85
mp4        90 85
mpegts     88 85
capi       42 22
ac3adm     82 75
admbridge  85 78
"

json="$build_dir/coverage.json"
html="$build_dir/coverage.html"

# The one expensive pass: run gcov over every object file and keep the result
# as a JSON trace the per-component gates below re-read, so N gates don't
# mean N re-extractions. Also writes the human-readable HTML report ci.yml
# uploads as its artifact (both outputs live in $build_dir so -b moves them
# together with the objects they describe; --html-self-contained so the
# uploaded pages carry their own CSS/JS instead of needing sidecar files the
# artifact glob would have to chase), and prints the whole-library summary.
#
# --gcov-ignore-parse-errors=suspicious_hits.warn: mdct.cpp's
# ForwardCosTable-driven hot loop (src/core/mdct.cpp) trips a documented gcov
# bug (gcc.gnu.org/bugzilla#68080, a false "suspicious hit value" on a tight
# accumulation loop) that otherwise aborts gcovr outright rather than just
# under/over-reporting that one line's count - gcovr's own error message
# names this exact flag as the fix. Warn, not skip, so a genuinely new
# suspicious-hit line elsewhere still shows up in the log instead of
# vanishing silently.
gcovr --root . \
    --filter 'src/(forge|audio|signing|matroska|mp4|mpegts|capi|ac3adm|admbridge)/.*' \
    --gcov-executable "$gcov_exe" \
    --exclude-throw-branches --exclude-unreachable-branches \
    --gcov-ignore-errors=no_working_dir_found \
    --gcov-ignore-parse-errors=suspicious_hits.warn \
    --object-directory "$build_dir" \
    --json "$json" --html-details "$html" --html-self-contained --print-summary

fail=0
while read -r comp line_min branch_min; do
    [ -n "$comp" ] || continue

    # A component with zero files in the trace is a broken measurement (built
    # without instrumentation, or not built at all - e.g. a coverage preset
    # that lost AC3FORGE_BUILD_ADM=ON), not a 0%-covered component. Fail
    # loudly rather than letting a silent no-data "pass" or a misleading 0%
    # stand in for the real answer.
    if ! grep -q "src/$comp/" "$json"; then
        echo "::error::coverage: no data for src/$comp - was it built with AC3FORGE_ENABLE_COVERAGE on?"
        fail=1
        continue
    fi

    echo
    echo "== src/$comp (gate: line >= $line_min%, branch >= $branch_min%) =="
    if ! gcovr --root . --add-tracefile "$json" --filter "src/$comp/.*" \
        --print-summary \
        --fail-under-line "$line_min" --fail-under-branch "$branch_min"; then
        echo "::error::coverage gate missed for src/$comp (need line >= $line_min%, branch >= $branch_min%)"
        fail=1
    fi
done <<EOF
$components
EOF

exit "$fail"
