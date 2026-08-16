#!/usr/bin/env bash
# fuzz/run.sh - build ac3forge's libFuzzer harnesses under Clang+ASan+UBSan and
# run each for a bounded time budget. This is deliberately NOT continuous
# fuzzing infrastructure (no OSS-Fuzz-style always-on service) - see
# .github/workflows/fuzz.yml for how CI bounds it further, and the README in
# this directory for what "bounded" means and why.
#
# Usage:
#   fuzz/run.sh                     # build, then run every default-list harness
#   fuzz/run.sh fuzz_scan            # build, then run just this harness
#   fuzz/run.sh regress              # replay every seed + regression corpus once, no mutation
#   fuzz/run.sh minimize <target> <path-to-crash-file>
#
# The differential harnesses (fuzz_differential_ac3_decode,
# fuzz_differential_eac3_decode - roadmap G3: same mutated bytes decoded by
# both ac3forge and FFmpeg, PCM diffed - see fuzz/differential_oracle.hpp)
# are NOT in the default target list `run`/`regress` use with no arguments:
# they need `ffmpeg` on PATH and are much slower per-exec, so name them
# explicitly, e.g. `fuzz/run.sh run fuzz_differential_ac3_decode`. CI's
# fuzz-differential job (fuzz.yml) does exactly this.
#
# Env overrides:
#   AC3FORGE_FUZZ_SECONDS       per-target time budget in `run` mode (default 60)
#   AC3FORGE_FUZZ_BUILD_DIR     CMake build directory (default build/fuzz)
#   AC3FORGE_FUZZ_CORPUS_DIR    grown, persistent corpus (default fuzz/corpus, gitignored)
#   AC3FORGE_FUZZ_ARTIFACT_DIR  where crashing inputs land (default fuzz/artifacts, gitignored)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${AC3FORGE_FUZZ_BUILD_DIR:-$REPO_ROOT/build/fuzz}"
CORPUS_ROOT="${AC3FORGE_FUZZ_CORPUS_DIR:-$REPO_ROOT/fuzz/corpus}"
ARTIFACT_DIR="${AC3FORGE_FUZZ_ARTIFACT_DIR:-$REPO_ROOT/fuzz/artifacts}"
SECONDS_PER_TARGET="${AC3FORGE_FUZZ_SECONDS:-60}"

# The crash-only targets fuzz-regress/fuzz-short/fuzz-nightly run by
# default. The two differential targets (fuzz_differential_ac3_decode,
# fuzz_differential_eac3_decode - roadmap G3) are deliberately NOT in this
# list: they need `ffmpeg` on PATH and are much slower per-exec (a real
# FFmpeg process per comparable input), so they get their own CI job
# (fuzz.yml's fuzz-differential) that names them explicitly, the same way
# `fuzz/run.sh run fuzz_scan` already lets a caller run just one target from
# this list. See seed_source_for below for how they reuse seed corpora
# without duplicating any files.
readonly TARGETS=(fuzz_scan fuzz_ac3_decode fuzz_eac3_decode fuzz_wav_read)

CXX_CANDIDATE="${CXX:-clang++}"
if ! command -v "$CXX_CANDIDATE" >/dev/null 2>&1; then
    echo "error: '$CXX_CANDIDATE' not found - fuzzing needs libFuzzer, which is an LLVM" >&2
    echo "built-in and unavailable under GCC or MSVC. Install/select Clang, or set CXX." >&2
    exit 1
fi

configure_and_build() {
    # RelWithDebInfo, not Debug: libFuzzer's own guidance is to build with
    # optimizations on even under sanitizers (an unoptimized decode loop over
    # a 6-channel IMDCT is measurably slower per exec than the mutation
    # engine itself, which starves the corpus of iterations within any
    # bounded time budget). -g still lands full symbols for triage.
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_COMPILER="$CXX_CANDIDATE" \
        -DAC3FORGE_BUILD_FUZZERS=ON \
        -DAC3FORGE_BUILD_CLI=OFF \
        -DAC3FORGE_BUILD_GUI=OFF \
        -DAC3FORGE_BUILD_TESTS=OFF \
        -DAC3FORGE_BUILD_EXAMPLES=OFF
    cmake --build "$BUILD_DIR" --target ac3forge_fuzzers
}

target_binary() {
    echo "$BUILD_DIR/bin/$1"
}

# A differential target (roadmap G3) shares its crash-only sibling's seed
# corpus rather than duplicating those files under a second directory - it
# drives the exact same decode path, just with an extra FFmpeg comparison on
# top (see fuzz/differential_oracle.hpp). Every other target is its own seed
# source, unchanged.
seed_source_for() {
    case "$1" in
        fuzz_differential_ac3_decode)  echo fuzz_ac3_decode ;;
        fuzz_differential_eac3_decode) echo fuzz_eac3_decode ;;
        *)                              echo "$1" ;;
    esac
}

# Every corpus/seed/regression/artifact directory a target could read from or
# write to, created ahead of time - libFuzzer does not create its OWN corpus
# directory for you, and a missing seed/regression directory is silently
# skipped rather than reported.
prepare_dirs() {
    local target="$1"
    mkdir -p "$CORPUS_ROOT/$target" "$ARTIFACT_DIR"
}

cmd_run() {
    local requested=("${@:-${TARGETS[@]}}")
    configure_and_build
    local status=0
    for target in "${requested[@]}"; do
        prepare_dirs "$target"
        # Seeds come from seed_source_for (shared for the differential
        # targets, see its own comment); regressions stay keyed by the
        # target's OWN name always - a divergence found by
        # fuzz_differential_ac3_decode is a different class of finding from
        # a crash found by fuzz_ac3_decode, and minimizes into its own
        # fuzz/regressions/fuzz_differential_ac3_decode/ directory.
        local seeds="$REPO_ROOT/fuzz/seeds/$(seed_source_for "$target")"
        local regressions="$REPO_ROOT/fuzz/regressions/$target"
        local extra_corpora=()
        [ -d "$seeds" ] && extra_corpora+=("$seeds")
        [ -d "$regressions" ] && extra_corpora+=("$regressions")
        echo "==> $target: ${SECONDS_PER_TARGET}s (corpus: $CORPUS_ROOT/$target)"
        if ! "$(target_binary "$target")" \
                -max_total_time="$SECONDS_PER_TARGET" \
                -rss_limit_mb=2048 \
                -timeout=10 \
                -artifact_prefix="$ARTIFACT_DIR/${target}-" \
                "$CORPUS_ROOT/$target" "${extra_corpora[@]}"; then
            status=1
            echo "!! $target: a crash/hang/sanitizer report was found -" \
                 "see $ARTIFACT_DIR/${target}-*" >&2
        fi
    done
    if [ "$status" -ne 0 ]; then
        echo "" >&2
        echo "fuzzing found something - minimize it with:" >&2
        echo "  fuzz/run.sh minimize <target> <artifact file>" >&2
    fi
    exit "$status"
}

# Replays the seed + regression corpus with no time budget for mutation - a
# fast correctness check (every past regression must still not crash) rather
# than a fuzzing run. This is what CI's push-triggered job runs; the longer
# mutation budget in cmd_run is for the scheduled/nightly job.
cmd_regress() {
    local requested=("${@:-${TARGETS[@]}}")
    configure_and_build
    local status=0
    for target in "${requested[@]}"; do
        local seeds="$REPO_ROOT/fuzz/seeds/$(seed_source_for "$target")"
        local regressions="$REPO_ROOT/fuzz/regressions/$target"
        local inputs=()
        [ -d "$seeds" ] && inputs+=("$seeds")
        [ -d "$regressions" ] && inputs+=("$regressions")
        if [ "${#inputs[@]}" -eq 0 ]; then
            continue
        fi
        echo "==> $target: replaying ${inputs[*]}"
        mkdir -p "$ARTIFACT_DIR"
        if ! "$(target_binary "$target")" \
                -rss_limit_mb=2048 -timeout=10 \
                -artifact_prefix="$ARTIFACT_DIR/${target}-regress-" \
                -runs=0 "${inputs[@]}"; then
            status=1
            echo "!! $target: a known-bad input regressed" >&2
        fi
    done
    exit "$status"
}

cmd_minimize() {
    local target="${1:?usage: fuzz/run.sh minimize <target> <crash-file>}"
    local input="${2:?usage: fuzz/run.sh minimize <target> <crash-file>}"
    configure_and_build
    mkdir -p "$ARTIFACT_DIR"
    "$(target_binary "$target")" -minimize_crash=1 -runs=100000 \
        -exact_artifact_path="$ARTIFACT_DIR/${target}-minimized" \
        "$input"
    echo "minimized input written to $ARTIFACT_DIR/${target}-minimized"
}

case "${1:-run}" in
    minimize) shift; cmd_minimize "$@" ;;
    regress)  shift; cmd_regress "$@" ;;
    run)      shift || true; cmd_run "$@" ;;
    *)        cmd_run "$@" ;;
esac
