"""Property/fuzz harness over the AC-3 ENCODER's own input space.

Every fuzzing target this project already has (fuzz/fuzz_ac3_decode.cpp and
friends, .github/workflows/fuzz.yml) mutates an ALREADY-ENCODED bitstream:
they ask "does the decoder survive corrupt input". Nothing asked the other
question - "does the encoder, driven across its own legal configuration space
by adversarial but perfectly valid audio, ever emit a stream a decoder
refuses". scripts/run-codec-matrix.sh walks a hand-enumerated list of command
lines against one bootstrap tone; tools/check_matrix_coverage.py only checks
that each option TOKEN appears in that list. Neither has any notion of option
COMBINATIONS, and neither varies the input material at all.

That gap is not hypothetical. PR #186 fixed an encoder defect where deltbaie
== 0 was emitted to mean "no delta bit allocation this block" when A/52
§5.4.3.47 defines it outside block 0 as "retain the previous block's delta
bit allocation". The encoder's allocation and the decoder's then disagreed,
every subsequent field was read at the wrong bit offset, and the stream was
rejected by BOTH this project's decoder and FFmpeg. It escaped ctest, the
codec matrix, the gold-reference gate and every fuzzing job, because reaching
it needs a specific input SHAPE - dense harmonic content followed by digital
silence part-way through one frame, so an exponent run stops wanting a delta
correction mid-frame - at a rate low enough for the allocator to have wanted
one in the first place. No hand-written matrix would think to enumerate that.
A generator finds it in seconds; the `cliff` audio profile below is that
shape, named and kept.

What one case does:

  1. Draw a random-but-legal encoder configuration (layout, bitrate,
     coupling, DRC profile, heavy compression, dialnorm, downmix levels,
     forward-MDCT path, and layout 1+1's second-programme twins of those).
  2. Draw adversarial PCM and write it as a real WAV - built per 256-sample
     BLOCK, not per file, so a frame's character can change part-way through
     it. That is what drives exponent-run splits, block switching and the
     delta bit allocation decisions the configuration space alone cannot
     reach.
  3. Encode it.
  4. Decode the result with BOTH this project's own decoder (`ac3cli decode`)
     and FFmpeg's strict decode - the same invocation and the same reasoning
     as run_ffmpeg_check() in scripts/run-codec-matrix.sh, -xerror included,
     because -err_detect alone leaves ffmpeg's exit code at 0 after a logged
     error. A refusal from either one fails the case.

Two independent decoders matter here for the same reason they do in the codec
matrix: a stream that only round-trips against its own encoder proves much
less than one two unrelated implementations both accept.

Determinism. Every case is a pure function of a single 64-bit case seed, so a
failure is reproducible from the one number printed with it - see --replay.
The master seed only decides which case seeds get drawn; it is printed at the
start of every run, failure or not.

Known open finding, as of the commit that added this file. Roughly one case in
seven hundred fails at the `ffmpeg` stage with "error in bit allocation" while
`ac3cli decode` accepts the same stream, always with coupling on.
choose_delta_segments (src/lib/src/core/bitalloc.cpp) writes delta bit
allocation offsets with its cursor starting at band 0; FFmpeg starts its
cursor at the channel's own start band, so on the coupling channel - the only
one whose start band is not 0 - the segments land past band 50 and hit
FFmpeg's range guard. compute_bit_allocation() starts at band 0 as well, so
this encoder and this decoder agree with each other and the round trip never
notices, which is exactly what an independent oracle is for. Seeding that
cursor at bndstrt clears every reproducer, but that is a diagnosis and not a
fix: the decoder needs the matching change, and which reading §7.2.2.6 intends
has to be settled first. Until then both CI callers of this harness are
continue-on-error - see the note on ci.yml's own encoder-space step.

Scope: AC-3 (`ac3cli encode`) only. The bug that motivated this is AC-3-only
(E-AC-3 computes its delta bit allocation once per frame and its decoder
clears at block 0, so its state cannot go stale between blocks), and AC-3's
configuration space is the one whose acceptance envelope this file pins
below. E-AC-3's own space - the Annex E tool tokens, VBR, the wider layouts -
is a real remaining gap, and deliberately not smuggled in half-covered here.

Usage (repo root, after building):
  python tools/fuzz_encoder_space.py --cli build/dev/bin/ac3cli.exe --cases 200
  python tools/fuzz_encoder_space.py --seconds 120            # bounded, for CI
  python tools/fuzz_encoder_space.py --check-envelope         # re-measure the table below
  python tools/fuzz_encoder_space.py --replay 12345678901234  # one exact case
"""

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from dataclasses import dataclass, field, asdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# A/52 Table 5.18's nominal rates - the only ones AC-3's frmsizcod can index
# (ac3::is_valid_bitrate, src/lib/include/ac3/core/tables.hpp).
LEGAL_RATES = [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
               384, 448, 512, 576, 640]

# A/52 §5.4.1.3: 48, 44.1 and 32 kHz. 44.1 additionally exercises the
# frame-padding (pad441) path nothing else here would reach.
SAMPLE_RATES = [48000, 44100, 32000]

BLOCK = 256                  # §5.3.2: samples per audio block
BLOCKS_PER_FRAME = 6         # §5.3.1
FRAME = BLOCK * BLOCKS_PER_FRAME

# The smallest elementary stream this harness will hand FFmpeg, in bytes.
#
# Not a property of AC-3, and not a guess. FFmpeg auto-detects the container
# of a raw .ac3 file by probing, and on a stream of only a few kilobytes that
# probe is unreliable in a way that has nothing to do with whether the
# bitstream is valid. Measured while building this: a perfectly good 5-frame,
# 1920-byte mono stream was rejected with "Invalid data found when processing
# input" before decoding began, while the SAME BYTES decoded cleanly under
# `-f ac3` with the full -err_detect crccheck+bitstream+buffer+explode set,
# and the same content at ten frames probed fine. Narrowing it further:
# overwriting the crc1 field of any single middle frame - or zeroing crc1
# outright, which can only make the stream MORE wrong - flipped the probe from
# reject to accept, with every BSI header byte identical throughout. That is a
# probe-scoring artifact on a short file, not a defect a decoder would ever
# see.
#
# scripts/run-codec-matrix.sh never meets it because everything it encodes
# runs for seconds. Rather than force -f ac3 and quietly stop testing the same
# thing the codec matrix tests, this harness keeps that script's exact
# invocation and stays in the size regime where it means what it says. 16 KiB
# is roughly eight times the size where the effect was last seen.
MIN_STREAM_BYTES = 16384

# Where each layout's usable rate range starts, measured against this build
# rather than derived from the spec. The refusal is not a table lookup but
# ac3::FrameEncoder's own budget check (encoder.cpp, "the chosen configuration
# cannot fit its own headers at this rate"), and its cost moves with how many
# streams the layout codes, with coupling and DRC, and - through the exponent
# strategy and the delta bit allocation - with the AUDIO. So there is no one
# floor to record; there are two, and conflating them would make this table
# either a lie or useless:
#
#   robust  every rate from here up encodes whatever you feed it. A refusal at
#           or above this rate is a regression, and --check-envelope asserts it
#           across a spread of deliberately different probe material.
#   min     the lowest rate this generator DRAWS. Between min and robust the
#           answer genuinely depends on the input, so drawing there is real
#           coverage of the most bit-starved corner of the space - it just
#           means some of those cases come back refused rather than encoded.
#           --check-envelope asserts something still encodes here, so a min
#           set below anything reachable shows up instead of silently burning
#           budget on cases that can never run.
#
# Refusals between min and robust are recognised by the encoder's own exact
# message, counted, and reported in the run summary - never treated as
# failures, and never allowed to absorb any other kind of non-zero exit. See
# classify().
LAYOUTS = {
    #             min  robust  source channel counts this layout accepts
    "mono":   {"min": 32, "robust": 32, "sources": [1, 2, 6]},
    "stereo": {"min": 48, "robust": 48, "sources": [2, 6]},
    "51":     {"min": 96, "robust": 112, "sources": [6]},
    # 1+1 routes exactly two source channels as two independent programmes -
    # a strict identity, never a fold-down, so it refuses any other width.
    "1+1":    {"min": 48, "robust": 48, "sources": [2]},
}

DRC_PROFILES = ["film-standard", "film-light", "music-standard", "music-light", "speech"]
CMIXLEV = ["-3", "-4.5", "-6"]
SURMIXLEV = ["-3", "-6", "off"]

# The two ways a generated configuration can be legitimately refused, keyed by
# the CLI's own exact words on exit code 1. Both are correct behaviour meeting
# a real limit, not defects, and neither can be excluded up front - see the
# per-entry notes.
#
# This is an allow-list, not a catch-all: any OTHER non-zero exit is a
# failure. A blanket "non-zero means skip" would let a crash, or a brand new
# error, pass as a tolerated refusal - the exact way a harness stops being a
# gate. The run summary breaks refusals down by reason so a shift in the mix
# is visible rather than buried in one total.
REFUSALS = {
    # ac3::FrameEncoder's budget check (encoder.cpp: "the chosen configuration
    # cannot fit its own headers at this rate"). Reachable between a layout's
    # `min` and `robust` rates because the side-information cost moves with
    # coupling, DRC and the audio itself - see LAYOUTS.
    "header room": "bitrate must be a legal AC-3 rate",
    # dialnorm=auto measures BS.1770-4 loudness, whose -70 LKFS absolute gate
    # discards every block below it; material that is entirely below the gate
    # leaves nothing to measure, and the CLI says so and names the fix rather
    # than inventing a number. This generator deliberately produces near-
    # silent and mostly-silent material (the `transient` profile, and the
    # silent half of every `cliff`), so it meets that limit honestly.
    "sub-gate loudness": "no audio above the -70 LKFS absolute gate",
}


# --- adversarial PCM --------------------------------------------------------
# Each function fills ONE 256-sample block for ONE channel, so the character
# of the signal can change part-way through a frame. `phase` is carried across
# blocks by the caller, which is what makes a run of `tone` blocks one
# continuous sinusoid while a change of character (or of frequency) lands as a
# genuine discontinuity at a block boundary.
#
# Block boundaries here are the WAV's, not the encoder's: the MDCT's own
# half-block lookahead means sample-domain block k lands in coded block k or
# k+1 depending on the window. That offset does not matter - the point is that
# a boundary exists inside the frame at all, not which side of it lands where.

def _fill_silence(out, rng, p, phase, rate):
    for i in range(BLOCK):
        out[i] = 0.0
    return phase


def _fill_dc(out, rng, p, phase, rate):
    level = p["amp"] * rng.choice([1.0, -1.0])
    for i in range(BLOCK):
        out[i] = level
    return phase


def _fill_tone(out, rng, p, phase, rate):
    step = 2.0 * math.pi * p["freq"] / rate
    amp = p["amp"]
    for i in range(BLOCK):
        out[i] = amp * math.sin(phase)
        phase += step
    return phase % (2.0 * math.pi)


def _fill_harmonics(out, rng, p, phase, rate):
    """Dense harmonic content - the half of the `cliff` shape that makes the
    allocator want a delta correction in the first place. A single tone leaves
    most of the spectrum empty and most exponent runs flat; forty harmonics do
    not."""
    f0 = p["freq"]
    count = max(1, int((rate / 2.0) / f0))
    count = min(count, p["harmonics"])
    norm = sum(1.0 / k for k in range(1, count + 1))
    amp = p["amp"] / norm
    for i in range(BLOCK):
        t = phase + 2.0 * math.pi * f0 * i / rate
        acc = 0.0
        for k in range(1, count + 1):
            acc += math.sin(k * t) / k
        out[i] = amp * acc
    return (phase + 2.0 * math.pi * f0 * BLOCK / rate) % (2.0 * math.pi)


def _fill_noise(out, rng, p, phase, rate):
    amp = p["amp"]
    for i in range(BLOCK):
        out[i] = amp * rng.uniform(-1.0, 1.0)
    return phase


def _fill_pink(out, rng, p, phase, rate):
    """Broadband but spectrally tilted, via a one-pole smoother over white -
    enough to give the exponent strategy a sloped spectrum to describe rather
    than the flat one white noise gives it, without a filter-design
    dependency."""
    amp = p["amp"]
    state = 0.0
    for i in range(BLOCK):
        state = 0.92 * state + 0.08 * rng.uniform(-1.0, 1.0)
        out[i] = amp * state * 6.0
    _clamp(out)
    return phase


def _fill_impulse(out, rng, p, phase, rate):
    """Silence with a handful of full-scale spikes: the transient-detector and
    block-switching path's worst case, and a maximal within-block energy
    step."""
    for i in range(BLOCK):
        out[i] = 0.0
    for _ in range(rng.randint(1, 4)):
        out[rng.randrange(BLOCK)] = rng.choice([1.0, -1.0]) * p["amp"]
    return phase


def _fill_chirp(out, rng, p, phase, rate):
    """A sweep crossing most of the band inside one block, so no exponent run
    describes it well for long."""
    lo, hi = 200.0, rate / 2.0 * 0.95
    amp = p["amp"]
    for i in range(BLOCK):
        frac = i / BLOCK
        f = lo * math.pow(hi / lo, frac)
        phase += 2.0 * math.pi * f / rate
        out[i] = amp * math.sin(phase)
    return phase % (2.0 * math.pi)


def _fill_nyquist(out, rng, p, phase, rate):
    """Alternating +/- amp: all the energy in the top exponent band."""
    amp = p["amp"]
    for i in range(BLOCK):
        out[i] = amp if (i % 2) == 0 else -amp
    return phase


def _fill_clipped(out, rng, p, phase, rate):
    """A sine driven well past full scale then hard-clipped - a square-ish
    wave whose harmonics reach the top of the band at full level, and the
    sample values a real over-driven master arrives with."""
    step = 2.0 * math.pi * p["freq"] / rate
    for i in range(BLOCK):
        value = 3.0 * math.sin(phase)
        out[i] = max(-1.0, min(1.0, value))
        phase += step
    return phase % (2.0 * math.pi)


def _fill_square(out, rng, p, phase, rate):
    step = 2.0 * math.pi * p["freq"] / rate
    amp = p["amp"]
    for i in range(BLOCK):
        out[i] = amp if math.sin(phase) >= 0.0 else -amp
        phase += step
    return phase % (2.0 * math.pi)


def _clamp(out):
    for i in range(BLOCK):
        out[i] = max(-1.0, min(1.0, out[i]))


CHARACTERS = {
    "silence": _fill_silence,
    "dc": _fill_dc,
    "tone": _fill_tone,
    "harmonics": _fill_harmonics,
    "noise": _fill_noise,
    "pink": _fill_pink,
    "impulse": _fill_impulse,
    "chirp": _fill_chirp,
    "nyquist": _fill_nyquist,
    "clipped": _fill_clipped,
    "square": _fill_square,
}

# Characters that put real, dense energy across the band - the "loud" half of
# a cliff, and what makes the bit allocator work hard enough to want a delta
# correction at all.
DENSE = ["harmonics", "noise", "pink", "chirp", "clipped", "square"]

AUDIO_PROFILES = [
    "chaotic",        # a fresh character every block
    "runs",           # one character held for a few blocks, then a switch
    "cliff",          # dense energy then DIGITAL SILENCE, inside one frame
    "spectral_jump",  # low-band and high-band content alternating per block
    "transient",      # near-silence punctuated by impulses
    "steady",         # one character throughout - the baseline case
]


def _character_params(rng, character):
    return {
        "amp": rng.choice([1.0, 0.95, 0.7, 0.4, 0.15, 0.02]),
        "freq": rng.choice([60.0, 110.0, 220.0, 440.0, 997.0, 3000.0, 7500.0, 14000.0]),
        "harmonics": rng.randint(8, 40),
    }


def _block_plan(rng, profile, blocks):
    """The (character, params) each 256-sample block gets, for one channel.

    Returned as a whole-file plan rather than decided as the file is written,
    so `cliff` can place its silence relative to a FRAME boundary - the
    property the deltbaie defect needed and the reason this is per-block at
    all."""
    plan = []
    if profile == "steady":
        character = rng.choice(list(CHARACTERS))
        params = _character_params(rng, character)
        return [(character, params)] * blocks

    if profile == "cliff":
        # Per frame: j blocks of dense, loud content, then digital silence for
        # the rest of THAT frame. j is redrawn each frame so the boundary
        # moves, and j == BLOCKS_PER_FRAME occasionally gives a frame with no
        # cliff at all, which is what makes the NEXT frame's cliff a
        # transition rather than a steady state.
        for start in range(0, blocks, BLOCKS_PER_FRAME):
            character = rng.choice(DENSE)
            params = _character_params(rng, character)
            params["amp"] = rng.choice([1.0, 0.95, 0.8])
            j = rng.randint(1, BLOCKS_PER_FRAME)
            for b in range(BLOCKS_PER_FRAME):
                if start + b >= blocks:
                    break
                if b < j:
                    plan.append((character, params))
                else:
                    plan.append(("silence", params))
        return plan

    if profile == "spectral_jump":
        low = {"amp": rng.choice([1.0, 0.7, 0.4]), "freq": rng.choice([60.0, 110.0, 220.0]),
               "harmonics": rng.randint(2, 6)}
        high = {"amp": rng.choice([1.0, 0.7, 0.4]), "freq": rng.choice([7500.0, 14000.0]),
                "harmonics": rng.randint(8, 40)}
        for b in range(blocks):
            if b % 2 == 0:
                plan.append(("harmonics", low))
            else:
                plan.append((rng.choice(["nyquist", "chirp", "tone"]), high))
        return plan

    if profile == "transient":
        quiet = {"amp": 0.02, "freq": 440.0, "harmonics": 8}
        for b in range(blocks):
            if rng.random() < 0.3:
                plan.append(("impulse", {"amp": 1.0, "freq": 440.0, "harmonics": 8}))
            else:
                plan.append((rng.choice(["silence", "tone"]), quiet))
        return plan

    if profile == "runs":
        while len(plan) < blocks:
            character = rng.choice(list(CHARACTERS))
            params = _character_params(rng, character)
            for _ in range(rng.randint(1, 5)):
                plan.append((character, params))
        return plan[:blocks]

    # chaotic
    for _ in range(blocks):
        character = rng.choice(list(CHARACTERS))
        plan.append((character, _character_params(rng, character)))
    return plan


def generate_pcm(rng, channels, blocks, rate, profile, correlation):
    """Every channel's float samples for the whole file, in [-1, 1]."""
    data = [[0.0] * (blocks * BLOCK) for _ in range(channels)]
    scratch = [0.0] * BLOCK

    # Which channels get their own material; the rest are copied or inverted
    # from a neighbour below. Correlation is worth varying because coupling
    # (§7.4) and rematrixing both key off how alike the channels are, and
    # independently-generated channels are never alike.
    independent = list(range(channels))
    if correlation == "identical":
        independent = [0]
    elif correlation in ("pairs", "inverted"):
        # L/R (and Ls/Rs) share material, so those pairs are genuinely
        # correlated - `inverted` makes them maximally ANTI-correlated, which
        # is what a sum/difference rematrixing decision has to survive.
        independent = [c for c in range(channels) if c % 2 == 0]

    rendered = {}
    for ch in independent:
        ch_profile = profile if profile != "mixed" else rng.choice(AUDIO_PROFILES)
        plan = _block_plan(rng, ch_profile, blocks)
        phase = rng.uniform(0.0, 2.0 * math.pi)
        for b, (character, params) in enumerate(plan):
            phase = CHARACTERS[character](scratch, rng, params, phase, rate)
            _clamp(scratch)
            data[ch][b * BLOCK:(b + 1) * BLOCK] = scratch[:]
        rendered[ch] = data[ch]

    for ch in range(channels):
        if ch in rendered:
            continue
        source = rendered[0] if correlation == "identical" else rendered[ch - 1]
        if correlation == "inverted" or (correlation == "pairs" and rng.random() < 0.3):
            data[ch] = [-s for s in source]
        else:
            data[ch] = list(source)

    return data


def write_wav(path, data, rate, pcm16):
    """A real WAV on disk - PCM16 or float32, the two ac3::io::read_wav
    accepts (src/lib/src/io/wav.cpp). Both are exercised: the float path
    carries exact +/-1.0 full scale, which PCM16's asymmetric range cannot."""
    channels = len(data)
    frames = len(data[0])
    if pcm16:
        payload = bytearray()
        for i in range(frames):
            for ch in range(channels):
                value = int(round(max(-1.0, min(1.0, data[ch][i])) * 32767.0))
                payload += struct.pack("<h", value)
        with wave.open(str(path), "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(bytes(payload))
        return

    # float32: written by hand, since the stdlib wave module only writes
    # integer PCM.
    payload = bytearray()
    for i in range(frames):
        for ch in range(channels):
            payload += struct.pack("<f", max(-1.0, min(1.0, data[ch][i])))
    fmt = struct.pack("<HHIIHH", 3, channels, rate,
                      rate * channels * 4, channels * 4, 32)
    riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + \
        b"data" + struct.pack("<I", len(payload)) + bytes(payload)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)


# --- configuration ----------------------------------------------------------

@dataclass
class Case:
    seed: int
    layout: str
    bitrate: int
    sample_rate: int
    source_channels: int
    frames: int
    pcm16: bool
    audio_profile: str
    correlation: str
    options: list = field(default_factory=list)

    def cli_args(self, wav, out):
        return ["encode", str(wav), str(out), str(self.bitrate), self.layout, *self.options]


def draw_case(seed):
    """A case is a pure function of its seed - the whole point of --replay."""
    rng = random.Random(seed)

    layout = rng.choice(list(LAYOUTS))
    info = LAYOUTS[layout]
    rates = [r for r in LEGAL_RATES if r >= info["min"]]
    # Weighted low: the deltbaie defect fired at 64 and 96 kbit/s, and the
    # side-information budget, the exponent strategy and the delta bit
    # allocation are all under most pressure at the bottom of the range. The
    # top of the range still gets drawn, just less often.
    weights = [1.0 / (1.0 + i) for i in range(len(rates))]
    bitrate = rng.choices(rates, weights=weights, k=1)[0]

    options = []
    couple = rng.random() < 0.45
    if couple:
        options.append("couple")
    if rng.random() < 0.5:
        options.append(f"drc={rng.choice(DRC_PROFILES)}")
    if rng.random() < 0.3:
        options.append("heavy")
        if rng.random() < 0.5:
            options.append(f"ceiling={rng.choice(['-0.1', '-0.5', '-1.0', '-3.0'])}")
        if rng.random() < 0.5:
            options.append(f"dialogue={rng.choice(['-31', '-24', '-20', '-14'])}")
    roll = rng.random()
    if roll < 0.2:
        options.append("dialnorm=auto")
    elif roll < 0.4:
        options.append(f"dialnorm={rng.randint(1, 31)}")
    if rng.random() < 0.3:
        options.append(f"cmixlev={rng.choice(CMIXLEV)}")
    if rng.random() < 0.3:
        options.append(f"surmixlev={rng.choice(SURMIXLEV)}")
    if rng.random() < 0.15:
        # The direct §8.2.3.2 forward MDCT rather than the default §7.9.4 fast
        # path - the validation oracle, and a different set of coefficient
        # roundings feeding every downstream decision.
        options.append("fast-mdct=off")
    if layout == "1+1":
        # The second programme's own metadata, which is NOT inherited from the
        # first: an encoder that quietly applied one programme's settings to
        # both would still pass every check above.
        if rng.random() < 0.5:
            options.append(f"drc2={rng.choice(DRC_PROFILES)}")
        if rng.random() < 0.3:
            options.append("heavy2")
        if rng.random() < 0.3:
            options.append(f"dialnorm2={rng.choice(['auto', str(rng.randint(1, 31))])}")

    sample_rate = rng.choice(SAMPLE_RATES)
    source_channels = rng.choice(info["sources"])
    # Several frames, never one: "silence and frame 0 give false passes" is
    # this project's own recorded lesson, and the delta bit allocation state
    # this exists to stress only goes stale BETWEEN blocks of a frame that
    # already carried one. The floor on top of that is MIN_STREAM_BYTES'.
    frames = frames_for(bitrate, sample_rate, rng.randint(6, 24))

    return Case(
        seed=seed,
        layout=layout,
        bitrate=bitrate,
        sample_rate=sample_rate,
        source_channels=source_channels,
        frames=frames,
        pcm16=rng.random() < 0.7,
        audio_profile=rng.choices(
            AUDIO_PROFILES + ["mixed"],
            # `cliff` is the shape the known defect needed, so it is drawn
            # more often than an even split would give it - without crowding
            # out the profiles that would find a DIFFERENT shape's bug.
            weights=[1.0, 1.0, 2.5, 1.5, 1.0, 0.5, 1.0], k=1)[0],
        correlation=rng.choice(["independent", "identical", "pairs", "inverted"]),
        options=options,
    )


def frames_for(bitrate, sample_rate, wanted):
    """`wanted` frames, or however many it takes to clear MIN_STREAM_BYTES.

    Frame size is fixed by the rate pair (§5.4.1.3): bitrate x 1536 samples /
    sample rate, in bytes. At 640 kbit/s the floor costs nothing; at 32 kbit/s
    it is what turns a fifth of a second into a few seconds."""
    frame_bytes = bitrate * 1000 * FRAME / sample_rate / 8
    return max(wanted, math.ceil(MIN_STREAM_BYTES / frame_bytes))


def case_seed(master, index):
    digest = hashlib.sha256(f"{master}:{index}".encode()).hexdigest()
    return int(digest[:16], 16)


# --- running one case -------------------------------------------------------

@dataclass
class Result:
    case: Case
    status: str          # "ok" | "refused" | "fail"
    detail: str = ""
    stage: str = ""
    reason: str = ""     # which REFUSALS entry matched, when status == "refused"


def _run(argv, cwd=None):
    return subprocess.run(argv, capture_output=True, text=True, cwd=cwd)


def ffmpeg_check(ffmpeg, path):
    """FFmpeg's strict decode, byte-for-byte the invocation
    run_ffmpeg_check() in scripts/run-codec-matrix.sh uses.

    -xerror is not belt-and-braces: -err_detect alone only changes what the
    decoder treats as an error internally, concealing a bad frame and moving
    on with exit code 0. -xerror is what turns a detected error into a failing
    process."""
    return _run([ffmpeg, "-v", "error", "-xerror", "-err_detect",
                 "crccheck+bitstream+buffer+explode", "-i", str(path), "-f", "null", "-"])


def classify(case, encode, out_path):
    """Why a non-zero encode is not automatically a failure - and why almost
    every non-zero encode still is. See REFUSALS for the two recognised
    refusals and why neither can be excluded up front; everything else,
    including a zero exit that wrote nothing, is a failure."""
    if encode.returncode != 0:
        if encode.returncode == 1:
            for reason, message in REFUSALS.items():
                if message in encode.stderr:
                    return Result(case, "refused", encode.stderr.strip(), "encode", reason)
        return Result(case, "fail",
                      f"encode exited {encode.returncode}\n{encode.stderr.strip()}", "encode")
    if not out_path.exists() or out_path.stat().st_size == 0:
        return Result(case, "fail", "encode exited 0 but wrote no bitstream", "encode")
    return Result(case, "ok")


def run_case(cli, ffmpeg, case, workdir, artifacts):
    tmp = Path(tempfile.mkdtemp(prefix=f"encspace_{case.seed:016x}_", dir=workdir))
    try:
        wav = tmp / "in.wav"
        out = tmp / "out.ac3"
        decoded = tmp / "out.wav"

        data = generate_pcm(random.Random(case.seed ^ 0xA53F), case.source_channels,
                            case.frames * BLOCKS_PER_FRAME, case.sample_rate,
                            case.audio_profile, case.correlation)
        write_wav(wav, data, case.sample_rate, case.pcm16)

        encode = _run([cli, *case.cli_args(wav, out)])
        result = classify(case, encode, out)
        if result.status != "ok":
            if result.status == "fail":
                save_artifacts(artifacts, case, result, tmp)
            return result

        decode = _run([cli, "decode", str(out), str(decoded)])
        if decode.returncode != 0:
            result = Result(case, "fail",
                            f"ac3cli decode exited {decode.returncode}\n"
                            f"{decode.stderr.strip()}", "decode")
            save_artifacts(artifacts, case, result, tmp)
            return result
        if not decoded.exists() or decoded.stat().st_size == 0:
            result = Result(case, "fail", "decode exited 0 but wrote no PCM", "decode")
            save_artifacts(artifacts, case, result, tmp)
            return result

        if ffmpeg is not None:
            check = ffmpeg_check(ffmpeg, out)
            if check.returncode != 0:
                result = Result(case, "fail",
                                f"ffmpeg strict decode exited {check.returncode}\n"
                                f"{check.stderr.strip()}", "ffmpeg")
                save_artifacts(artifacts, case, result, tmp)
                return result

        return Result(case, "ok")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def save_artifacts(artifacts, case, result, tmp):
    if artifacts is None:
        return
    dest = artifacts / f"{case.seed:016x}"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("in.wav", "out.ac3", "out.wav"):
        source = tmp / name
        if source.exists():
            shutil.copy2(source, dest / name)
    (dest / "case.json").write_text(json.dumps(asdict(case), indent=2) + "\n")
    (dest / "failure.txt").write_text(
        f"{describe(case)}\n\nstage: {result.stage}\n\n{result.detail}\n\n"
        f"{repro(case)}\n")


def describe(case):
    return (f"seed={case.seed} layout={case.layout} bitrate={case.bitrate} "
            f"rate={case.sample_rate} src_ch={case.source_channels} "
            f"frames={case.frames} fmt={'pcm16' if case.pcm16 else 'float32'} "
            f"audio={case.audio_profile}/{case.correlation} "
            f"options=[{' '.join(case.options)}]")


def repro(case):
    return (f"regenerate:  python tools/fuzz_encoder_space.py --replay {case.seed}\n"
            f"the encode:  ac3cli encode in.wav out.ac3 {case.bitrate} {case.layout} "
            f"{' '.join(case.options)}")


# --- the acceptance envelope this generator draws from ----------------------

# Deliberately different shapes rather than N draws of one - the whole reason
# `robust` and `min` differ is that the floor moves with the material, so a
# probe set that agrees with itself would measure nothing.
ENVELOPE_PROBES = ["steady", "chaotic", "cliff", "spectral_jump", "transient", "runs"]


def check_envelope(cli):
    """Re-measure LAYOUTS against this binary.

    A hand-maintained table is exactly the kind of second copy this project's
    own tools/check_matrix_coverage.py refuses to trust, so it gets checked
    rather than believed. Two assertions, one per column of that table:

      robust  EVERY probe encodes at EVERY rate from here up. This is the one
              that catches a regression - an encoder that started refusing
              configurations it used to fit.
      min     at least one probe encodes at each rate from min up to robust.
              This catches a min set below anything reachable, which would
              silently spend the run's budget on cases that can never encode.
    """
    failures = []
    with tempfile.TemporaryDirectory(prefix="encspace_env_") as tmp_str:
        tmp = Path(tmp_str)
        out = tmp / "env.ac3"
        for layout, info in LAYOUTS.items():
            channels = max(info["sources"])
            probes = []
            for i, profile in enumerate(ENVELOPE_PROBES):
                wav = tmp / f"env_{layout}_{profile}.wav"
                data = generate_pcm(random.Random(100 + i), channels, BLOCKS_PER_FRAME * 4,
                                    48000, profile, "independent")
                write_wav(wav, data, 48000, True)
                probes.append(wav)

            counts = {}
            for rate in LEGAL_RATES:
                if rate < info["min"]:
                    continue
                counts[rate] = sum(
                    1 for wav in probes
                    if _run([cli, "encode", str(wav), str(out), str(rate),
                             layout]).returncode == 0)

            not_robust = [r for r, n in counts.items()
                          if r >= info["robust"] and n < len(probes)]
            unreachable = [r for r, n in counts.items() if r < info["robust"] and n == 0]
            ok = not not_robust and not unreachable
            summary = " ".join(f"{r}:{n}/{len(probes)}" for r, n in counts.items())
            print(f"  {'PASS' if ok else 'FAIL'}  {layout} (min {info['min']}, "
                  f"robust {info['robust']}): {summary}")
            if not_robust:
                failures.append(f"{layout}: refused at or above robust={info['robust']} "
                                f"for rates {not_robust}")
            if unreachable:
                failures.append(f"{layout}: no probe encodes at all at rates {unreachable}, "
                                f"which min={info['min']} still draws")
    if failures:
        print("\nacceptance envelope has drifted from LAYOUTS in this file:")
        for f in failures:
            print(f"  {f}")
        print("A 'refused at or above robust' line is an encoder regression, not a table "
              "problem - fix that before touching this file. A 'no probe encodes' line means "
              "min is set below anything reachable; raise it, and say why in the commit.")
        return 1
    print("\nacceptance envelope matches LAYOUTS")
    return 0


# --- driver -----------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default=os.environ.get("AC3CLI", "build/dev/bin/ac3cli.exe"),
                        help="path to ac3cli (or set AC3CLI)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="path to ffmpeg")
    parser.add_argument("--no-ffmpeg", action="store_true",
                        help="skip the independent oracle - the in-repo decoder only")
    parser.add_argument("--seed", type=int, default=None,
                        help="master seed; a random one is drawn and printed if omitted")
    parser.add_argument("--cases", type=int, default=None, help="run exactly this many cases")
    parser.add_argument("--seconds", type=float, default=None,
                        help="run until this many seconds have elapsed")
    parser.add_argument("--replay", type=int, default=None,
                        help="run the single case with this exact case seed")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--artifacts", default="fuzz-encoder-artifacts",
                        help="where failing cases are written")
    parser.add_argument("--check-envelope", action="store_true",
                        help="re-measure the per-layout minimum bitrates and exit")
    parser.add_argument("--max-failures", type=int, default=10,
                        help="stop after this many failing cases")
    args = parser.parse_args()

    cli = args.cli if Path(args.cli).is_absolute() else str((REPO / args.cli).resolve())
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")

    if args.check_envelope:
        print("acceptance envelope - the minimum legal rate each layout's encoder accepts")
        sys.exit(check_envelope(cli))

    ffmpeg = None
    if not args.no_ffmpeg:
        ffmpeg = shutil.which(args.ffmpeg)
        if ffmpeg is None:
            raise SystemExit(
                f"ffmpeg not found ('{args.ffmpeg}') - it is the independent oracle half of "
                "this harness. Pass --no-ffmpeg to run against the in-repo decoder alone, "
                "knowing that a stream which only round-trips against its own encoder proves "
                "much less.")

    artifacts = Path(args.artifacts)
    if not artifacts.is_absolute():
        artifacts = REPO / artifacts

    if args.replay is not None:
        case = draw_case(args.replay)
        print(describe(case))
        with tempfile.TemporaryDirectory(prefix="encspace_") as workdir:
            result = run_case(cli, ffmpeg, case, workdir, artifacts)
        print(f"  {result.status}"
              + (f" ({result.stage}): {result.detail}" if result.detail else ""))
        sys.exit(1 if result.status == "fail" else 0)

    if args.cases is None and args.seconds is None:
        args.cases = 100
    master = args.seed if args.seed is not None else random.SystemRandom().randrange(2 ** 63)

    print(f"encoder-space fuzz: cli={cli} ffmpeg={'off' if ffmpeg is None else ffmpeg}")
    print(f"master seed {master}"
          + (f", {args.cases} cases" if args.cases else f", {args.seconds:g}s budget")
          + f", {args.jobs} jobs")
    print("(every failure below prints its own case seed; --replay <seed> reruns just it)")
    print()

    started = time.monotonic()
    counts = {"ok": 0, "refused": 0, "fail": 0}
    refusals = {reason: 0 for reason in REFUSALS}
    failures = []
    index = 0

    def budget_left():
        if args.cases is not None:
            return index < args.cases
        return (time.monotonic() - started) < args.seconds

    with tempfile.TemporaryDirectory(prefix="encspace_") as workdir:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = set()
            while (budget_left() or pending) and len(failures) < args.max_failures:
                while budget_left() and len(pending) < args.jobs * 2:
                    case = draw_case(case_seed(master, index))
                    index += 1
                    pending.add(pool.submit(run_case, cli, ffmpeg, case, workdir, artifacts))
                if not pending:
                    break
                done, pending = concurrent.futures.wait(
                    pending, return_when=concurrent.futures.FIRST_COMPLETED)
                for future in done:
                    result = future.result()
                    counts[result.status] += 1
                    if result.status == "refused":
                        refusals[result.reason] += 1
                    if result.status == "fail":
                        failures.append(result)
                        print(f"FAIL [{result.stage}] {describe(result.case)}")
                        print(f"  {result.detail.splitlines()[0] if result.detail else ''}")
                        print(f"  {repro(result.case)}".replace("\n", "\n  "))
                        print()

    elapsed = time.monotonic() - started
    total = sum(counts.values())
    breakdown = ", ".join(f"{n} {reason}" for reason, n in refusals.items() if n)
    print(f"{total} cases in {elapsed:.1f}s: {counts['ok']} encoded and decoded cleanly, "
          f"{counts['refused']} refused"
          + (f" ({breakdown})" if breakdown else "")
          + f", {counts['fail']} failed")

    if counts["fail"]:
        print(f"\nfailing inputs kept in {artifacts}")
        print("Each directory holds the exact in.wav/out.ac3 plus the config that produced it.")
        sys.exit(1)

    # A run where the encoder refused nearly everything is not a pass: it
    # would mean the generator has drifted out of the accepted space, or the
    # encoder has regressed into refusing it, and every case above would have
    # been "clean" without a single stream being checked.
    if total >= 20 and counts["ok"] < total * 0.5:
        print(f"\nonly {counts['ok']} of {total} configurations encoded at all - too few to "
              "call this a pass. Either the generator is drawing outside the accepted space "
              "(re-run with --check-envelope) or the encoder has regressed into refusing it.")
        sys.exit(1)

    if total == 0:
        print("\nno cases ran at all - the budget was too small to be a gate")
        sys.exit(1)


if __name__ == "__main__":
    main()
