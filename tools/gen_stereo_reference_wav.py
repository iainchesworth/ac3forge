"""Stereo reference WAV generator for the external-encoder landscape comparison.

Produces tests/golden/audio/reference_stereo.wav: a fixed, checked-in stereo
PCM16 WAV, sibling to gen_gold_reference_wav.py's reference_51.wav and built
the same way - from first principles (sin()/pseudo-random noise/simple FIR
smoothing), not bootstrapped by decoding one of our own encodes. Stereo is
the one layout where coupling, enhanced coupling, spectral extension, AHT,
transient pre-noise processing AND rematrixing are all simultaneously in
play, which is why this file exists alongside the 5.1 one rather than
trying to make 5.1 carry every tool's own test case.

Five concatenated segments, each chosen for a specific tool rather than one
generic signal trying to exercise everything at once:
  1. A vibrato chord, L/R correlated enough that rematrix has real work to
     do and a broken rematrix would be audible as an SNR drop (same idea as
     reference_51.wav's L/R, just the entire stereo signal here).
  2. A 100->8000 Hz sweep, R detuned from L by a fixed ratio - moving
     spectral content for spectral extension's synthesized high band to
     track, rather than one fixed seam position.
  3. Band-limited noise, R mostly-L plus independent noise - real
     high-frequency energy for spx's noise blend and coupling's shared
     envelope, not a pure tone with nothing above the coupling frequency.
  4. Near-mono "speech-like" AM/formant content, R a scaled copy of L - the
     most rematrix-favourable case, and also stationary enough across
     blocks to be a good AHT candidate.
  5. Decaying tone bursts, R the time-reverse of L - sharp attacks after
     near-silence to trigger block switching and exercise transient
     pre-noise processing's pre-echo correction.

Deliberately not silence, not a single tone, and several seconds long (see
the project's own "silence and frame 0 give false passes" lesson, restated
in CONTRIBUTING.md's "Test with real audio, from frame 1 onward").

Stdlib-only (no numpy): this only needs to run once, locally, to produce the
checked-in file - not in CI.

Usage (repo root):  python tools/gen_stereo_reference_wav.py
"""

import math
import random
import struct
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "tests" / "golden" / "audio" / "reference_stereo.wav"

RATE = 48000
SEGMENT_S = 0.6
CHANNELS = 2
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


def segment_chord(n: int) -> tuple[list[float], list[float]]:
    t = [i / RATE for i in range(n)]
    vibrato = [1.0 + 0.003 * math.sin(2 * math.pi * 4.5 * tt) for tt in t]
    chord = [sum(math.sin(2 * math.pi * f * v * tt) for f in (220.0, 277.18, 329.63))
             for tt, v in zip(t, vibrato)]
    left = [0.18 * c + 0.03 * math.sin(2 * math.pi * 1318.5 * tt) for c, tt in zip(chord, t)]
    right = [0.18 * c + 0.03 * math.sin(2 * math.pi * 987.77 * tt) for c, tt in zip(chord, t)]
    return left, right


def segment_sweep(n: int) -> tuple[list[float], list[float]]:
    t = [i / RATE for i in range(n)]
    duration = n / RATE
    f0, f1 = 100.0, 8000.0
    # Linear sweep: phase = 2*pi*(f0*t + (f1-f0)/(2*duration)*t^2).
    phase = [2 * math.pi * (f0 * tt + (f1 - f0) / (2 * duration) * tt * tt) for tt in t]
    left = [0.35 * math.sin(p) for p in phase]
    right = [0.35 * math.sin(p * 1.0006) for p in phase]  # detuned, not identical
    return left, right


def segment_noise(n: int) -> tuple[list[float], list[float]]:
    base = noise(seed=0xEAC35201, length=n, taps=20)
    extra = noise(seed=0xEAC35202, length=n, taps=20)
    left = [0.28 * v for v in base]
    right = [0.28 * (0.85 * b + 0.15 * e) for b, e in zip(base, extra)]
    return left, right


def segment_near_mono(n: int) -> tuple[list[float], list[float]]:
    t = [i / RATE for i in range(n)]
    env = [0.5 + 0.5 * math.sin(2 * math.pi * 3.0 * tt) for tt in t]
    formants = [sum(0.3 * math.sin(2 * math.pi * f * tt) for f in (700.0, 1220.0, 2600.0))
                for tt in t]
    carrier = [0.25 * math.sin(2 * math.pi * 180.0 * tt) * e * (1.0 + 0.3 * fm)
               for tt, e, fm in zip(t, env, formants)]
    left = carrier
    right = [0.97 * v for v in carrier]
    return left, right


def segment_bursts(n: int) -> tuple[list[float], list[float]]:
    left = [0.0] * n
    burst_len = 3200  # ~67 ms, well inside one segment
    burst_count = 4
    for k in range(burst_count):
        at = k * n // burst_count
        for i in range(min(burst_len, n - at)):
            left[at + i] += 0.5 * math.sin(2 * math.pi * 1500.0 * i / RATE) * math.exp(-i / 700.0)
    right = list(reversed(left))
    return left, right


def make_channels() -> list[list[float]]:
    n = int(RATE * SEGMENT_S)
    segments = [
        segment_chord(n),
        segment_sweep(n),
        segment_noise(n),
        segment_near_mono(n),
        segment_bursts(n),
    ]
    left = [v for seg in segments for v in seg[0]]
    right = [v for seg in segments for v in seg[1]]
    channels = [left, right]
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
