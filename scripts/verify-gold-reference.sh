#!/usr/bin/env bash
#
# The gold-reference correctness gate: proves ac3cli's own decoder agrees
# with an independent decoder (FFmpeg) on the same encoded bitstream, using a
# fixed, checked-in 5.1 WAV (tests/golden/audio/reference_51.wav - see
# tools/gen_gold_reference_wav.py) as the input material. This is
# docs/RESEARCH.md's validation pyramid L3 ("FFmpeg oracle, every commit")
# plus a lightweight L4 (SNR vs. FFmpeg's own decode) - designed from the
# start, but never wired into any CI leg until now. scripts/run-codec-
# matrix.sh already exercises "does every layout/tool/metadata combination
# run without crashing"; this is the complementary "is the audio actually
# right" check that script deliberately does not attempt.
#
# Usage: verify-gold-reference.sh <path-to-ac3cli> [workdir]
# Requires ffmpeg and python3 (or python) on PATH. Exits non-zero on the
# first check that fails.
set -euo pipefail

CLI="${1:?usage: verify-gold-reference.sh <path-to-ac3cli> [workdir]}"
WORKDIR="${2:-$(mktemp -d)}"
mkdir -p "$WORKDIR"

# Resolved from this script's own location, not the caller's cwd, so it works
# the same whether invoked from the repo root (as CI does) or anywhere else.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOLD_WAV="$REPO_ROOT/tests/golden/audio/reference_51.wav"
COMPARE="$REPO_ROOT/scripts/compare_wav.py"

# Same reasoning as docs/RESEARCH.md's L3 recipe: pin drc_scale to 0 on both
# sides so a dynamic-range-compression default mismatch between FFmpeg and
# ac3cli's own decoder (which also defaults drc_scale to 0 - see
# src/cli/main.cpp's MetaOptions) can never masquerade as a fidelity loss.
MIN_SNR_DB="${MIN_SNR_DB:-30}"

# Optional: when set, check_one also asks compare_wav.py to write a
# structured result to "$RESULTS_JSON_DIR/<label>.json" - consumed by CI's
# quality-trend job (see .github/workflows/_build.yml and
# scripts/append-quality-history.py). Unset for a plain local/manual run,
# which behaves exactly as before this existed.
RESULTS_JSON_DIR="${RESULTS_JSON_DIR:-}"

PYTHON=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
if [ -z "$PYTHON" ]; then
    echo "::error::no python3/python on PATH" >&2
    exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "::error::ffmpeg not on PATH" >&2
    exit 1
fi
if [ ! -f "$GOLD_WAV" ]; then
    echo "::error::gold reference WAV missing: $GOLD_WAV" >&2
    exit 1
fi

count=0

# L3: FFmpeg strict-decode. -err_detect crccheck+bitstream+buffer+explode
# turns on checks FFmpeg does NOT run by default (crucially the AC-3 CRC,
# per docs/RESEARCH.md's own critical finding) - pass is exit 0 with empty
# stderr, not just "a WAV came out."
ffmpeg_strict_decode() {
    local in="$1" out="$2"
    local stderr_out
    stderr_out="$(ffmpeg -y -v error -err_detect crccheck+bitstream+buffer+explode \
        -drc_scale 0 -i "$in" -f wav "$out" 2>&1 >/dev/null)"
    if [ -n "$stderr_out" ]; then
        echo "::error::ffmpeg strict decode of $in reported errors:" >&2
        echo "$stderr_out" >&2
        exit 1
    fi
}

# One (encode, decode-with-ac3cli, decode-with-ffmpeg, compare) round for a
# given codec. $1: human label. $2: the file ac3cli just produced. $3: codec
# label for --json-out. $4: nominal bitrate in kbps for --json-out.
check_one() {
    local label="$1" encoded="$2" codec="$3" bitrate_kbps="$4"
    local ffmpeg_wav="$WORKDIR/${label}_ffmpeg.wav"
    local our_wav="$WORKDIR/${label}_ours.wav"

    count=$((count + 1))
    echo "[$count] $label: FFmpeg strict decode (L3)"
    ffmpeg_strict_decode "$encoded" "$ffmpeg_wav"

    count=$((count + 1))
    echo "[$count] $label: ac3cli decode"
    "$CLI" decode "$encoded" "$our_wav" >/dev/null

    count=$((count + 1))
    echo "[$count] $label: SNR vs. FFmpeg's decode (L4-lite, >= ${MIN_SNR_DB} dB)"
    local json_args=()
    if [ -n "$RESULTS_JSON_DIR" ]; then
        mkdir -p "$RESULTS_JSON_DIR"
        json_args=(--json-out "$RESULTS_JSON_DIR/${label}.json" --codec-label "$codec" --bitrate-kbps "$bitrate_kbps")
    fi
    # Not "${json_args[@]}" unguarded: macOS's /bin/bash is stuck on 3.2, whose
    # `set -u` treats expanding a zero-element array as an unbound variable -
    # same reasoning as _build.yml's Configure step.
    if [ ${#json_args[@]} -eq 0 ]; then
        "$PYTHON" "$COMPARE" "$ffmpeg_wav" "$our_wav" --min-snr-db "$MIN_SNR_DB"
    else
        "$PYTHON" "$COMPARE" "$ffmpeg_wav" "$our_wav" --min-snr-db "$MIN_SNR_DB" "${json_args[@]}"
    fi
}

count=$((count + 1))
echo "[$count] encode: AC-3 5.1 @ 448 kbps"
"$CLI" encode "$GOLD_WAV" "$WORKDIR/gold.ac3" 448 51 >/dev/null
check_one "ac3" "$WORKDIR/gold.ac3" "ac3" 448

count=$((count + 1))
echo "[$count] encode: E-AC-3 5.1 @ 256 kbps (tools=none)"
# tools=none, not cpl/spx/aht/all: this decoder refuses any Annex E
# tool-enabled stream on decode (see scripts/run-codec-matrix.sh's own note
# on the same limitation) - this gate needs ac3cli's own decode to succeed,
# not just tolerate a known refusal, so it stays on the plain-decodable path.
"$CLI" eac3-encode "$GOLD_WAV" "$WORKDIR/gold.ec3" 256 none 51 >/dev/null
check_one "eac3" "$WORKDIR/gold.ec3" "eac3" 256

echo "gold reference gate: $count checks passed in $WORKDIR"
