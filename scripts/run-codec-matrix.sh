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
#   - The in-repo decoder reads every Annex E tool combination now (standard
#     and enhanced coupling, spectral extension, AHT, transient pre-noise
#     processing, and any combination including 7.1.4 with several at once),
#     so every `run decode` below is a real, asserted round-trip - there is
#     nothing left to tolerate a known refusal against.
#   - FFmpeg still has no oracle at all for 7.1.4 (`714`): its
#     ff_ac3_parse_header rejects a second dependent substream's
#     `substreamid != 0` in every container tried, regardless of which Annex
#     E tools are in play. Those streams skip the FFmpeg check entirely
#     rather than being tolerated - there is nothing to tolerate a decode
#     failure against, and skipping (not tolerating) is what keeps this
#     script from silently claiming FFmpeg coverage it does not have. The
#     in-repo decoder is checked at 7.1.4 same as everywhere else.
#   - Same story, different reason, for enhanced coupling (`ecpl`) and
#     transient pre-noise processing (`tpn`): FFmpeg's own Annex E parser has
#     never read either tool's syntax at all, so there is no "known refusal"
#     to tolerate, just no oracle - see docs/verification.md's own note.
#     Those streams skip the FFmpeg check too; the in-repo decoder round trip
#     still covers them.
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
# is a stream the verification-gap table says FFmpeg CAN read, so a failure
# here always fails the script; there is no known, accepted FFmpeg gap to
# tolerate.
run_ffmpeg_check() {
    count=$((count + 1))
    echo "[$count] ffmpeg strict-decode $1"
    ffmpeg -v error -xerror -err_detect crccheck+bitstream+buffer+explode -i "$1" -f null -
}

# --- AC-3: every layout sine can address, with and without coupling --------
# (commit 8386c8f is the coupling reconstruction this exercises.)
for layout in mono stereo stereoc 51 51c 1+1; do
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
# fast-mdct=off: every other encode in this matrix now runs the default
# §7.9.4 fast forward MDCT, so this is the leg that keeps the direct
# §8.2.3.2 reference form - the validation oracle - walked under the
# sanitizers too. (E-AC-3's spelling of the same choice is the nofastmdct
# tool token below.)
run encode bootstrap_51.wav enc_fastmdct_off.ac3 256 51 fast-mdct=off
run decode enc_fastmdct_off.ac3 enc_fastmdct_off.wav
run_ffmpeg_check enc_fastmdct_off.ac3

# The synthetic panning-orbit generator: same AC-3 encode path as 'sine', with
# object motion baked in rather than a fixed layout.
run orbit orbit.ac3 2 448 4
run decode orbit.ac3 orbit.wav
run_ffmpeg_check orbit.ac3

# --- E-AC-3: every layout, every Annex E tool token -------------------------
# eac3-sine takes no tools argument (it never turns coupling/spx/aht on), so
# every layout round-trips through decode cleanly. FFmpeg reads every one of
# these EXCEPT 714 (two dependent substreams) - see the header comment.
for layout in mono stereo 51 71 512 514 714 1+1; do
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
# - so they round-trip like "none". "nofastmdct" is the same shape one step
# further: not a coding tool at all, just the direct-form forward MDCT
# instead of the default fast path, so its stream differs from "none"'s only
# at the coefficient-rounding level. Anything that actually sets
# coupling/spx/aht does not round-trip like "none", per the note above.
for tools in none "atten:2" noatten nofastmdct; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
    run_ffmpeg_check "eac3enc_${safe}.ec3"
done
# Both the in-repo decoder and FFmpeg read every one of these now - two
# independent decoders agreeing is stronger proof these Annex-E-tool encodes
# are spec-correct than either checked alone.
for tools in cpl spx aht all "spx+aht" "cpl:4+spx:5" "aht:0" "all+atten:2" "all+noatten" \
             "all+nofastmdct"; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
    run_ffmpeg_check "eac3enc_${safe}.ec3"
done

# Enhanced coupling (ecpl) and transient pre-noise processing (tpn): unlike
# every tool combination above, FFmpeg's own Annex E parser has never read
# either one's syntax at all - not a known, tolerated refusal the way 714 is
# below, just no model of the bits at all - so these skip the FFmpeg check
# entirely rather than being tolerated, same convention as 714. The in-repo
# decoder round trip (`run decode`) still covers every one of these.
for tools in "cpl+ecpl" tpn "cpl+ecpl+tpn"; do
    safe=$(echo "$tools" | tr ':+' '__')
    run eac3-encode bootstrap_51.wav "eac3enc_${safe}.ec3" 192 "$tools" 51
    run decode "eac3enc_${safe}.ec3" "eac3enc_${safe}.wav"
    echo "    [skip] eac3enc_${safe}.ec3: no FFmpeg oracle for ecpl/tpn (docs/verification.md) - the in-repo decoder is still checked above"
done

# Wider layouts: a genuine round trip with no tools, plus a tool-enabled
# encode (coupling + spx + AHT together via "all") so the wider chanmap/
# dependent-substream paths get exercised under the tools too, not just at
# 5.1. 714 is where FFmpeg's own, unrelated gap shows up: it can't read a
# second dependent substream at all regardless of which Annex E tools are in
# play, so eac3_714.ec3 and eac3_714_all.ec3 both skip the FFmpeg check same
# as the sine loop above - the in-repo decoder has no such limit and is
# checked at every layout including 714 either way.
for layout in 71 512 714; do
    run eac3-encode bootstrap_51.wav "eac3_${layout}.ec3" 256 none "$layout"
    run decode "eac3_${layout}.ec3" "eac3_${layout}_decoded.wav"
    if [ "$layout" != "714" ]; then
        run_ffmpeg_check "eac3_${layout}.ec3"
    fi
    run eac3-encode bootstrap_51.wav "eac3_${layout}_all.ec3" 256 all "$layout"
    run decode "eac3_${layout}_all.ec3" "eac3_${layout}_all.wav"
    if [ "$layout" != "714" ]; then
        run_ffmpeg_check "eac3_${layout}_all.ec3"
    else
        echo "    [skip] eac3_${layout}_all.ec3: no FFmpeg oracle for 7.1.4 (README.md Verification gaps) - the in-repo decoder is still checked above"
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

# --- 1+1 dual mono: two independent programmes, both input shapes ----------
# The sine loops above prove 1+1 round-trips through both codecs at all; this
# proves the real-audio CLI path both ways a user actually supplies Ch1/Ch2 -
# one two-channel file, or two mono ones - land the same two programmes.
# bootstrap_51.wav cannot stand in here the way it does for every layout
# above: 1+1's routing is a strict identity on exactly two source channels,
# never a fold-down, so a 6-channel source is refused rather than downmixed.
run sine bootstrap_11.ac3 3 448 440 70 1+1
run decode bootstrap_11.ac3 bootstrap_11.wav
run_ffmpeg_check bootstrap_11.ac3
# Two genuinely different mono sources, so this also proves the two files
# land as Ch1/Ch2 rather than one silently winning - not just that the
# command accepts two paths.
run sine mono_a.ac3 3 448 440 70 mono
run decode mono_a.ac3 mono_a.wav
run sine mono_b.ac3 3 448 660 70 mono
run decode mono_b.ac3 mono_b.wav

run encode bootstrap_11.wav enc_11.ac3 192 1+1 dialnorm=27 dialnorm2=18
run decode enc_11.ac3 enc_11.wav
run_ffmpeg_check enc_11.ac3
run encode mono_a.wav enc_11_twofile.ac3 192 1+1 mono_b.wav heavy
run decode enc_11_twofile.ac3 enc_11_twofile.wav
run_ffmpeg_check enc_11_twofile.ac3

run eac3-encode bootstrap_11.wav eac3enc_11.ec3 192 none 1+1 off dialnorm=27 dialnorm2=18
run decode eac3enc_11.ec3 eac3enc_11.wav
run_ffmpeg_check eac3enc_11.ec3
run eac3-encode mono_a.wav eac3enc_11_twofile.ec3 192 none 1+1 off mono_b.wav heavy
run decode eac3enc_11_twofile.ec3 eac3enc_11_twofile.wav
run_ffmpeg_check eac3enc_11_twofile.ec3

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
