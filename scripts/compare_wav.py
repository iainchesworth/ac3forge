"""Delay-compensated SNR between two WAVs - stdlib only, no numpy.

Used by scripts/verify-gold-reference.sh as the L4-lite half of the gold-
reference gate: FFmpeg's decode of ac3cli's own encoder output is the
independent reference, ac3cli's own decode of the same file is what is being
checked, and this asserts they agree to within a threshold rather than just
"neither one crashed."

Deliberately stdlib-only (wave/struct/math): GitHub-hosted runners ship
Python 3 preinstalled on Windows/Linux/macOS, so this adds zero new CI
provisioning the way pulling in numpy would.

Usage:  python compare_wav.py <reference.wav> <actual.wav> [--min-snr-db N] [--max-lag-samples N]
Exit 0 if every channel's SNR >= the threshold, exit 1 (with the offending
channel and its SNR) otherwise.

--json-out additionally writes a structured result (per-channel SNR, lag,
threshold, pass/fail) for a caller that wants to persist the numbers rather
than just gate on them - see docs/quality-trend.md. --codec-label and
--bitrate-kbps are pure passthrough metadata for that file; this script does
not care what codec produced its inputs.
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

# Not the stdlib `wave` module: it hard-rejects WAVE_FORMAT_IEEE_FLOAT
# (format tag 3, "unknown format: 3") with no opt-out, and ac3cli's own
# decode writes exactly that (see write_wav_f32 in src/lib/src/io/wav.cpp) -
# confirmed locally, this is not a hypothetical. A small manual RIFF/WAVE
# walk (mirroring that same C++ reader) handles PCM16 and float32 uniformly,
# plus WAVE_FORMAT_EXTENSIBLE, without needing two code paths.


def read_channels(path: Path) -> tuple[list[list[float]], int]:
    data = path.read_bytes()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file")

    fmt_tag = channels = bits = 0
    rate = 0
    payload = b""
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        body_at = pos + 8
        if chunk_id == b"fmt ":
            fmt_tag, channels = struct.unpack_from("<HH", data, body_at)
            rate = struct.unpack_from("<I", data, body_at + 4)[0]
            bits = struct.unpack_from("<H", data, body_at + 14)[0]
            if fmt_tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real tag is in the SubFormat GUID
                fmt_tag = struct.unpack_from("<H", data, body_at + 24)[0]
        elif chunk_id == b"data":
            payload = data[body_at:body_at + chunk_size]
        pos = body_at + chunk_size + (chunk_size & 1)  # chunks are word-aligned

    if channels == 0 or rate == 0 or not payload:
        raise SystemExit(f"{path}: missing fmt/data chunk")

    if fmt_tag == 1 and bits == 16:
        sample_fmt, scale = "h", 32768.0
    elif fmt_tag == 3 and bits == 32:
        sample_fmt, scale = "f", 1.0
    else:
        raise SystemExit(f"{path}: unsupported format tag {fmt_tag}/{bits}-bit")

    sample_size = struct.calcsize(sample_fmt)
    frame_bytes = channels * sample_size
    frames = len(payload) // frame_bytes
    interleaved = struct.unpack(f"<{frames * channels}{sample_fmt}",
                                 payload[:frames * frame_bytes])
    out = [[0.0] * frames for _ in range(channels)]
    for i in range(frames):
        base = i * channels
        for c in range(channels):
            out[c][i] = interleaved[base + c] / scale
    return out, rate


def dot(a: list[float], b: list[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def best_lag(ref_mono: list[float], act_mono: list[float], max_lag: int, probe_len: int) -> int:
    """Cross-correlate a short prefix to find how many samples `act` leads or
    lags `ref` by - decoder priming/lookahead can shift the two by a handful
    of samples even when decoding the identical bitstream, and a search here
    is cheap insurance against penalizing that as a fidelity loss."""
    n = min(probe_len, len(ref_mono), len(act_mono))
    ref_probe = ref_mono[:n]
    best = (0, -1.0)
    for lag in range(-max_lag, max_lag + 1):
        if lag >= 0:
            act_probe = act_mono[lag:lag + n]
        else:
            act_probe = ([0.0] * (-lag) + act_mono)[: n]
        if len(act_probe) < n:
            continue
        score = dot(ref_probe, act_probe)
        if score > best[1]:
            best = (lag, score)
    return best[0]


def align(a: list[float], b: list[float], lag: int) -> tuple[list[float], list[float]]:
    """Shift `b` by `lag` samples relative to `a`, then trim both to the
    overlapping region."""
    if lag >= 0:
        b = b[lag:]
    else:
        a = a[-lag:]
    n = min(len(a), len(b))
    return a[:n], b[:n]


def snr_db(reference: list[float], actual: list[float]) -> float:
    signal_power = sum(v * v for v in reference)
    noise_power = sum((r - a) ** 2 for r, a in zip(reference, actual))
    if noise_power <= 1e-20:
        return math.inf
    if signal_power <= 1e-20:
        return -math.inf
    return 10.0 * math.log10(signal_power / noise_power)


def json_safe_db(value: float) -> float:
    """Clamp +-inf (a bit-exact match, or a degenerate all-zero reference) to a
    finite sentinel. json.dumps happily emits the non-standard `Infinity`
    token, but JSON.parse in the browser rejects it outright, and this file's
    JSON output is meant to be fetched and parsed client-side (see
    docs/quality-trend.md) - so it must stay valid JSON, not just valid
    Python-flavoured JSON."""
    if value == math.inf:
        return 200.0
    if value == -math.inf:
        return -200.0
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("actual", type=Path)
    parser.add_argument("--min-snr-db", type=float, default=20.0)
    parser.add_argument("--max-lag-samples", type=int, default=512)
    parser.add_argument("--probe-samples", type=int, default=20000)
    parser.add_argument("--json-out", type=Path, default=None,
                         help="Also write a structured JSON result here.")
    parser.add_argument("--codec-label", default="",
                         help="Passthrough metadata for --json-out (e.g. 'ac3').")
    parser.add_argument("--bitrate-kbps", type=int, default=0,
                         help="Passthrough metadata for --json-out.")
    args = parser.parse_args()

    ref_channels, ref_rate = read_channels(args.reference)
    act_channels, act_rate = read_channels(args.actual)

    if ref_rate != act_rate:
        print(f"FAIL: sample rate mismatch ({args.reference}={ref_rate} vs {args.actual}={act_rate})")
        return 1
    if len(ref_channels) != len(act_channels):
        print(f"FAIL: channel count mismatch ({args.reference}={len(ref_channels)} "
              f"vs {args.actual}={len(act_channels)})")
        return 1

    ref_mono = [sum(frame) for frame in zip(*ref_channels)]
    act_mono = [sum(frame) for frame in zip(*act_channels)]
    lag = best_lag(ref_mono, act_mono, args.max_lag_samples, args.probe_samples)

    worst = math.inf
    failed = False
    channels_db = []
    for idx, (ref_ch, act_ch) in enumerate(zip(ref_channels, act_channels)):
        r, a = align(ref_ch, act_ch, lag)
        result = snr_db(r, a)
        channels_db.append(result)
        worst = min(worst, result)
        status = "ok" if result >= args.min_snr_db else "FAIL"
        if status == "FAIL":
            failed = True
        print(f"channel {idx}: {result:.2f} dB [{status}]")

    print(f"lag: {lag} samples, worst channel: {worst:.2f} dB, threshold: {args.min_snr_db} dB")

    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps({
            "codec": args.codec_label,
            "bitrate_kbps": args.bitrate_kbps,
            "sample_rate": ref_rate,
            "lag_samples": lag,
            "threshold_db": args.min_snr_db,
            "channels_db": [json_safe_db(v) for v in channels_db],
            "worst_db": json_safe_db(worst),
            "pass": not failed,
        }, indent=2))

    if failed:
        print("FAIL: at least one channel is below the SNR threshold")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
