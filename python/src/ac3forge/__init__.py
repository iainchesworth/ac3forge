"""Python bindings for ac3forge - a clean-room AC-3/E-AC-3 encoder and decoder in C++23,
including the Atmos-in-DD+ object layer (OAMD + JOC).

Everything here is a thin wrapper over the compiled ``_ac3forge`` extension - see
docs/library/python-api.md in the ac3forge repository for the full surface and worked examples.
"""

from ._ac3forge import (
    Ac3DecodeError,
    Ac3EncodeError,
    Ac3Error,
    Acmod,
    AtmosConfig,
    AtmosEncoder,
    BLOCKS_PER_FRAME,
    CentreMixLevel,
    DecodeError,
    DecodedAccessUnit,
    DecodedFrame,
    DecodedProgram,
    DecodedSubstream,
    DecoderConfig,
    DynamicObject,
    Eac3Decoder,
    EncoderConfig,
    FrameDecoder,
    FrameEncoder,
    FrameError,
    HeavyConfig,
    Position,
    ObjectPlacement,
    Profile,
    ProfileId,
    Program,
    SAMPLES_PER_FRAME,
    SampleRate,
    StreamType,
    SurroundMixLevel,
    describe,
    fullbw_channel_count,
    profile_for,
    profile_name,
    sample_rate_hz,
    split_access_units,
    split_frames,
    stream_bsid,
)

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    __version__ = _pkg_version("ac3forge")
except PackageNotFoundError:  # pragma: no cover - an editable/unbuilt checkout
    __version__ = "0.0.0"

__all__ = [
    "Ac3DecodeError",
    "Ac3EncodeError",
    "Ac3Error",
    "Acmod",
    "AtmosConfig",
    "AtmosEncoder",
    "BLOCKS_PER_FRAME",
    "CentreMixLevel",
    "DecodeError",
    "DecodedAccessUnit",
    "DecodedFrame",
    "DecodedProgram",
    "DecodedSubstream",
    "DecoderConfig",
    "DynamicObject",
    "Eac3Decoder",
    "EncoderConfig",
    "FrameDecoder",
    "FrameEncoder",
    "FrameError",
    "HeavyConfig",
    "ObjectPlacement",
    "Position",
    "Profile",
    "ProfileId",
    "Program",
    "SAMPLES_PER_FRAME",
    "SampleRate",
    "StreamType",
    "SurroundMixLevel",
    "__version__",
    "describe",
    "fullbw_channel_count",
    "profile_for",
    "profile_name",
    "sample_rate_hz",
    "split_access_units",
    "split_frames",
    "stream_bsid",
]
