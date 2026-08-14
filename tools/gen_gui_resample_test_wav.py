"""44.1kHz reference WAV for the GUI's resample-on-load QML test.

Produces fuzz/seeds/fuzz_wav_read/resample-44100.wav: a small, checked-in
stereo PCM16 WAV at 44100 Hz - deliberately a rate that does NOT match
roundtrip-stereo.wav/roundtrip-51.wav's 48000 Hz, since ac3gui's own
addSourceFile() only resamples a second source onto the primary's rate when
the two actually differ. tst_source_loading.qml loads this as the SECOND
source (after one of the 48kHz fixtures as primary) to exercise that path
end to end, checking the resampled row's "44.1->48 k" label and that the
tone survives at the right frequency (silence or a single short click would
give a false pass either way - see CONTRIBUTING.md's "test with real audio"
rule).

Two distinct, multi-cycle tones (one per channel) rather than one tone
duplicated on both, so a channel-mixup bug in the resample path would show
up as measuring the wrong frequency on the wrong channel. Comfortably below
full scale (PEAK below) so nothing here is about clipping - that is a
separate concern this fixture is not trying to cover.

Stdlib-only (no numpy): this only needs to run once, locally, to produce the
checked-in file - not in CI.

Usage (repo root):  python tools/gen_gui_resample_test_wav.py
"""

import math
import struct
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "fuzz" / "seeds" / "fuzz_wav_read" / "resample-44100.wav"

RATE = 44100
DURATION_S = 1.5
LEFT_HZ = 440.0
RIGHT_HZ = 660.0
PEAK = 0.5


def make_channels() -> list[list[float]]:
    n = int(RATE * DURATION_S)
    t = [i / RATE for i in range(n)]
    left = [PEAK * math.sin(2 * math.pi * LEFT_HZ * tt) for tt in t]
    right = [PEAK * math.sin(2 * math.pi * RIGHT_HZ * tt) for tt in t]
    return [left, right]


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
