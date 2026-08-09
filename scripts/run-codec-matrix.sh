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
# Every stream this script produces also gets FFmpeg's independent strict
# decode (CONTRIBUTING.md's "Oracles" list, #2) alongside the in-repo
# decoder's `run decode`, per the verification-gap table in README.md:
#   - FFmpeg is the only oracle for Annex E coupling/spx/AHT, and a failure
#     there is always a hard failure - unlike the in-repo decoder's known,
#     tolerated refusal of those same tools (see
#     run_tolerate_eac3_tool_unsupported below).
#   - FFmpeg has no oracle at all for 7.1.4 (`714`): its ff_ac3_parse_header
#     rejects a second dependent substream's `substreamid != 0` in every
#     container tried. Those streams skip the FFmpeg check entirely rather
#     than being tolerated - there is nothing to tolerate a decode failure
#     against, and skipping (not tolerating) is what keeps this script from
#     silently claiming coverage it does not have.
#   - 7.1.4 combined with an Annex E tool has no oracle anywhere in this
#     project (neither decoder reads it) and stays that way here too.
#
# Usage: run-codec-matrix.sh <path-to-ac3cli> [workdir]
# Exits non-zero on the first command that fails (a sanitizer violation exits
# non-zero on its own via -fno-sanitize-recover=all; this also catches a
# plain crash, a refused command that should have succeeded, or an FFmpeg
# decode that should have succeeded but didn't).
set -euo pipefail

CLI="${1:?usage: run-codec-matrix.sh <path-to-ac3cli> [workdir]}"
# Resolve to an absolute path before the `cd "$WORKDIR"` below: CI passes a
# path relative to the repo root (e.g. "build/.../bin/ac3cli"), which stops
# resolving the moment the working directory changes.
case "$CLI" in
    /*) ;;
    *) CLI="$PWD/$CLI" ;;
esac
WORKDIR="${2:-$(mktemp -d)}"
mkdir -p "$WORKDIR"
cd "$WORKDIR"

command -v ffmpeg >/dev/null 2>&1 || {
    echo "ffmpeg not found on PATH; it is required as the independent oracle this script checks against" >&2
    exit 1
}

count=0
run() {
    count=$((count + 1))
    echo "[$count] $*"
    "$CLI" "$@" >/dev/null
}

# FFmpeg as an independent oracle (CONTRIBUTING.md's "Oracles" list, #2),
# always the strict decode-to-null the oracles table documents - without
# -err_detect FFmpeg conceals errors rather than reporting them. -xerror is
# NOT optional belt-and-braces: -err_detect alone only controls what the
# decoder treats as an error internally (concealing a bad frame and moving
# on); it does not, by itself, change ffmpeg's own exit code, which stays 0
# even after a logged CRC mismatch. -xerror ("exit on error") is the flag
# that actually turns a detected error into a failing process - verified by
# hand against a deliberately corrupted stream while writing this function,
# per CONTRIBUTING.md's "prove the test can fail" rule. Every call site below
# is a stream the verification-gap table says FFmpeg CAN read, so unlike
# run_tolerate_eac3_tool_unsupported above, a failure here always fails the
# script; there is no known, accepted FFmpeg gap to tolerate.
run_ffmpeg_check() {
    count=$((count + 1))
    echo "[$count] ffmpeg strict-decode $1"
    ffmpeg -v error -xerror -err_detect crccheck+bitstream+buffer+explode -i "$1" -f null -
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
    run_ffmpeg_check "ac3_${layout}.ac3"
done
run silence ac3_silence.ac3 1 192
run decode ac3_silence.ac3 ac3_silence.wav
run_ffmpeg_check ac3_silence.ac3

# A real (non-silent, non-tone-generator) WAV to drive encode/eac3-encode/
# atmos-encode: bootstrap it from a decoded sine rather than depending on an
# external audio toolchain.
run sine bootstrap_51.ac3 3 448 440 70 51
run decode bootstrap_51.ac3 bootstrap_51.wav
run_ffmpeg_check bootstrap_51.ac3

for layout in mono stereo 51; do
    run encode bootstrap_51.wav "enc_${layout}.ac3" 256 "$layout"
    run decode "enc_${layout}.ac3" "enc_${layout}.wav"
    run_ffmpeg_check "enc_${layout}.ac3"
done
run encode bootstrap_51.wav enc_drc.ac3 256 51 drc=film-standard
run_ffmpeg_check enc_drc.ac3
run encode bootstrap_51.wav enc_heavy.ac3 192 mono heavy ceiling=-1.0 dialogue=-24
run_ffmpeg_check enc_heavy.ac3
run encode bootstrap_51.wav enc_dialnorm_auto.ac3 256 51 dialnorm=auto
run_ffmpeg_check enc_dialnorm_auto.ac3
run encode bootstrap_51.wav enc_cmix.ac3 224 stereo cmixlev=-4.5
run_ffmpeg_check enc_cmix.ac3
run encode bootstrap_51.wav enc_surmix.ac3 224 51 surmixlev=off
run_ffmpeg_check enc_surmix.ac3

# The synthetic panning-orbit generator: same AC-3 encode path as 'sine', with
# object motion baked in rather than a fixed layout.
run orbit orbit.ac3 2 448 4
run decode orbit.ac3 orbit.wav
run_ffmpeg_check orbit.ac3

# --- E-AC-3: every layout, every Annex E tool token -------------------------
# eac3-sine takes no tools argument (it never turns coupling/spx/aht on), so
# every layout round-trips through decode cleanly. FFmpeg reads every one of
# these EXCEPT 714 (two dependent substreams) - see the header comment.
for layout in mono stereo 51 71 512 514 714; do
    run eac3-sine "eac3_${layout}.ec3" 2 192 1000 80 "$layout"
    run decode "eac3_${layout}.ec3" "eac3_${layout}.wav"
    if [ "$layout" != "714" ]; then
        run_ffmpeg_check "eac3_${layout}.ec3"
    fi
done
run eac3-silence eac3_silence.ec3 1 192 51
run decode eac3_silence.ec3 eac3_silence.wav
run_ffmpeg_check eac3_silence.ec3

# "atten:N" and "noatten" alone tune spectral extension's notch but do not,
# by themselves, turn spx on (see parse_tools in src/lib/src/encoder/plan.cpp)
# - so they round-trip like "none". Anything that actually sets
# coupling/spx/aht does not, per the note above.
for tools in none "atten:2" noatten; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
    run_ffmpeg_check "eac3enc_${safe}.ec3"
done
# FFmpeg is the ONLY oracle for these: it reads cpl/spx/aht where the in-repo
# decoder refuses them (tolerated above), so the FFmpeg check here is not
# optional belt-and-braces - it is the sole proof any of this matrix's
# Annex-E-tool encodes are actually spec-correct.
for tools in cpl spx aht all "spx+aht" "cpl:4+spx:5" "aht:0" "all+atten:2" "all+noatten"; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run_tolerate_eac3_tool_unsupported decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
    run_ffmpeg_check "eac3enc_${safe}.ec3"
done

# Wider layouts: a genuine round trip with no tools, plus tool-enabled
# encodes (tolerated on decode) so the wider chanmap/dependent-substream
# paths get exercised under the tools too, not just at 5.1. 714 is where the
# two gaps stack: FFmpeg can't read a second dependent substream at all, so
# eac3_714.ec3 (no tools) skips the FFmpeg check same as the sine loop above,
# and eac3_714_all.ec3 (two dependents AND an Annex E tool) has no working
# decoder anywhere in the project - skip it outright rather than tolerate a
# failure, so this script never implies that combination is covered.
for layout in 71 512 714; do
    run eac3-encode bootstrap_51.wav "eac3_${layout}.ec3" 256 none "$layout"
    run decode "eac3_${layout}.ec3" "eac3_${layout}_decoded.wav"
    if [ "$layout" != "714" ]; then
        run_ffmpeg_check "eac3_${layout}.ec3"
    fi
    run eac3-encode bootstrap_51.wav "eac3_${layout}_all.ec3" 256 all "$layout"
    run_tolerate_eac3_tool_unsupported decode "eac3_${layout}_all.ec3" "eac3_${layout}_all.wav"
    if [ "$layout" != "714" ]; then
        run_ffmpeg_check "eac3_${layout}_all.ec3"
    else
        echo "    [skip] eac3_${layout}_all.ec3: 7.1.4 + Annex E tool has no oracle anywhere (README.md Verification gaps)"
    fi
done

run eac3-encode bootstrap_51.wav eac3_meta.ec3 192 none 51 \
    mixmeta lfemix=10 dmixmod=ltrt drc=music-light dialnorm=auto
run decode eac3_meta.ec3 eac3_meta.wav
run_ffmpeg_check eac3_meta.ec3

# --- E-AC-3 VBR: quality-targeted rate control (eac3-encode's [vbr] arg,
# default "off" - everything above this point never touched it) -------------
# bitrate_kbps still matters in vbr mode - it feeds the coupling/spx
# frequency defaults, per the CLI's own vbr help text - so it stays a real
# value rather than a placeholder. A modest quality with no max bound stays
# well clear of the "refuses real programme material outright" warning; the
# bounded case exercises the min:/max: syntax the unbounded one does not.
for vbr in "q:0.3" "q:0.6,min:96,max:256"; do
    safe=$(echo "$vbr" | tr ':,' '__')
    run eac3-encode bootstrap_51.wav "eac3_vbr_${safe}.ec3" 192 none 51 "$vbr"
    run decode "eac3_vbr_${safe}.ec3" "eac3_vbr_${safe}.wav"
    run_ffmpeg_check "eac3_vbr_${safe}.ec3"
done

# --- Atmos: object counts, orbit rates, both container modes ----------------
# Always a 5.1 bed (JOC/OAMD ride in the same independent substream's EMDF
# container, never a dependent one), so FFmpeg reads all of these - it is how
# README.md's "FFmpeg reports Dolby Digital Plus + Dolby Atmos" claim is
# checked at all.
for objects in 1 2 4 8; do
    run atmos "atmos_${objects}.ec3" 2 256 "$objects" 4 objects
    run decode "atmos_${objects}.ec3" "atmos_${objects}.wav"
    run_ffmpeg_check "atmos_${objects}.ec3"
done
run atmos atmos_bed51.ec3 2 256 4 4 bed51
run decode atmos_bed51.ec3 atmos_bed51.wav
run_ffmpeg_check atmos_bed51.ec3
run atmos-encode bootstrap_51.wav atmos_enc.ec3 256 6
run decode atmos_enc.ec3 atmos_enc.wav
run_ffmpeg_check atmos_enc.ec3

# atmos-path: a tiny hand-authored keyframe file, proving the file-driven
# object path round-trips too, not just the built-in synthetic orbit 'atmos'
# uses. Format is 'object time_s x y z gain lfe_send' per run_atmos_path's
# parser (src/cli/main.cpp).
cat > atmos_paths.txt <<'PATHSEOF'
0 0.0 0.1 0.5 0.0 0.7 0.0
0 2.0 0.9 0.5 1.0 0.7 0.0
1 0.0 0.5 0.1 0.0 0.7 0.0
1 2.0 0.5 0.9 1.0 0.7 0.0
PATHSEOF
run atmos-path atmos_path.ec3 atmos_paths.txt 3 256 2
run decode atmos_path.ec3 atmos_path.wav
run_ffmpeg_check atmos_path.ec3

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
