"""Where does FFmpeg's AC-3 advantage actually come from?

This project's AC-3 encoder scores about 3 dB below FFmpeg's at matched
bitrate on the landscape leg (`ac3-51-448`). Two structural explanations are
visible in src/lib/src/encoder/encoder.cpp, and they are not distinguishable
from a single overall SNR number:

  shared fine SNR offset   Every channel is written the same `fsnroffst`
                           (~line 945) even though A/52 gives each its own
                           4-bit field, and the search optimises one
                           frame-wide composite offset. Bits cannot move
                           between channels, so an easy channel is served
                           more precision than it needs while a demanding
                           one is starved.

  fixed allocation codes   `const BitAllocCodes codes{}` (~line 374) pins
                           sdcycod/fdcycod/sgaincod/dbpbcod/floorcod to the
                           §8.2.12 basic-encoder defaults. Those shape the
                           masking curve itself and are never searched.

The two leave different fingerprints, which is what this measures:

  * A shared offset misallocates BETWEEN CHANNELS. Its deficit should vary
    channel to channel - large where a channel is demanding, small or even
    negative where it is easy - while looking similar across frequency.
  * A mis-shaped masking curve misallocates ACROSS FREQUENCY. Its deficit
    should vary band to band in the same pattern for every channel.

So both axes are reported: per-channel SNR, and per-band SNR averaged over
channels. Whichever axis carries the spread is the one worth fixing.

Deliberately NOT a pass/fail gate. It answers "which lever", and the answer
changes as the encoder does; `tools/quality_race.py ci` remains the thing
with thresholds in it.

Both streams are decoded with THIS project's decoder, not FFmpeg's, so the
comparison isolates the two ENCODERS - a decoder difference would otherwise
land in the same number and be indistinguishable from an allocation one.

Usage (repo root, after building):
  python tools/check_ac3_allocation.py [--rate 448] [--cli path]
"""

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import quality_race as qr  # noqa: E402  (CLI path, WAV IO, alignment)

REPO = Path(__file__).resolve().parent.parent
SOURCE = REPO / "tests" / "golden" / "audio" / "reference_51.wav"
# reference_51.wav's channel order, for labelling only.
CHANNELS = ["L", "R", "C", "LFE", "Ls", "Rs"]

# Octave-ish edges in Hz. Wide enough that each band holds plenty of energy on
# 2.5 s of material, narrow enough to show a masking curve leaning one way.
BAND_EDGES = [0, 500, 1000, 2000, 4000, 8000, 12000, 16000, 24000]


def encode_ours(cli: str, out: Path, rate: int) -> None:
    qr.run([cli, "encode", str(SOURCE), str(out), str(rate)])


def encode_ffmpeg(out: Path, rate: int) -> None:
    qr.run(["ffmpeg", "-v", "error", "-y", "-i", str(SOURCE),
            "-c:a", "ac3", "-b:a", f"{rate}k", str(out)])


def decode_ours(cli: str, coded: Path, wav: Path) -> np.ndarray:
    qr.run([cli, "decode", str(coded), str(wav)])
    return qr.read_wav_f32(wav)


def per_channel_snr(original: np.ndarray, decoded: np.ndarray) -> np.ndarray:
    """SNR in dB for each channel, on the common aligned overlap.

    Aligned once on channel 0 and that lag applied to all of them, rather
    than per channel: a per-channel lag would silently absorb a real
    inter-channel timing error into the alignment and report it as clean.
    """
    o, d, _ = qr.align(original, decoded, **qr.FIXED_ALIGN)
    noise = d - o
    return np.array([
        10 * np.log10(np.sum(o[:, c] ** 2) / max(np.sum(noise[:, c] ** 2), 1e-30))
        for c in range(o.shape[1])
    ])


def per_band_snr(original: np.ndarray, decoded: np.ndarray) -> np.ndarray:
    """SNR in dB per frequency band, summed over channels.

    Signal and noise are accumulated across channels BEFORE the ratio, so a
    band's number is the whole frame's behaviour there rather than a mean of
    per-channel ratios (which one near-silent channel could dominate).
    """
    o, d, _ = qr.align(original, decoded, **qr.FIXED_ALIGN)
    noise = d - o
    freqs = np.fft.rfftfreq(o.shape[0], 1.0 / qr.RATE)
    out = []
    for low, high in zip(BAND_EDGES, BAND_EDGES[1:]):
        mask = (freqs >= low) & (freqs < high)
        sig = err = 0.0
        for c in range(o.shape[1]):
            sig += np.sum(np.abs(np.fft.rfft(o[:, c])[mask]) ** 2)
            err += np.sum(np.abs(np.fft.rfft(noise[:, c])[mask]) ** 2)
        out.append(10 * np.log10(sig / max(err, 1e-30)))
    return np.array(out)


def spread(values: np.ndarray) -> float:
    return float(np.max(values) - np.min(values))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rate", type=int, default=448)
    parser.add_argument("--cli", default=str(qr.CLI))
    args = parser.parse_args()

    cli = args.cli
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")

    build = qr.BUILD
    build.mkdir(parents=True, exist_ok=True)
    original = qr.read_wav_any(SOURCE)

    ours, theirs = build / f"alloc_ours_{args.rate}.ac3", build / f"alloc_ff_{args.rate}.ac3"
    encode_ours(cli, ours, args.rate)
    encode_ffmpeg(theirs, args.rate)
    ours_pcm = decode_ours(cli, ours, build / f"alloc_ours_{args.rate}.wav")
    theirs_pcm = decode_ours(cli, theirs, build / f"alloc_ff_{args.rate}.wav")

    print(f"AC-3 5.1 @ {args.rate} kbps, {SOURCE.name}, both decoded by ac3cli\n")

    ours_ch, theirs_ch = per_channel_snr(original, ours_pcm), per_channel_snr(original, theirs_pcm)
    print(f"{'channel':<9}{'ours':>9}{'ffmpeg':>9}{'deficit':>9}")
    print("-" * 36)
    for i, name in enumerate(CHANNELS[:len(ours_ch)]):
        print(f"{name:<9}{ours_ch[i]:>9.2f}{theirs_ch[i]:>9.2f}{ours_ch[i] - theirs_ch[i]:>+9.2f}")
    ch_deficit = ours_ch - theirs_ch
    print(f"{'spread':<9}{'':>9}{'':>9}{spread(ch_deficit):>9.2f}")

    ours_bd, theirs_bd = per_band_snr(original, ours_pcm), per_band_snr(original, theirs_pcm)
    print(f"\n{'band':<14}{'ours':>9}{'ffmpeg':>9}{'deficit':>9}")
    print("-" * 41)
    for i, (low, high) in enumerate(zip(BAND_EDGES, BAND_EDGES[1:])):
        label = f"{low/1000:g}-{high/1000:g}k"
        print(f"{label:<14}{ours_bd[i]:>9.2f}{theirs_bd[i]:>9.2f}"
              f"{ours_bd[i] - theirs_bd[i]:>+9.2f}")
    bd_deficit = ours_bd - theirs_bd
    print(f"{'spread':<14}{'':>9}{'':>9}{spread(bd_deficit):>9.2f}")

    print("\nreading it")
    print(f"  overall deficit      {np.mean(ch_deficit):+.2f} dB")
    print(f"  spread across channels {spread(ch_deficit):.2f} dB")
    print(f"  spread across bands    {spread(bd_deficit):.2f} dB")
    if spread(ch_deficit) > spread(bd_deficit):
        print("  -> the deficit is mostly BETWEEN CHANNELS: the shared fine SNR offset")
        print("     (encoder.cpp ~945) is the lever - bits cannot move between channels.")
    else:
        print("  -> the deficit is mostly ACROSS FREQUENCY: the fixed BitAllocCodes")
        print("     (encoder.cpp ~374) are the lever - the masking curve is mis-shaped.")
    print("  Neither is proof on its own; it says which one to try first.")


if __name__ == "__main__":
    main()
