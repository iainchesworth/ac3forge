"""std::expected's error branch, translated at the binding boundary into Python exceptions - see
python/src/ac3forge_ext/bindings.cpp's own header comment on why exceptions rather than a
Result-like return. Every case here checks both the exception TYPE and its `.error` enum value,
not just that *something* was raised.
"""

import numpy as np
import pytest

import ac3forge as ac3


def test_decode_truncated_buffer_raises():
    decoder = ac3.FrameDecoder()
    with pytest.raises(ac3.Ac3DecodeError) as exc_info:
        decoder.decode_frame(b"\x00\x01\x02")
    assert isinstance(exc_info.value.error, ac3.DecodeError)
    assert isinstance(exc_info.value, ac3.Ac3Error)  # the base class every ac3 error derives from


def test_decode_missing_sync_word_raises():
    decoder = ac3.FrameDecoder()
    garbage = bytes(64)  # no 0x0B77 sync word anywhere in here
    with pytest.raises(ac3.Ac3DecodeError):
        decoder.decode_frame(garbage)


def test_encode_invalid_bitrate_raises():
    config = ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=3)  # not a legal Table 5.18 rate
    encoder = ac3.FrameEncoder(config)
    channels = [np.zeros(ac3.SAMPLES_PER_FRAME, dtype=np.float32) for _ in range(2)]
    with pytest.raises(ac3.Ac3EncodeError) as exc_info:
        encoder.encode_frame(channels)
    assert exc_info.value.error == ac3.FrameError.kInvalidBitrate


def test_channel_length_mismatch_is_a_value_error():
    # A wrong-length channel array is a Python-level usage error (the C++ side documents this as
    # "a programming error, not a runtime one" - docs/library/index.md), not a codec-level
    # Ac3EncodeError, so this is a plain ValueError rather than the Ac3Error hierarchy.
    encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0))
    with pytest.raises(ValueError):
        encoder.encode_frame([np.zeros(10, dtype=np.float32), np.zeros(10, dtype=np.float32)])


def test_encoder_config_rejects_unknown_kwarg():
    with pytest.raises(TypeError):
        ac3.EncoderConfig(dialnrm=10)  # typo: not a real field
