"""Does channel coupling actually preserve each channel's HF envelope?

Coupling replaces the coefficients above the coupling frequency with a shared
channel plus per-band coordinates, so the two channels no longer carry
independent detail up there - only their per-band energy is restored. The
meaningful check is therefore not "each channel keeps its own tone" but
"a channel loud in a high band stays loud, and a quiet one stays quiet".

Builds a stereo probe (both channels share a low tone; only LEFT carries a
high tone inside the coupled region), encodes it with and without coupling,
decodes both with FFmpeg, and reports the left/right energy ratio in the
high band plus the low-band fidelity that coupling must not disturb.

Usage (repo root, after building):  python tools/check_coupling.py
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
# AC3CLI overrides the binary - see tools/quality_race.py's CLI constant for
# why the default below (a "dev" preset that does not exist, an .exe on a
# platform that may not have one) is not a usable default everywhere.
CLI = Path(os.environ.get("AC3CLI", str(BUILD / "dev" / "bin" / "ac3cli.exe")))
RATE = 48000
SECONDS = 4
LOW_HZ = 700.0
HIGH_HZ = 12000.0  # inside the default coupling region (~10.2-20.3 kHz)


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
        raise SystemExit(f"failed: {' '.join(map(str, cmd))}\n{result.stdout}{result.stderr}")


def band_energy(x, low, high):
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    freqs = np.fft.rfftfreq(len(x), 1.0 / RATE)
    return float(np.sum(spec[(freqs >= low) & (freqs <= high)]))


def analyse(tag, path):
    pcm = read_wav_f32(path)
    seg = pcm[RATE:RATE + 2 * RATE]  # skip the encoder warm-up
    left, right = seg[:, 0], seg[:, 1]

    hi_l = band_energy(left, HIGH_HZ - 600, HIGH_HZ + 600)
    hi_r = band_energy(right, HIGH_HZ - 600, HIGH_HZ + 600)
    lo_l = band_energy(left, LOW_HZ - 200, LOW_HZ + 200)
    ratio_db = 10 * np.log10(hi_l / max(hi_r, 1e-30))
    print(f"{tag:<22} HF L/R separation {ratio_db:7.1f} dB   "
          f"low-band energy {10 * np.log10(max(lo_l, 1e-30)):7.2f} dB")
    return ratio_db, lo_l


def main():
    t = np.arange(SECONDS * RATE) / RATE
    low = 0.35 * np.sin(2 * np.pi * LOW_HZ * t)
    high = 0.35 * np.sin(2 * np.pi * HIGH_HZ * t)
    left = (low + high).astype(np.float32)
    right = low.astype(np.float32)

    source = BUILD / "cpl_probe.wav"
    write_wav_f32(source, left, right)

    results = {}
    for tag, extra in (("without coupling", []), ("with coupling", ["couple"])):
        ac3 = BUILD / f"cpl_probe_{'c' if extra else 'n'}.ac3"
        wav = BUILD / f"cpl_probe_{'c' if extra else 'n'}.wav"
        run([CLI, "encode", source, ac3, "192", *extra])
        # -xerror is required alongside -err_detect: -err_detect alone only
        # controls what the decoder treats as an error internally (concealing
        # a bad frame and moving on) - it does not by itself change ffmpeg's
        # exit code, which run() above is the only thing checking.
        run(["ffmpeg", "-v", "error", "-y", "-xerror", "-err_detect",
             "crccheck+bitstream+buffer+explode", "-i", ac3, "-c:a", "pcm_f32le", wav])
        results[tag] = analyse(tag, wav)
        print(f"{'':22} stream size {ac3.stat().st_size} bytes")

    coupled_ratio, coupled_low = results["with coupling"]
    plain_ratio, plain_low = results["without coupling"]

    # Coupling keeps the envelope difference: left must remain clearly louder
    # in the high band. It will be less separation than uncoupled (that is the
    # trade), but nowhere near collapsed to 0 dB.
    ok = coupled_ratio > 12.0
    # And it must leave the low band - below the coupling frequency - alone.
    low_delta_db = abs(10 * np.log10(coupled_low / max(plain_low, 1e-30)))
    ok &= low_delta_db < 1.0

    print()
    print(f"coupled HF separation {coupled_ratio:.1f} dB (uncoupled {plain_ratio:.1f} dB)")
    print(f"low band unchanged by coupling to within {low_delta_db:.2f} dB")
    print("COUPLING ENVELOPE:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
