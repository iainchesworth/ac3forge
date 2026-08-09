#!/usr/bin/env bash
#
# Exercises ac3cli across the layout/tool/metadata matrix it documents in its
# own --help text, so a sanitizer build's "make it fail loudly" only works if
# something actually walks these code paths. ctest's 200+ cases cover a lot of
# encoder/decoder logic in isolation; this script covers the combinations a
# real user's command line would hit - every layout, every Annex E tool
# token, both Atmos container modes, and the metadata options - round-tripped
# through encode -> decode -> levels/loudness/spdif/mkv.
#
# Usage: run-codec-matrix.sh <path-to-ac3cli> [workdir]
# Exits non-zero on the first command that fails (a sanitizer violation exits
# non-zero on its own via -fno-sanitize-recover=all; this also catches a
# plain crash or a refused command that should have succeeded).
set -euo pipefail

CLI="${1:?usage: run-codec-matrix.sh <path-to-ac3cli> [workdir]}"
WORKDIR="${2:-$(mktemp -d)}"
mkdir -p "$WORKDIR"
cd "$WORKDIR"

count=0
run() {
    count=$((count + 1))
    echo "[$count] $*"
    "$CLI" "$@" >/dev/null
}

# The E-AC-3 decoder unconditionally refuses any stream that turns on ANY of
# the three Annex E tools - coupling, spectral extension, or AHT
# (src/lib/src/decoder/eac3_decoder.cpp returns DecodeError::kUnsupported the
# moment cplinu, spxinu or ahte is set) - even though eac3-encode's cpl/spx/
# aht/all tokens happily produce such a stream. tools/quality_race.py already
# works around this by decoding tool-enabled streams with FFmpeg or the
# reference player rather than this decoder, so it is likely a known,
# deliberate scope boundary (this decoder as a basic-stream/AC-3-coupling
# oracle, not a full Annex E implementation) rather than a bug - but that is
# an inference, not something this script should decide. It runs the encode
# side regardless (still exercises the tool's encoder under the sanitizer,
# and exercises the decoder's OWN refusal-detection bit-reads) but tolerates
# exactly that one, already-known refusal rather than treating it as fatal,
# so the rest of the matrix still runs. Anything else - a crash, a sanitizer
# abort, a different error code - still fails the script.
run_tolerate_eac3_tool_unsupported() {
    count=$((count + 1))
    echo "[$count] $* (Annex E tool: decode refusal is a known gap, not asserted)"
    set +e
    out=$("$CLI" "$@" 2>&1)
    status=$?
    set -e
    if [ "$status" -eq 0 ]; then
        return 0
    fi
    if [ "$status" -eq 1 ] && printf '%s' "$out" | grep -q "decode failed (code 4)"; then
        echo "    known limitation confirmed, continuing"
        return 0
    fi
    echo "$out" >&2
    echo "unexpected failure (status $status), not the known Annex E tool gap" >&2
    exit "$status"
}

# --- AC-3: every layout sine can address, with and without coupling --------
# (AC-3 coupling decode is fully implemented - unlike E-AC-3's, see above -
# commit 8386c8f is the coupling reconstruction this exercises.)
for layout in mono stereo stereoc 51 51c; do
    run sine "ac3_${layout}.ac3" 2 192 1000 80 "$layout"
    run decode "ac3_${layout}.ac3" "ac3_${layout}.wav"
done
run silence ac3_silence.ac3 1 192
run decode ac3_silence.ac3 ac3_silence.wav

# A real (non-silent, non-tone-generator) WAV to drive encode/eac3-encode/
# atmos-encode: bootstrap it from a decoded sine rather than depending on an
# external audio toolchain.
run sine bootstrap_51.ac3 3 448 440 70 51
run decode bootstrap_51.ac3 bootstrap_51.wav

for layout in mono stereo 51; do
    run encode bootstrap_51.wav "enc_${layout}.ac3" 256 "$layout"
    run decode "enc_${layout}.ac3" "enc_${layout}.wav"
done
run encode bootstrap_51.wav enc_drc.ac3 256 51 drc=film-standard
run encode bootstrap_51.wav enc_heavy.ac3 192 mono heavy ceiling=-1.0 dialogue=-24
run encode bootstrap_51.wav enc_dialnorm_auto.ac3 256 51 dialnorm=auto
run encode bootstrap_51.wav enc_cmix.ac3 224 stereo cmixlev=-4.5
run encode bootstrap_51.wav enc_surmix.ac3 224 51 surmixlev=off

# --- E-AC-3: every layout, every Annex E tool token -------------------------
# eac3-sine takes no tools argument (it never turns coupling/spx/aht on), so
# every layout round-trips through decode cleanly.
for layout in mono stereo 51 71 512 514 714; do
    run eac3-sine "eac3_${layout}.ec3" 2 192 1000 80 "$layout"
    run decode "eac3_${layout}.ec3" "eac3_${layout}.wav"
done
run eac3-silence eac3_silence.ec3 1 192 51
run decode eac3_silence.ec3 eac3_silence.wav

# "atten:N" and "noatten" alone tune spectral extension's notch but do not,
# by themselves, turn spx on (see parse_tools in src/lib/src/encoder/plan.cpp)
# - so they round-trip like "none". Anything that actually sets
# coupling/spx/aht does not, per the note above.
for tools in none "atten:2" noatten; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
done
for tools in cpl spx aht all "spx+aht" "cpl:4+spx:5" "aht:0" "all+atten:2" "all+noatten"; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run_tolerate_eac3_tool_unsupported decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
done

# Wider layouts: a genuine round trip with no tools, plus tool-enabled
# encodes (tolerated on decode) so the wider chanmap/dependent-substream
# paths get exercised under the tools too, not just at 5.1.
for layout in 71 512 714; do
    run eac3-encode bootstrap_51.wav "eac3_${layout}.ec3" 256 none "$layout"
    run decode "eac3_${layout}.ec3" "eac3_${layout}_decoded.wav"
    run eac3-encode bootstrap_51.wav "eac3_${layout}_all.ec3" 256 all "$layout"
    run_tolerate_eac3_tool_unsupported decode "eac3_${layout}_all.ec3" "eac3_${layout}_all.wav"
done

run eac3-encode bootstrap_51.wav eac3_meta.ec3 192 none 51 \
    mixmeta lfemix=10 dmixmod=ltrt drc=music-light dialnorm=auto
run decode eac3_meta.ec3 eac3_meta.wav

# --- Atmos: object counts, orbit rates, both container modes ----------------
for objects in 1 2 4 8; do
    run atmos "atmos_${objects}.ec3" 2 256 "$objects" 4 objects
    run decode "atmos_${objects}.ec3" "atmos_${objects}.wav"
done
run atmos atmos_bed51.ec3 2 256 4 4 bed51
run decode atmos_bed51.ec3 atmos_bed51.wav
run atmos-encode bootstrap_51.wav atmos_enc.ec3 256 6
run decode atmos_enc.ec3 atmos_enc.wav

# --- Reporting / container passes over a representative subset -------------
run levels bootstrap_51.wav
run levels enc_stereo.ac3
run levels eac3enc_none.ec3
run loudness bootstrap_51.wav
run spdif ac3_stereo.ac3 spdif_out.wav
run mkv enc_51.ac3 enc_51.mkv
run mkv eac3enc_none.ec3 eac3enc_none.mkv
run mkv atmos_4.ec3 atmos_4.mkv

echo "codec matrix: $count commands completed cleanly in $WORKDIR"
