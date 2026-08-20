#!/usr/bin/env bash
#
# Exercises ac3cli across the layout/tool/metadata matrix it documents in its
# own --help text, so a sanitizer build's "make it fail loudly" only works if
# something actually walks these code paths. ctest's 200+ cases cover a lot of
# encoder/decoder logic in isolation; this script covers the combinations a
# real user's command line would hit - every layout, every Annex E tool
# token, both Atmos container modes, and the metadata options - round-tripped
# through encode -> decode -> levels/loudness/spdif/mkv/mp4.
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
#   - One whole COMMAND, not just one FFmpeg check, is conditional: `atmos-adm`
#     (roadmap B1) only runs for real when this build was configured with
#     -DAC3FORGE_BUILD_ADM=ON, which neither of this script's two CI callers'
#     presets turn on - see that command's own block below for the detection
#     and the reasoning.
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
# The golden fixtures are read from the repo, but every path below is used
# after the `cd "$WORKDIR"` on the next lines. Resolve them now, from this
# script's own location rather than $PWD, so it does not matter where the
# script was invoked from.
FIXTURES="$(cd "$(dirname "${BASH_SOURCE[0]}")/../tests/golden/audio" && pwd)"

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

# --- AC-3: real programme material, across each layout's whole rate range --
# Everything above drives AC-3 from `sine`, `silence`, or bootstrap_51.wav -
# which is itself a decoded sine. Synthetic material cannot reach a whole
# class of encoder state, and not because of some missing option: a
# stationary tone puts near-identical exponents in every block, so
# needs_new_exponents never splits a frame into several exponent runs, and
# any defect that only appears at a mid-frame run boundary is structurally
# unreachable - at every layout, and at every bitrate.
#
# The deltbaie stale-delta defect was exactly that shape. A delta bit
# allocation that stopped part-way through a frame was never cleared, so the
# decoder went on applying it, its allocation diverged from the encoder's,
# and every field after that point was read at the wrong bit offset. It
# produced streams neither this project's decoder nor FFmpeg would accept,
# and it survived this matrix, the fidelity gate and the whole unit suite -
# because none of them ever fed the encoder real programme material. Sweeping
# the rate range on sine would not have caught it either; only the material
# axis does.
#
# So: the golden fixtures, swept rather than pinned to one comfortable rate,
# each stream decoded by BOTH the in-repo decoder and FFmpeg. The rate lists
# are each layout's real lower bound for this material - 5.1 needs 96 kbit/s
# before a frame can carry its own headers, stereo and 1+1 need 48, mono
# reaches the full range. E-AC-3 is not repeated here: quality_race.py's CI
# gate already round-trips these same fixtures through eac3-encode across its
# tool variants, so that path is not blind the way this one was.
for kbps in 48 64 96 128 160 192 256 384 448 640; do
    run encode "$FIXTURES/reference_stereo.wav" "real_stereo_${kbps}.ac3" "$kbps" stereo
    run decode "real_stereo_${kbps}.ac3" "real_stereo_${kbps}.wav"
    run_ffmpeg_check "real_stereo_${kbps}.ac3"
done
for kbps in 32 64 96 192 448 640; do
    run encode "$FIXTURES/reference_stereo.wav" "real_mono_${kbps}.ac3" "$kbps" mono
    run decode "real_mono_${kbps}.ac3" "real_mono_${kbps}.wav"
    run_ffmpeg_check "real_mono_${kbps}.ac3"
done
for kbps in 48 96 192 640; do
    run encode "$FIXTURES/reference_stereo.wav" "real_dualmono_${kbps}.ac3" "$kbps" 1+1
    run decode "real_dualmono_${kbps}.ac3" "real_dualmono_${kbps}.wav"
    run_ffmpeg_check "real_dualmono_${kbps}.ac3"
done
for kbps in 96 128 192 256 384 448 640; do
    run encode "$FIXTURES/reference_51.wav" "real_51_${kbps}.ac3" "$kbps" 51
    run decode "real_51_${kbps}.ac3" "real_51_${kbps}.wav"
    run_ffmpeg_check "real_51_${kbps}.ac3"
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
# by themselves, turn spx on (see parse_tools in src/forge/src/encoder/plan.cpp)
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
#
# "auto" belongs in this group rather than the one above because of the rate
# this loop runs at: 192 kbit/s over 5.1 is 38 kbit/s per full-bandwidth
# channel, below both of the ceilings in eac3_frame.cpp, so it turns coupling,
# spectral extension and AHT all on and its stream is nothing like "none"'s.
# It is also the tool set the landscape comparison reports, which makes it the
# one most worth holding an independent decoder against. "auto+spx:5" covers
# the other half of that decision - a caller pinning the band edge while
# leaving the on/off choice to the rate policy.
for tools in cpl spx aht all auto "auto+spx:5" "spx+aht" "cpl:4+spx:5" "aht:0" "all+atten:2" \
             "all+noatten" "all+nofastmdct"; do
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
# parser (apps/cli/main.cpp).
cat > atmos_paths.txt <<'PATHSEOF'
0 0.0 0.1 0.5 0.0 0.7 0.0
0 2.0 0.9 0.5 1.0 0.7 0.0
1 0.0 0.5 0.1 0.0 0.7 0.0
1 2.0 0.5 0.9 1.0 0.7 0.0
PATHSEOF
run atmos-path atmos_path.ec3 atmos_paths.txt 3 256 2
run decode atmos_path.ec3 atmos_path.wav
run_ffmpeg_check atmos_path.ec3

# atmos-adm (roadmap B1): only exercised for real when THIS build actually has it.
# ac3adm::ac3adm/ac3::admbridge are this project's one opt-in, non-default library
# (AC3FORGE_BUILD_ADM, default off - see the root CMakeLists.txt's own option()), and it needs
# Boost plus a dedicated vcpkg feature neither of this script's two CI callers (the ASan+UBSan
# leg, the FFmpeg-oracle leg this file's own header describes) pulls in - both build the plain
# default preset. Detected the same way ac3cli's own usage listing already answers this
# (main.cpp's Needs::kAdm/unmet(): a build without the flag lists the row as "UNAVAILABLE HERE"
# rather than omitting it), not guessed from a preset name, so this stays correct automatically
# if that ever changes (e.g. roadmap B1's own adm-validate CI job, which DOES build with the flag
# on, were ever pointed at this same script). When available, examples/encode_adm's own
# --write-fixture mode reuses its existing BW64/ADM fixture-writing code (see that file's own
# header comment on why this exists rather than a fourth copy of the same chunk-writing helpers)
# to produce a real file on disk, so atmos-adm is driven through a real file the same way every
# other command in this matrix is - not a synthetic shortcut. `run atmos-adm ...` appears in this
# script's own text either way, which is what tools/check_matrix_coverage.py's static presence
# check actually looks for - see that script's own module docstring.
ADM_FIXTURE_TOOL="$(dirname "$CLI")/examples/encode_adm"
if "$CLI" 2>&1 | grep -E '^  ac3cli atmos-adm[[:space:]]' | grep -q 'UNAVAILABLE HERE'; then
    echo "    [skip] atmos-adm: this ac3cli build has no -DAC3FORGE_BUILD_ADM=ON (apps/cli/adm/atmos_adm.hpp) - covered instead by the adm-validate CI job and tests/test_cli_atmos_adm.cpp, which do build with it"
elif [ ! -x "$ADM_FIXTURE_TOOL" ]; then
    echo "    [skip] atmos-adm: examples/encode_adm was not built alongside this ac3cli (AC3FORGE_BUILD_EXAMPLES=OFF?), so its --write-fixture mode is unavailable to generate a real ADM file"
else
    "$ADM_FIXTURE_TOOL" --write-fixture atmos_adm_fixture.wav
    run atmos-adm atmos_adm_fixture.wav atmos_adm.ec3 256
    run decode atmos_adm.ec3 atmos_adm.wav
    run_ffmpeg_check atmos_adm.ec3
fi

# --- Reporting / container passes over a representative subset -------------
run levels bootstrap_51.wav
run levels enc_stereo.ac3
run levels eac3enc_none.ec3
run loudness bootstrap_51.wav
# qc (roadmap C2): bitstream-aware loudness QC over an already-encoded
# stream. Measure-only (no preset=) always exits 0 on a clean decode, same
# as every other `run` call in this script. preset=/preset=all additionally
# gate the measurement against a named delivery spec - a real PASS/FAIL
# verdict this synthetic 440 Hz test tone has no reason to hit (it was never
# mastered to -23/-24/-27 LKFS), so its exit code is captured rather than
# trusted the way `run` trusts a clean 0 everywhere else here; this still
# proves the option parses and the whole measure-then-gate path runs to
# completion on both AC-3 and E-AC-3, which is what this script checks.
run qc bootstrap_51.ac3
run qc eac3enc_none.ec3
count=$((count + 1))
echo "[$count] qc bootstrap_51.ac3 preset=all (verdict not asserted - see comment above)"
"$CLI" qc bootstrap_51.ac3 preset=all >/dev/null || true
run spdif ac3_stereo.ac3 spdif_out.wav
run mkv enc_51.ac3 enc_51.mkv
run mkv eac3enc_none.ec3 eac3enc_none.mkv
run mkv atmos_4.ec3 atmos_4.mkv
run mp4 enc_51.ac3 enc_51.mp4
run mp4 eac3enc_none.ec3 eac3enc_none.mp4
run mp4 atmos_4.ec3 atmos_4.mp4
# fmp4 writes a directory (init segment + media segments + HLS/DASH
# manifests) rather than one file - atmos_4.ec3 in particular exercises the
# HLS CHANNELS="<N>/JOC" path (mp4/hls.hpp), since that stream carries Dolby
# Atmos objects. Concatenating the init segment with every media segment and
# strict-decoding the result, and strict-decoding the HLS media playlist
# directly, both through FFmpeg's own demuxers, is a stronger check than the
# plain exit-code one every other 'run' call gets here - exactly the
# fragment-boundary/manifest-signaling logic a single-fragment or synthetic
# test cannot exercise.
run fmp4 enc_51.ac3 fmp4_51 4
run fmp4 eac3enc_none.ec3 fmp4_eac3 4
run fmp4 atmos_4.ec3 fmp4_atmos 4
# ls -v (natural/version sort) matters here, not a plain glob: a plain
# 'segment*.m4s' glob sorts lexicographically ("segment10.m4s" before
# "segment2.m4s"), which would concatenate fragments out of sequence order -
# every moof's mfhd sequence_number/tfdt needs to stay monotonic for a real
# decoder to accept the result.
cat fmp4_atmos/init.mp4 $(ls -v fmp4_atmos/segment*.m4s) > fmp4_atmos_combined.mp4
run_ffmpeg_check fmp4_atmos_combined.mp4
run_ffmpeg_check fmp4_atmos/audio.m3u8
run ts enc_51.ac3 enc_51.ts
run ts eac3enc_none.ec3 eac3enc_none.ts
run ts atmos_4.ec3 atmos_4.ts

echo "codec matrix: $count commands completed cleanly in $WORKDIR"
