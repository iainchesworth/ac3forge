"""Atmos-in-DD+ object encode -> decode round trip: ac3.AtmosEncoder into ac3.Eac3Decoder,
checking that OAMD (object_metadata) and JOC (object_audio) both come back populated.
"""

import numpy as np

import ac3forge as ac3


def _tone(freq_hz, n, sample_rate=48000, amplitude=0.3, phase=0.0):
    t = np.arange(n, dtype=np.float64) / sample_rate
    return (amplitude * np.sin(2 * np.pi * freq_hz * t + phase)).astype(np.float32)


N_OBJECTS = 2
N_FRAMES = 3


def test_atmos_two_objects_roundtrip():
    config = ac3.AtmosConfig(bitrate_kbps=448)
    encoder = ac3.AtmosEncoder(config, N_OBJECTS)
    decoder = ac3.Eac3Decoder()

    placements = [
        ac3.ObjectPlacement(position=ac3.Position(x=0.2, y=0.5, z=0.0), gain=1.0),
        ac3.ObjectPlacement(position=ac3.Position(x=0.8, y=0.5, z=0.5), gain=1.0),
    ]

    saw_objects = False
    for i in range(N_FRAMES):
        objects = [
            _tone(300.0, ac3.SAMPLES_PER_FRAME, phase=0.0),
            _tone(900.0, ac3.SAMPLES_PER_FRAME, phase=1.1),
        ]
        unit = encoder.encode_frame(objects, placements)
        assert isinstance(unit, bytes)
        assert len(unit) > 0

        decoded = decoder.decode_access_unit(unit)
        if decoded is None:
            continue  # transient pre-noise hold-back (§3.7); collected via flush() below
        assert decoded.substream_count == 1

        if decoded.object_metadata is not None and decoded.object_metadata.objects:
            saw_objects = True
            assert len(decoded.object_metadata.objects) == N_OBJECTS
            assert len(decoded.object_audio) == N_OBJECTS
            for obj_audio in decoded.object_audio:
                assert obj_audio.shape == (ac3.SAMPLES_PER_FRAME,)
            assert decoded.object_metadata.program.dynamic_objects == N_OBJECTS

    for flushed in decoder.flush():
        if flushed.object_metadata is not None and flushed.object_metadata.objects:
            saw_objects = True

    assert saw_objects, "expected at least one decoded frame to carry OAMD/JOC object data"


def test_atmos_encoder_reports_dynamic_object_count():
    config = ac3.AtmosConfig(bitrate_kbps=448)
    encoder = ac3.AtmosEncoder(config, N_OBJECTS)
    assert encoder.dynamic_object_count == N_OBJECTS
    assert encoder.program.dynamic_objects == N_OBJECTS
