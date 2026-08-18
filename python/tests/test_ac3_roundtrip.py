"""AC-3 encode -> decode round trip through the Python bindings.

Follows the C++ suite's own validation discipline (see CONTRIBUTING.md): real tone content, not
silence or DC: distinct content per channel, so a channel-order bug would show up as a failed
correlation rather than passing by coincidence: and at least a few frames, comparing from frame 1
onward, because frame 0's MDCT overlap-add history starts at zero and is not representative (see
[[feedback-verify-transform-delay-empirically]] and CONTRIBUTING.md's own "frame 0 is not a test"
warning). Comparison is correlation-over-a-small-alignment-window rather than sample-exact SNR,
deliberately: the MDCT/IMDCT pair carries a real, and real-but-unstated-here, block of delay, and
this test cares about "the codec faithfully reproduced the tone", not the exact delay in samples.
"""

import numpy as np
import pytest

import ac3forge as ac3


def _tone(freq_hz, n, sample_rate=48000, amplitude=0.2, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


def _best_correlation(ref, test, max_shift=64):
    best = -1.0
    for shift in range(-max_shift, max_shift + 1):
        if shift >= 0:
            a, b = ref[: len(ref) - shift], test[shift:]
        else:
            a, b = ref[-shift:], test[: len(test) + shift]
        if len(a) < 256:
            continue
        a = a - a.mean()
        b = b - b.mean()
        denom = np.sqrt(np.sum(a * a) * np.sum(b * b))
        if denom < 1e-12:
            continue
        best = max(best, float(np.sum(a * b) / denom))
    return best


N_FRAMES = 4


def _encode_decode(acmod, channel_tones):
    config = ac3.EncoderConfig(acmod=acmod, bitrate_kbps=192)
    encoder = ac3.FrameEncoder(config)
    decoder = ac3.FrameDecoder()

    total = N_FRAMES * ac3.SAMPLES_PER_FRAME
    originals = [_tone(freq, total, phase=phase) for freq, phase in channel_tones]

    decoded_channels = [[] for _ in originals]
    for i in range(N_FRAMES):
        start, end = i * ac3.SAMPLES_PER_FRAME, (i + 1) * ac3.SAMPLES_PER_FRAME
        frame_bytes = encoder.encode_frame([o[start:end] for o in originals])
        assert isinstance(frame_bytes, bytes)
        assert len(frame_bytes) > 0

        decoded = decoder.decode_frame(frame_bytes)
        assert decoded.acmod == acmod
        assert len(decoded.channels) == len(originals)
        for ch, buf in enumerate(decoded.channels):
            assert buf.shape == (ac3.SAMPLES_PER_FRAME,)
            decoded_channels[ch].append(buf)

    return originals, decoded_channels


def test_stereo_roundtrip_distinct_channels():
    originals, decoded = _encode_decode(ac3.Acmod.k2_0, [(440.0, 0.0), (523.25, 0.7)])

    for ch, original in enumerate(originals):
        for i in range(1, N_FRAMES):  # skip frame 0 - cold MDCT overlap, not representative
            start, end = i * ac3.SAMPLES_PER_FRAME, (i + 1) * ac3.SAMPLES_PER_FRAME
            corr = _best_correlation(original[start:end], decoded[ch][i])
            assert corr > 0.9, f"channel {ch} frame {i}: correlation {corr:.3f}"


def test_51_roundtrip_channel_labels():
    tones = [(220.0, 0.0), (330.0, 0.1), (440.0, 0.2), (150.0, 0.3), (200.0, 0.4)]
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k3_2, lfe=True, bitrate_kbps=384)
    encoder = ac3.FrameEncoder(config)
    decoder = ac3.FrameDecoder()

    channels = [_tone(freq, ac3.SAMPLES_PER_FRAME, phase=phase) for freq, phase in tones]
    lfe = _tone(60.0, ac3.SAMPLES_PER_FRAME, amplitude=0.1)
    frame_bytes = encoder.encode_frame([*channels, lfe])

    decoded = decoder.decode_frame(frame_bytes)
    assert decoded.acmod == ac3.Acmod.k3_2
    assert decoded.lfe is True
    assert decoded.channel_labels == ["L", "C", "R", "Ls", "Rs", "LFE"]
    assert len(decoded.channels) == 6


def test_prove_correlation_check_can_fail():
    # CONTRIBUTING.md's "prove the test can fail" discipline: comparing a channel against pure
    # noise (rather than its own decoded reconstruction) must NOT pass the same correlation bar.
    original = _tone(440.0, ac3.SAMPLES_PER_FRAME)
    rng = np.random.default_rng(0)
    noise = (0.2 * rng.standard_normal(ac3.SAMPLES_PER_FRAME)).astype(np.float32)
    assert _best_correlation(original, noise) < 0.5


def test_decoder_default_construction():
    decoder = ac3.FrameDecoder(ac3.DecoderConfig(drc_scale=1.0))
    assert decoder is not None
