"""Encode a 5.1 layout to AC-3 and decode it straight back.

Mirrors examples/encode_ac3.cpp in spirit (same kind of real, distinct-per-channel audio,
per CONTRIBUTING.md's own "silence is not a test signal" rule) but through the Python bindings
(roadmap F2), and adds the decode half so it demonstrates the full round trip in one script.
docs/library/python-api.md excerpts the functions below directly - this file is what the build
actually runs, not a snippet copied by hand.

Run with: python examples/python/encode_decode_roundtrip.py
"""

from __future__ import annotations

import math

import numpy as np

import ac3forge as ac3

TONES_HZ = [1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0]  # L, C, R, Ls, Rs, LFE
SAMPLE_RATE = 48000
FRAME_COUNT = 31  # ~48000 / 1536 samples, one second


def build_channel(tone_hz: float, frame: int) -> np.ndarray:
    """One frame (ac3.SAMPLES_PER_FRAME samples) of a sine tone, continuous across frames."""
    n = np.arange(ac3.SAMPLES_PER_FRAME, dtype=np.float64)
    t = (frame * ac3.SAMPLES_PER_FRAME + n) / SAMPLE_RATE
    return (0.5 * np.sin(2.0 * math.pi * tone_hz * t)).astype(np.float32)


def encode_stream() -> bytes:
    encoder = ac3.FrameEncoder(
        ac3.EncoderConfig(bitrate_kbps=448, acmod=ac3.Acmod.k3_2, lfe=True)
    )
    stream = bytearray()
    for frame in range(FRAME_COUNT):
        channels = [build_channel(tone, frame) for tone in TONES_HZ]
        stream += encoder.encode_frame(channels)
    return bytes(stream)


def decode_stream(stream: bytes) -> None:
    decoder = ac3.FrameDecoder()
    for i, frame_bytes in enumerate(ac3.split_frames(stream)):
        decoded = decoder.decode_frame(frame_bytes)
        if i == 0:
            print(f"channels: {decoded.channel_labels}")
            print(f"sample rate: {ac3.sample_rate_hz(decoded.sample_rate)} Hz")


def main() -> None:
    stream = encode_stream()
    print(f"{len(stream)} bytes of AC-3 ({FRAME_COUNT} frames)")
    decode_stream(stream)


if __name__ == "__main__":
    main()
