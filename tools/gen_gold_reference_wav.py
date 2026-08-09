"""Gold-reference WAV generator for the CI correctness gate.

Produces tests/golden/audio/reference_51.wav: a fixed, checked-in 5.1 PCM16
WAV, independent of ac3forge's own encoder/decoder (it is built here from
first principles - sin()/pseudo-random noise/simple FIR smoothing - not
bootstrapped by decoding one of our own encodes the way
scripts/run-codec-matrix.sh's "bootstrap_51.wav" is). scripts/verify-gold-
reference.sh encodes this file with ac3cli, strict-decodes the result with
FFmpeg, and compares that against ac3cli's own decode - so a codec bug that
happens to round-trip cleanly against itself still gets caught here.

Deliberately not silence, not a single tone, and several seconds long (see
the project's own "silence and frame 0 give false passes" lesson) - every
channel carries genuinely different, decorrelated material so coupling,
rematrixing and the LFE path all see real signal.

Stdlib-only (no numpy): this only needs to run once, locally, to produce the
checked-in file - not in CI - but there is no reason to ask for a dependency
this simple content does not need.

Usage (repo root):  python tools/gen_gold_reference_wav.py
"""

import math
import random
import struct
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "tests" / "golden" / "audio" / "reference_51.wav"

RATE = 48000
DURATION_S = 2.5
N = int(RATE * DURATION_S)

# WAV channel order ac3::io::ac3_layout_for(6) expects: FL FR FC LFE BL BR.
CHANNELS = 6
PEAK = 0.55  # headroom so nothing in the mix approaches full scale


def smooth(samples: list[float], taps: int) -> list[float]:
    """Simple boxcar FIR - enough to turn white noise into band-limited hiss
    without pulling in a real filter design library for a test fixture."""
    out = [0.0] * len(samples)
    window_sum = 0.0
    window: list[float] = []
    for i, s in enumerate(samples):
        window.append(s)
        window_sum += s
        if len(window) > taps:
            window_sum -= window.pop(0)
        out[i] = window_sum / len(window)
    return out


def noise(seed: int, length: int, taps: int) -> list[float]:
    rng = random.Random(seed)
    raw = [rng.uniform(-1.0, 1.0) for _ in range(length)]
    filtered = smooth(raw, taps)
    peak = max(abs(v) for v in filtered) or 1.0
    return [v / peak for v in filtered]


def make_channels() -> list[list[float]]:
    t = [i / RATE for i in range(N)]

    # Front left/right: a vibrato chord, decorrelated enough between L/R to
    # give rematrixing something real to do, correlated enough that a broken
    # rematrix would be audible/measurable as an SNR drop.
    vibrato = [1.0 + 0.003 * math.sin(2 * math.pi * 4.5 * tt) for tt in t]
    chord = [sum(math.sin(2 * math.pi * f * v * tt) for f in (220.0, 277.18, 329.63))
             for tt, v in zip(t, vibrato)]
    left = [0.18 * c + 0.03 * math.sin(2 * math.pi * 1318.5 * tt) for c, tt in zip(chord, t)]
    right = [0.18 * c + 0.03 * math.sin(2 * math.pi * 987.77 * tt) for c, tt in zip(chord, t)]

    # Center: a slow AM-modulated formant stack, standing in for dialogue -
    # distinct in character from L/R so the center channel is identifiably
    # its own signal, not a phantom image of the front pair.
    env = [0.5 + 0.5 * math.sin(2 * math.pi * 2.2 * tt) for tt in t]
    formants = [sum(0.28 * math.sin(2 * math.pi * f * tt) for f in (700.0, 1220.0, 2450.0))
                for tt in t]
    center = [0.22 * math.sin(2 * math.pi * 180.0 * tt) * e * (1.0 + 0.4 * fm)
              for tt, e, fm in zip(t, env, formants)]

    # LFE: low tones only (a real LFE path is low-passed upstream of this;
    # a test fixture just needs energy that unambiguously belongs there).
    lfe = [0.3 * math.sin(2 * math.pi * 55.0 * tt) + 0.15 * math.sin(2 * math.pi * 82.5 * tt)
           for tt in t]

    # Surrounds: band-limited noise, independently seeded so Ls/Rs are
    # decorrelated from each other and from the front pair - real diffuse
    # surround content, not a copy of anything already in the mix.
    ls = [0.16 * v for v in noise(seed=0xAC3F5105, length=N, taps=24)]
    rs = [0.16 * v for v in noise(seed=0xAC3F5106, length=N, taps=24)]

    channels = [left, right, center, lfe, ls, rs]
    peak = max(abs(v) for ch in channels for v in ch)
    scale = PEAK / peak
    return [[v * scale for v in ch] for ch in channels]


def write_wav(path: Path, channels: list[list[float]], rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = len(channels[0])
    with wave.open(str(path), "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        payload = bytearray()
        for i in range(frames):
            for ch in channels:
                sample = max(-32768, min(32767, round(ch[i] * 32767.0)))
                payload += struct.pack("<h", sample)
        w.writeframesraw(bytes(payload))


def main() -> None:
    channels = make_channels()
    write_wav(OUT, channels, RATE)
    print(f"wrote {OUT} ({len(channels[0]) / RATE:.2f}s, {len(channels)} channels @ {RATE} Hz)")


if __name__ == "__main__":
    main()
