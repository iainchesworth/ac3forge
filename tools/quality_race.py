"""Quality race: our encoder vs FFmpeg's ac3 encoder at matched bitrates.

Synthesizes stereo program material (tones with vibrato, a sweep, filtered
noise, correlated near-mono content for rematrixing, tone bursts), encodes it
with both encoders, decodes both with FFmpeg (the neutral referee), aligns by
cross-correlation, and reports SNR vs the original.

Races (repo root, after building):
  python tools/quality_race.py ac3      ours vs ffmpeg           (the default)
  python tools/quality_race.py couple   ours with vs without coupling

The `couple` race scores the two things coupling is supposed to trade off
against each other, because a single overall SNR hides both. Below the
coupling frequency it reports SNR: those coefficients are still coded
normally, so coupling should BUY precision there and a drop means the shared
channel is eating the bits it was meant to free. Above it it reports level
error instead of SNR, because a coupled decoder reconstructs that band's
envelope and not its waveform - and level, per short window, is exactly what
a coupling scale that changes from block to block gets wrong.
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
# Bin 109 of the 256-bin MDCT, which is where cplbegf 6 starts coupling.
CPL_HZ = 109 * (RATE / 512.0)


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


def align(original, decoded):
    """Align by cross-correlation on a probe window; return the overlap."""
    probe = original[RATE:RATE + 32768, 0]
    window = decoded[: RATE + 65536, 0]
    corr = np.correlate(window, probe, mode="valid")
    lag = int(np.argmax(np.abs(corr))) - RATE
    n = min(len(original), len(decoded) - lag) - 2 * RATE
    o = original[RATE:RATE + n - RATE]
    d = decoded[RATE + lag:RATE + lag + len(o)]
    return o, d, lag


def snr_db(o, d):
    noise = d - o
    return 10 * np.log10(np.sum(o**2) / max(np.sum(noise**2), 1e-30))


def aligned_snr(original, decoded):
    o, d, lag = align(original, decoded)
    return snr_db(o, d), lag


def band_measures(o, d, size=2048):
    """SNR below the coupling frequency, level error above it.

    Above it a coupled decoder restores the band's envelope rather than its
    waveform, so a waveform SNR up there measures the tool's premise, not its
    correctness. The level per short window is the thing that must survive -
    and the thing a coupling coordinate carrying the wrong block's scale
    destroys, in exactly the three blocks of six that reuse a coordinate.
    """
    hop = size // 2
    win = np.hanning(size)
    freqs = np.fft.rfftfreq(size, 1.0 / RATE)
    low = freqs < CPL_HZ
    high = ~low
    low_signal = 0.0
    low_noise = 0.0
    pairs = []
    for start in range(0, len(o) - size, hop):
        for ch in range(o.shape[1]):
            spec_o = np.fft.rfft(o[start:start + size, ch] * win)
            spec_d = np.fft.rfft(d[start:start + size, ch] * win)
            low_signal += float(np.sum(np.abs(spec_o[low]) ** 2))
            low_noise += float(np.sum(np.abs(spec_d[low] - spec_o[low]) ** 2))
            pairs.append((float(np.sum(np.abs(spec_o[high]) ** 2)),
                          float(np.sum(np.abs(spec_d[high]) ** 2))))
    # Score only the windows that carry real high-frequency content. A window
    # 40 dB below the loudest one is inaudible up there, and counting it would
    # drown the measurement in the near-silence between events.
    loudest = max((p[0] for p in pairs), default=0.0)
    errors = [abs(10 * np.log10(max(got, 1e-30) / want))
              for want, got in pairs if want > loudest * 1e-4]
    return (10 * np.log10(low_signal / max(low_noise, 1e-30)),
            float(np.mean(errors)) if errors else float("nan"))


def encode_and_decode(source, tag, kbps, couple=False):
    """Our encoder, then FFmpeg as the neutral decoder."""
    ac3 = BUILD / f"race_{tag}_{kbps}.ac3"
    wav = BUILD / f"race_{tag}_{kbps}.wav"
    cmd = [CLI, "encode", source, ac3, str(kbps)]
    if couple:
        cmd.append("couple")
    run(cmd)
    run(["ffmpeg", "-v", "error", "-y",
         "-err_detect", "crccheck+bitstream+buffer+explode",
         "-i", ac3, "-c:a", "pcm_f32le", wav])
    return read_wav_f32(wav)


def race_ffmpeg(source, original):
    print(f"{'kbps':>5} | {'ours dB':>8} | {'ffmpeg dB':>9} | {'gap':>6}")
    print("-" * 38)
    worst_gap = -1e9
    for kbps in (192, 256, 320, 448):
        ff_ac3 = BUILD / f"race_ff_{kbps}.ac3"
        ff_wav = BUILD / f"race_ff_{kbps}.wav"
        run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "ac3",
             "-b:a", f"{kbps}k", ff_ac3])
        run(["ffmpeg", "-v", "error", "-y", "-i", ff_ac3, "-c:a", "pcm_f32le", ff_wav])
        ours_snr, _ = aligned_snr(original, encode_and_decode(source, "ours", kbps))
        ff_snr, _ = aligned_snr(original, read_wav_f32(ff_wav))
        gap = ff_snr - ours_snr
        worst_gap = max(worst_gap, gap)
        print(f"{kbps:>5} | {ours_snr:>8.2f} | {ff_snr:>9.2f} | {gap:>+6.2f}")
    print(f"\nworst gap vs ffmpeg: {worst_gap:+.2f} dB (positive = ffmpeg better)")


def race_coupling(source, original):
    print(f"{'kbps':>5} | {'mode':>6} | {'all dB':>7} | "
          f"{'<10.2k dB':>9} | {'>10.2k err dB':>13}")
    print("-" * 56)
    for kbps in (96, 128, 192, 256):
        scores = {}
        for mode, couple in (("plain", False), ("couple", True)):
            decoded = encode_and_decode(source, f"cpl_{mode}", kbps, couple)
            o, d, _ = align(original, decoded)
            low_snr, high_err = band_measures(o, d)
            scores[mode] = (snr_db(o, d), low_snr, high_err)
            print(f"{kbps:>5} | {mode:>6} | {scores[mode][0]:>7.2f} | "
                  f"{low_snr:>9.2f} | {high_err:>13.2f}")
        low_gain = scores["couple"][1] - scores["plain"][1]
        print(f"{'':>5} | {'delta':>6} | {'':>7} | {low_gain:>+9.2f} | "
              f"{scores['couple'][2] - scores['plain'][2]:>+13.2f}")
    print("\nBaseband delta positive = coupling bought precision where it should.")
    print("Coupled-band error is |level error|, so lower is better either way.")


def main():
    race = sys.argv[1] if len(sys.argv) > 1 else "ac3"
    if race not in ("ac3", "couple"):
        raise SystemExit(f"unknown race {race!r}; expected 'ac3' or 'couple'")
    BUILD.mkdir(exist_ok=True)
    left, right = make_material()
    source = BUILD / "race_src.wav"
    write_wav_f32(source, left, right)
    original = read_wav_f32(source)
    if race == "ac3":
        race_ffmpeg(source, original)
    else:
        race_coupling(source, original)


if __name__ == "__main__":
    main()
