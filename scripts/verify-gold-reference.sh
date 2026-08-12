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
#
# 55, not some more conservative-looking round number: this gate compares two
# decodes of the *same* bitstream (FFmpeg vs. ac3cli's own decoder), so absent
# a real bug it should sit near the floating-point noise floor forever, not
# vary the way a lossy-vs-original comparison (see tools/quality_race.py's
# very different, much lower floors) legitimately does. Every real run
# recorded in quality-history (docs/quality-trend.md) to date has landed
# 61.8-67.9 dB, with the ~6 dB floor-to-floor spread being the known,
# expected macOS-vs-Linux/Windows libm difference - not commit-to-commit
# noise, which has stayed inside 0.02-0.08 dB. 55 leaves macOS's own ~61.8 dB
# floor about 7 dB of headroom (comfortably above that noise) while catching
# a regression more than an order of magnitude smaller than the previous
# 30 dB floor ever could.
MIN_SNR_DB="${MIN_SNR_DB:-55}"

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
# label for --json-out. $4: nominal bitrate in kbps for --json-out. $5:
# min-snr-db override (default MIN_SNR_DB) - separate from the global default
# because that default is calibrated for streams THIS PROJECT's own encoder
# produced (see MIN_SNR_DB's own comment); a real third-party encoder's
# output has no such guarantee and needs its own, separately-justified floor.
check_one() {
    local label="$1" encoded="$2" codec="$3" bitrate_kbps="$4"
    local min_snr_db="${5:-$MIN_SNR_DB}"
    local ffmpeg_wav="$WORKDIR/${label}_ffmpeg.wav"
    local our_wav="$WORKDIR/${label}_ours.wav"

    count=$((count + 1))
    echo "[$count] $label: FFmpeg strict decode (L3)"
    ffmpeg_strict_decode "$encoded" "$ffmpeg_wav"

    count=$((count + 1))
    echo "[$count] $label: ac3cli decode"
    "$CLI" decode "$encoded" "$our_wav" >/dev/null

    count=$((count + 1))
    echo "[$count] $label: SNR vs. FFmpeg's decode (L4-lite, >= ${min_snr_db} dB)"
    local json_args=()
    if [ -n "$RESULTS_JSON_DIR" ]; then
        mkdir -p "$RESULTS_JSON_DIR"
        json_args=(--json-out "$RESULTS_JSON_DIR/${label}.json" --codec-label "$codec" --bitrate-kbps "$bitrate_kbps")
    fi
    # Not "${json_args[@]}" unguarded: macOS's /bin/bash is stuck on 3.2, whose
    # `set -u` treats expanding a zero-element array as an unbound variable -
    # same reasoning as _build.yml's Configure step.
    if [ ${#json_args[@]} -eq 0 ]; then
        "$PYTHON" "$COMPARE" "$ffmpeg_wav" "$our_wav" --min-snr-db "$min_snr_db"
    else
        "$PYTHON" "$COMPARE" "$ffmpeg_wav" "$our_wav" --min-snr-db "$min_snr_db" "${json_args[@]}"
    fi
}

count=$((count + 1))
echo "[$count] encode: AC-3 5.1 @ 448 kbps"
"$CLI" encode "$GOLD_WAV" "$WORKDIR/gold.ac3" 448 51 >/dev/null
check_one "ac3" "$WORKDIR/gold.ac3" "ac3" 448

count=$((count + 1))
echo "[$count] encode: E-AC-3 5.1 @ 256 kbps (tools=none)"
# The plain-path baseline: no Annex E tool engages, so this is the closest
# thing to an apples-to-apples comparison between the two decoders and sets
# the tightest floor (see MIN_SNR_DB's own comment for the 61.8-67.9 dB range
# this has landed in historically).
"$CLI" eac3-encode "$GOLD_WAV" "$WORKDIR/gold.ec3" 256 none 51 >/dev/null
check_one "eac3" "$WORKDIR/gold.ec3" "eac3" 256

count=$((count + 1))
echo "[$count] encode: E-AC-3 5.1 @ 256 kbps (tools=cpl)"
# The in-repo decoder reads every Annex E tool now (see run-codec-matrix.sh's
# header comment - the "this decoder refuses any Annex E tool-enabled
# stream" limitation that used to live in this comment is gone, confirmed by
# hand 2026-08-12: `eac3-encode ... 256 cpl 51` followed by `decode` round-
# trips cleanly). That means this gate can finally exercise a tool-enabled
# encode/decode path for real, and coupling is the one worth adding here:
# measured worst-channel SNR against FFmpeg's own decode is 67.73 dB, close
# enough to the tools=none baseline above (67.90 dB on the same run) to
# reuse the same MIN_SNR_DB floor rather than needing a separately-justified
# one - unlike the cplbndstrce0 fixture below, whose lower floor is about
# comparing against a real third-party bitstream, not about coupling itself.
#
# spx and aht are deliberately NOT added here even though the decoder reads
# them too: measured worst-channel SNR against FFmpeg's own decode is only
# ~31 dB for spx, ~20 dB for aht, and ~28 dB for all (cpl+spx+aht together) -
# both tools are approximate/generative reconstruction (spx regenerates high
# frequencies from a copied-down band plus a noise blend; AHT's Huffman/
# transform path has its own reconstruction choices), so two independent,
# spec-correct decoders legitimately diverge far more than the plain or
# cpl-only paths do. A 55 dB floor would fail on that legitimate divergence,
# not a bug, and a floor low enough to accommodate it (~15-20 dB, like
# cplbndstrce0's) would be too loose to catch a real regression in this
# tool-enabled path specifically. run-codec-matrix.sh already covers spx/aht/
# all at the round-trips-without-crashing and FFmpeg-parses-it level; this
# gate's tight dB-based regression detection just isn't the right tool for
# them yet.
"$CLI" eac3-encode "$GOLD_WAV" "$WORKDIR/gold_cpl.ec3" 256 cpl 51 >/dev/null
check_one "eac3_cpl" "$WORKDIR/gold_cpl.ec3" "eac3" 256

# Third-party interop: cplbndstrce == 0 (Annex E's default coupling band
# structure, Table E2.12). This project's own encoder always transmits an
# explicit structure (see eac3_tools.hpp's kDefaultCplBandStructure comment),
# so nothing above ever exercises this path - only a real third-party
# encoder does, which is exactly what let a real decoder bug here go
# unnoticed until FFmpeg's own E-AC-3 output was tried against this decoder
# for the first time: it refused cplbndstrce == 0 outright (kUnsupported).
# gold.ec3 above (this project's own encoder) cannot stand in for that, so
# this checks a real FFmpeg-encoded fixture directly instead of an
# ac3cli-produced one:
#   tests/golden/audio/reference_51_eac3_448k_cplbndstrce0.ec3
#     ffmpeg -y -i tests/golden/audio/reference_51.wav -c:a eac3 -b:a 448k \
#         tests/golden/audio/reference_51_eac3_448k_cplbndstrce0.ec3
# Confirmed (ffmpeg 8.0.1) to set cplbndstrce == 0 with cplbegf == 12 in
# every block - cplbegf != 0 matters: an indexing bug that reads the default
# table relative to cplbegf instead of absolutely from it would still pass
# on a stream where cplbegf happens to be 0, so this fixture is deliberately
# NOT one of those.
# Measured (ffmpeg 8.0.1, this fixture) at 25.42 dB worst-channel agreement
# between ac3cli's decode and FFmpeg's own - lower than gold.ac3/gold.ec3's
# 55 dB floor above because those compare two decodes of a stream THIS
# PROJECT's own encoder produced, while this compares two decodes of a real
# third-party bitstream neither side controls. 15 dB stays well clear of
# that measured floor while still failing hard on the failure modes this
# fixture exists to catch: a full revert (kUnsupported, no WAV at all) and
# an indexing bug that reads the table relative to cplbegf instead of
# absolutely from it (confirmed by hand: with this fixture's cplbegf == 12,
# that specific bug picks the wrong merge decisions for every coupling band
# and the corrupted geometry fails outright with kInvalidStream downstream,
# rather than merely losing fidelity).
CPLBNDSTRCE0_MIN_SNR_DB=15
CPLBNDSTRCE0_EC3="$REPO_ROOT/tests/golden/audio/reference_51_eac3_448k_cplbndstrce0.ec3"
if [ ! -f "$CPLBNDSTRCE0_EC3" ]; then
    echo "::error::fixture missing: $CPLBNDSTRCE0_EC3" >&2
    exit 1
fi
check_one "eac3_cplbndstrce0" "$CPLBNDSTRCE0_EC3" "eac3" 448 "$CPLBNDSTRCE0_MIN_SNR_DB"

echo "gold reference gate: $count checks passed in $WORKDIR"
