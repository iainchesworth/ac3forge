#!/usr/bin/env bash
# fuzz/measure-agreement.sh - the calibration method behind
# differential_oracle.hpp's kMinAgreementDb (roadmap G3). Runs every file in
# fuzz/seeds/fuzz_ac3_decode/ and fuzz/seeds/fuzz_eac3_decode/ - real,
# already-shipping, unmutated content - through the differential harnesses
# once each with AC3FORGE_DIFF_MEASURE_ONLY=1 (measures and prints; never
# aborts, unlike a normal run) and reports the worst-channel SNR FFmpeg and
# this project's own decoder land on for each one.
#
# This is how kMinAgreementDb got its value, not a guess: the first version
# of this harness reused an existing 15 dB precedent from
# scripts/verify-gold-reference.sh, and this sweep found two seeds
# comfortably under it even though neither is remotely broken (see
# differential_oracle.hpp's own module comment for the bap-0-dither
# reasoning). Re-run this after adding new seed content - a new corner of
# legitimate decoder disagreement needs kMinAgreementDb reconsidered, not
# assumed - and after any change to compare_pcm's own alignment/silence-skip
# logic.
#
# Usage: fuzz/measure-agreement.sh
# Needs the two differential harness binaries already built
# (fuzz/run.sh regress fuzz_differential_ac3_decode fuzz_differential_eac3_decode
# builds them, even before ffmpeg is on PATH - it is only the differential
# comparison itself, not the build, that needs ffmpeg) and ffmpeg on PATH to
# measure anything at all.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${AC3FORGE_FUZZ_BUILD_DIR:-$REPO_ROOT/build/fuzz}"

command -v ffmpeg >/dev/null 2>&1 || {
    echo "error: ffmpeg not found on PATH - required as the oracle being measured against" >&2
    exit 1
}

measure() {
    local harness="$1" seeds_dir="$2"
    local binary="$BUILD_DIR/bin/$harness"
    if [ ! -x "$binary" ]; then
        echo "error: $binary not built - run: fuzz/run.sh regress $harness" >&2
        exit 1
    fi
    echo "=== $harness (against $seeds_dir) ==="
    local worst_file="" worst_db=""
    for f in "$seeds_dir"/*; do
        [ -f "$f" ] || continue
        local line
        line="$(AC3FORGE_DIFF_MEASURE_ONLY=1 "$binary" -runs=1 "$f" 2>&1 \
            | grep 'AC3FORGE_DIFF_MEASURE_ONLY' || true)"
        if [ -z "$line" ]; then
            printf '  %-40s (our own decoder declined, or FFmpeg has no oracle here)\n' "$(basename "$f")"
            continue
        fi
        local comparable db
        comparable="$(sed -n 's/.*comparable=\([0-9]*\).*/\1/p' <<<"$line")"
        db="$(sed -n 's/.*worst_db=\(-\?[0-9.]*\).*/\1/p' <<<"$line")"
        if [ "$comparable" != "1" ]; then
            printf '  %-40s (near-silent - skipped)\n' "$(basename "$f")"
            continue
        fi
        printf '  %-40s %s dB\n' "$(basename "$f")" "$db"
        if [ -z "$worst_db" ] || awk -v a="$db" -v b="$worst_db" 'BEGIN{exit !(a<b)}'; then
            worst_db="$db"
            worst_file="$(basename "$f")"
        fi
    done
    if [ -n "$worst_file" ]; then
        echo "  worst: $worst_file at $worst_db dB"
    fi
    echo
}

measure fuzz_differential_ac3_decode "$REPO_ROOT/fuzz/seeds/fuzz_ac3_decode"
measure fuzz_differential_eac3_decode "$REPO_ROOT/fuzz/seeds/fuzz_eac3_decode"
