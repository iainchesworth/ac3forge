"""Quality race: our encoder vs FFmpeg's ac3 encoder at matched bitrates.

Synthesizes stereo program material (tones with vibrato, a sweep, filtered
noise, correlated near-mono content for rematrixing, tone bursts), encodes it
with both encoders, decodes both with FFmpeg (the neutral referee), aligns by
cross-correlation, and reports SNR vs the original per segment and overall.

Usage (repo root, after building):  python tools/quality_race.py
"""

import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
CLI = BUILD / "dev" / "bin" / "ac3cli.exe"
RATE = 48000
SEG = 2 * RATE  # 2 s per segment


def make_material():
    rng = np.random.default_rng(0x0B77)
    t = np.arange(SEG) / RATE
    segments = []

    # a) chord with vibrato
    vib = 1.0 + 0.002 * np.sin(2 * np.pi * 5.0 * t)
    chord = sum(np.sin(2 * np.pi * f * vib * t) for f in (220.0, 277.2, 329.6, 440.0))
    left = 0.15 * chord + 0.02 * np.sin(2 * np.pi * 1234.0 * t)
    right = 0.15 * chord + 0.02 * np.sin(2 * np.pi * 987.0 * t)
    segments.append((left, right))

    # b) sweep 100 -> 8000 Hz
    phase = 2 * np.pi * (100.0 * t + (8000.0 - 100.0) / (2 * t[-1]) * t * t)
    segments.append((0.4 * np.sin(phase), 0.4 * np.sin(phase * 1.0005)))

    # c) band-limited noise (pink-ish via cumulative smoothing)
    w = rng.standard_normal(SEG + 512)
    kernel = np.hanning(64)
    smooth = np.convolve(w, kernel / kernel.sum(), mode="same")[:SEG]
    noise = 0.3 * smooth / np.max(np.abs(smooth))
    segments.append((noise, 0.9 * noise + 0.1 * rng.standard_normal(SEG) * 0.05))

    # d) near-mono speech-ish (correlated -> rematrix territory)
    carrier = np.sin(2 * np.pi * 180.0 * t) * (0.5 + 0.5 * np.sin(2 * np.pi * 3.0 * t))
    formants = sum(0.3 * np.sin(2 * np.pi * f * t) for f in (700.0, 1220.0, 2600.0))
    mono = 0.25 * carrier * (1.0 + 0.3 * formants)
    segments.append((mono, mono * 0.98))

    # e) tone bursts (transient-ish, long-block stress)
    burst = np.zeros(SEG)
    for k in range(8):
        at = k * SEG // 8
        n = np.arange(4096)
        burst[at:at + 4096] += 0.5 * np.sin(2 * np.pi * 1500.0 * n / RATE) * np.exp(-n / 800.0)
    segments.append((burst, burst[::-1].copy()))

    left = np.concatenate([s[0] for s in segments]).astype(np.float32)
    right = np.concatenate([s[1] for s in segments]).astype(np.float32)
    return np.clip(left, -0.98, 0.98), np.clip(right, -0.98, 0.98)


def write_wav_f32(path, left, right):
    data = np.empty(left.size * 2, dtype=np.float32)
    data[0::2] = left
    data[1::2] = right
    payload = data.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, 2, RATE, RATE * 8, 8, 32))
        f.write(b"data" + struct.pack("<I", len(payload)) + payload)


def read_wav_f32(path):
    b = Path(path).read_bytes()
    i = b.find(b"fmt ")
    ch = struct.unpack_from("<H", b, i + 10)[0]
    j = b.find(b"data")
    n = struct.unpack_from("<I", b, j + 4)[0]
    return np.frombuffer(b, dtype=np.float32, count=n // 4, offset=j + 8).reshape(-1, ch)


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(map(str, cmd))}\n{result.stderr}")


def aligned_snr(original, decoded):
    """Align by cross-correlation on a probe window, SNR over the overlap."""
    probe = original[RATE:RATE + 32768, 0]
    window = decoded[: RATE + 65536, 0]
    corr = np.correlate(window, probe, mode="valid")
    lag = int(np.argmax(np.abs(corr))) - RATE
    n = min(len(original), len(decoded) - lag) - 2 * RATE
    o = original[RATE:RATE + n - RATE]
    d = decoded[RATE + lag:RATE + lag + len(o)]
    noise = d - o
    return 10 * np.log10(np.sum(o**2) / max(np.sum(noise**2), 1e-30)), lag


def main():
    BUILD.mkdir(exist_ok=True)
    left, right = make_material()
    source = BUILD / "race_src.wav"
    write_wav_f32(source, left, right)
    original = read_wav_f32(source)

    print(f"{'kbps':>5} | {'ours dB':>8} | {'ffmpeg dB':>9} | {'gap':>6}")
    print("-" * 38)
    worst_gap = -1e9
    for kbps in (192, 256, 320, 448):
        ours_ac3 = BUILD / f"race_ours_{kbps}.ac3"
        ff_ac3 = BUILD / f"race_ff_{kbps}.ac3"
        run([CLI, "encode", source, ours_ac3, str(kbps)])
        run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "ac3",
             "-b:a", f"{kbps}k", ff_ac3])
        ours_wav = BUILD / f"race_ours_{kbps}.wav"
        ff_wav = BUILD / f"race_ff_{kbps}.wav"
        run(["ffmpeg", "-v", "error", "-y",
             "-err_detect", "crccheck+bitstream+buffer+explode",
             "-i", ours_ac3, "-c:a", "pcm_f32le", ours_wav])
        run(["ffmpeg", "-v", "error", "-y", "-i", ff_ac3, "-c:a", "pcm_f32le", ff_wav])
        ours_snr, _ = aligned_snr(original, read_wav_f32(ours_wav))
        ff_snr, _ = aligned_snr(original, read_wav_f32(ff_wav))
        gap = ff_snr - ours_snr
        worst_gap = max(worst_gap, gap)
        print(f"{kbps:>5} | {ours_snr:>8.2f} | {ff_snr:>9.2f} | {gap:>+6.2f}")
    print(f"\nworst gap vs ffmpeg: {worst_gap:+.2f} dB (positive = ffmpeg better)")


if __name__ == "__main__":
    main()
