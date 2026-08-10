"""Does coupling preserve absolute LEVEL, not just channel separation?

The coupling coordinate is a magnitude ratio against the sum of the coupled
channels. Whenever channels partially cancel, that ratio exceeds 1 - and the
transmitted coordinate tops out at 0.96875. An encoder that folds the
reconstruction's x8 headroom into the coupling channel instead of the
coordinate therefore clamps, and the whole coupled band comes out quiet.

This probe drives exactly that case: a high tone present in both channels
with opposite polarity and unequal gain, so the sum is much smaller than
either channel. It reports each channel's level error in the coupled band.

Usage (repo root, after building):  python tools/check_coupling_level.py
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
SECONDS = 3
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


def tone_amplitude(x):
    """Amplitude of the HIGH_HZ component, via a matched projection."""
    n = np.arange(len(x))
    ref = np.exp(-2j * np.pi * HIGH_HZ / RATE * n)
    return float(np.abs(2.0 * np.mean(x * ref)))


def main():
    t = np.arange(SECONDS * RATE) / RATE
    tone = np.sin(2 * np.pi * HIGH_HZ * t)
    # Opposite polarity, unequal gain: sum = 0.5x while left is x, so the
    # required coordinate for left is 2.0 - beyond the 0.96875 field maximum
    # unless the encoder scales the coupling channel per band.
    left = (1.0 * tone * 0.5).astype(np.float32)
    right = (-0.5 * tone * 0.5).astype(np.float32)

    source = BUILD / "cpl_level.wav"
    write_wav_f32(source, left, right)
    reference = read_wav_f32(source)
    ref_l = tone_amplitude(reference[RATE:2 * RATE, 0])
    ref_r = tone_amplitude(reference[RATE:2 * RATE, 1])

    results = {}
    for tag, extra in (("without coupling", []), ("with coupling", ["couple"])):
        ac3 = BUILD / f"cpl_level_{'c' if extra else 'n'}.ac3"
        wav = BUILD / f"cpl_level_{'c' if extra else 'n'}.wav"
        run([CLI, "encode", source, ac3, "192", *extra])
        # -xerror is required alongside -err_detect: -err_detect alone only
        # controls what the decoder treats as an error internally (concealing
        # a bad frame and moving on) - it does not by itself change ffmpeg's
        # exit code, which run() above is the only thing checking.
        run(["ffmpeg", "-v", "error", "-y", "-xerror", "-err_detect",
             "crccheck+bitstream+buffer+explode", "-i", ac3, "-c:a", "pcm_f32le", wav])
        pcm = read_wav_f32(wav)
        got_l = tone_amplitude(pcm[RATE:2 * RATE, 0])
        got_r = tone_amplitude(pcm[RATE:2 * RATE, 1])
        err_l = 20 * np.log10(max(got_l, 1e-12) / ref_l)
        err_r = 20 * np.log10(max(got_r, 1e-12) / ref_r)
        results[tag] = (err_l, err_r)
        print(f"{tag:<18}  L level error {err_l:+6.2f} dB   R level error {err_r:+6.2f} dB")

    err_l, err_r = results["with coupling"]
    ok = abs(err_l) < 1.5 and abs(err_r) < 1.5
    print()
    print("Anti-correlated content requires a coordinate of 2.0 for L; a coupling")
    print("channel scaled by a fixed 1/8 caps it at 0.96875 and loses 6.3 dB.")
    print("COUPLING LEVEL:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
