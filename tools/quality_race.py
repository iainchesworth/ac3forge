"""Quality race: our encoder vs FFmpeg's, at matched bitrates.

Synthesizes stereo program material (tones with vibrato, a sweep, filtered
noise, correlated near-mono content for rematrixing, tone bursts), encodes it
with both encoders, decodes both with FFmpeg (the neutral referee), aligns by
cross-correlation, and reports SNR vs the original.

Modes:
  ac3        - our AC-3 encoder vs FFmpeg's, at 192-448 kbps
  eac3       - our E-AC-3 encoder, one row per Annex E tool set, vs FFmpeg's
               E-AC-3 encoder, at the low rates the tools exist to serve
  eac3-51    - the same for 5.1, with genuinely decorrelated channels
  seam       - where the spectral extension notch lands, and how deep
  crosscheck - every tool set through BOTH decoders: FFmpeg and Dolby's own,
               via the reference player's GStreamer elements. Agreeing with
               one decoder says a stream is readable; agreeing with the
               reference implementation says it is right. Skips gracefully
               when the reference player is not installed.

Usage (repo root, after building):  python tools/quality_race.py [mode]
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
CLI = BUILD / "dev" / "bin" / "ac3cli.exe"
RATE = 48000
SEG = 2 * RATE  # 2 s per segment
# Bin 85, sub-band 4: the lowest the encoder will ever start coupling, so
# everything below it is coded per channel whatever the rate.
CPL_HZ = 85 * (RATE / 512.0)


def make_material():
    rng = np.random.default_rng(0x0B77)
    t = np.arange(SEG) / RATE
    segments = []

    # a) chord with vibrato
    vib = 1.0 + 0.002 * np.sin(2 * np.pi * 5.0 * t)
    chord = sum(np.sin(2 * np.pi * f * vib * t) for f in (220.0, 277.2, 329.6, 440.0))
    left = 0.15 * chord + 0.02 * np.sin(2 * np.pi * 1234.0 * t)
    right = 0.15 * chord + 0.02 * np.sin(2 * np.pi * 987.0 * t)
    segments.append((left, right))

    # b) sweep 100 -> 8000 Hz
    phase = 2 * np.pi * (100.0 * t + (8000.0 - 100.0) / (2 * t[-1]) * t * t)
    segments.append((0.4 * np.sin(phase), 0.4 * np.sin(phase * 1.0005)))

    # c) band-limited noise (pink-ish via cumulative smoothing)
    w = rng.standard_normal(SEG + 512)
    kernel = np.hanning(64)
    smooth = np.convolve(w, kernel / kernel.sum(), mode="same")[:SEG]
    noise = 0.3 * smooth / np.max(np.abs(smooth))
    segments.append((noise, 0.9 * noise + 0.1 * rng.standard_normal(SEG) * 0.05))

    # d) near-mono speech-ish (correlated -> rematrix territory)
    carrier = np.sin(2 * np.pi * 180.0 * t) * (0.5 + 0.5 * np.sin(2 * np.pi * 3.0 * t))
    formants = sum(0.3 * np.sin(2 * np.pi * f * t) for f in (700.0, 1220.0, 2600.0))
    mono = 0.25 * carrier * (1.0 + 0.3 * formants)
    segments.append((mono, mono * 0.98))

    # e) tone bursts (transient-ish, long-block stress)
    burst = np.zeros(SEG)
    for k in range(8):
        at = k * SEG // 8
        n = np.arange(4096)
        burst[at:at + 4096] += 0.5 * np.sin(2 * np.pi * 1500.0 * n / RATE) * np.exp(-n / 800.0)
    segments.append((burst, burst[::-1].copy()))

    left = np.concatenate([s[0] for s in segments]).astype(np.float32)
    right = np.concatenate([s[1] for s in segments]).astype(np.float32)
    return np.clip(left, -0.98, 0.98), np.clip(right, -0.98, 0.98)


def make_material_51():
    """Six DECORRELATED channels, in WAV order (FL FR FC LFE BL BR).

    An upmix would be the wrong test: coupling exists to exploit that channels
    share a high-frequency envelope but not a waveform, and correlated
    channels make it look better than it is. These share program material but
    differ in level, delay and detuning, which is what real multichannel
    content does.
    """
    rng = np.random.default_rng(0x0B77 + 51)
    left, right = make_material()
    n = left.size
    t = np.arange(n) / RATE
    centre = 0.6 * (left + right) / 2 + 0.15 * np.sin(2 * np.pi * 620.0 * t)
    lfe = 0.5 * np.sin(2 * np.pi * 45.0 * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.7 * t))
    # Surrounds: delayed, detuned and noise-dusted, so nothing above the
    # coupling frequency lines up with the fronts.
    delay = 719
    back_l = 0.55 * np.roll(right, delay) + 0.05 * rng.standard_normal(n)
    back_r = 0.55 * np.roll(left, -delay) + 0.05 * rng.standard_normal(n)
    channels = [left, right, centre, lfe, back_l, back_r]
    return [np.clip(c, -0.98, 0.98).astype(np.float32) for c in channels]


def write_wav_f32(path, *channels):
    if len(channels) == 1:
        channels = channels[0]
    count = len(channels)
    data = np.empty(channels[0].size * count, dtype=np.float32)
    for i, channel in enumerate(channels):
        data[i::count] = channel
    payload = data.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, count, RATE, RATE * 4 * count,
                                      4 * count, 32))
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
        raise SystemExit(f"command failed: {' '.join(map(str, cmd))}\n{result.stderr}")


def align(original, decoded):
    """Align by cross-correlation on a probe window; return the overlap."""
    probe = original[RATE:RATE + 32768, 0]
    window = decoded[: RATE + 65536, 0]
    corr = np.correlate(window, probe, mode="valid")
    lag = int(np.argmax(np.abs(corr))) - RATE
    n = min(len(original), len(decoded) - lag) - 2 * RATE
    o = original[RATE:RATE + n - RATE]
    d = decoded[RATE + lag:RATE + lag + len(o)]
    return o, d, lag


def aligned_snr(original, decoded):
    o, d, lag = align(original, decoded)
    noise = d - o
    return 10 * np.log10(np.sum(o**2) / max(np.sum(noise**2), 1e-30))


NFFT = 1024
_HANN = np.hanning(NFFT)


def _spectrogram(x):
    """Magnitude-squared STFT of one channel, frames along axis 0."""
    hop = NFFT // 2
    count = (len(x) - NFFT) // hop
    frames = np.lib.stride_tricks.as_strided(
        x, shape=(count, NFFT), strides=(x.strides[0] * hop, x.strides[0]))
    return np.abs(np.fft.rfft(frames * _HANN, axis=1)) ** 2


def _bark_bands():
    """Band edges (rfft bin indices) on a Bark-like scale up to Nyquist."""
    hz = np.fft.rfftfreq(NFFT, 1.0 / RATE)
    bark = 13 * np.arctan(0.00076 * hz) + 3.5 * np.arctan((hz / 7500.0) ** 2)
    edges = [0]
    for step in np.linspace(bark[1], bark[-1], 25)[1:]:
        edges.append(int(np.searchsorted(bark, step)))
    return [(a, b) for a, b in zip(edges, edges[1:]) if b > a]


BANDS = _bark_bands()


def spectral_scores(o, d):
    """Log-spectral distance, and the high-band energy ratio, both in dB.

    Waveform SNR is the wrong lens for parametric tools: coupling replaces a
    channel's high band with a scaled copy of a shared one, and spectral
    extension synthesizes it outright, so both destroy the waveform there by
    construction while preserving the banded envelope, which is what they set
    out to preserve and what a listener hears. LSD scores that envelope; the
    HF ratio says whether the top of the spectrum is present at all.
    """
    lsd = []
    hf = int(10000.0 / (RATE / NFFT))
    hf_o = hf_d = 0.0
    for c in range(o.shape[1]):
        so = _spectrogram(np.ascontiguousarray(o[:, c]))
        sd = _spectrogram(np.ascontiguousarray(d[:, c]))
        hf_o += so[:, hf:].sum()
        hf_d += sd[:, hf:].sum()
        # Ignore near-silent frames: their band ratios are dominated by the
        # floor and would swamp the average with meaningless dBs.
        energy = so.sum(axis=1)
        loud = energy > 1e-6 * max(energy.max(), 1e-30)
        for lo, hi in BANDS:
            eo = so[loud, lo:hi].sum(axis=1) + 1e-12
            ed = sd[loud, lo:hi].sum(axis=1) + 1e-12
            lsd.append(np.mean(np.abs(10 * np.log10(ed / eo))))
    return float(np.mean(lsd)), 10 * np.log10((hf_d + 1e-20) / (hf_o + 1e-20))


def decode_scores(original, coded, wav_path, strict=True):
    """Decode with FFmpeg (the neutral referee) and score against the source."""
    cmd = ["ffmpeg", "-v", "error", "-y"]
    if strict:
        # Only our own output gets the strict reader: a frame-layout error
        # shows up here as a CRC failure rather than as quiet noise.
        cmd += ["-err_detect", "crccheck+bitstream+buffer+explode"]
    run(cmd + ["-i", coded, "-c:a", "pcm_f32le", wav_path])
    o, d, _ = align(original, read_wav_f32(wav_path))
    snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
    lsd, hf = spectral_scores(o, d)
    return snr, lsd, hf


def measured_kbps(path, seconds):
    return Path(path).stat().st_size * 8 / seconds / 1000.0


def race_ac3(original, source, seconds):
    print(f"{'kbps':>5} | {'ours dB':>8} | {'ffmpeg dB':>9} | {'gap':>6}")
    print("-" * 38)
    worst_gap = -1e9
    for kbps in (192, 256, 320, 448):
        ours = BUILD / f"race_ours_{kbps}.ac3"
        theirs = BUILD / f"race_ff_{kbps}.ac3"
        run([CLI, "encode", source, ours, str(kbps)])
        run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "ac3",
             "-b:a", f"{kbps}k", theirs])
        ours_snr, _, _ = decode_scores(original, ours, BUILD / f"race_ours_{kbps}.wav")
        ff_snr, _, _ = decode_scores(original, theirs, BUILD / f"race_ff_{kbps}.wav",
                                     strict=False)
        gap = ff_snr - ours_snr
        worst_gap = max(worst_gap, gap)
        print(f"{kbps:>5} | {ours_snr:>8.2f} | {ff_snr:>9.2f} | {gap:>+6.2f}")
    print(f"\nworst gap vs ffmpeg: {worst_gap:+.2f} dB (positive = ffmpeg better)")


# One column per E-AC-3 variant: the label, and the tool token handed to
# `ac3cli eac3-encode`. "none" is the tool-free coding path the Annex E tools
# have to beat to earn their place.
EAC3_VARIANTS = [("none", None), ("cpl", "cpl"), ("spx", "spx"), ("aht", "aht"),
                 ("cpl+spx", "cpl+spx"), ("all", "all")]


def race_eac3(original, source, seconds, rates=(96, 128, 192)):
    print(f"{'kbps':>5} | {'variant':<10} | {'SNR dB':>7} | {'LSD dB':>6} | "
          f"{'HF dB':>6} | {'rate':>6}")
    print("-" * 60)
    for kbps in rates:
        for label, tools in EAC3_VARIANTS + [("ffmpeg", "ffmpeg")]:
            coded = BUILD / f"race_e_{label}_{kbps}.ec3"
            if tools == "ffmpeg":
                run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "eac3",
                     "-b:a", f"{kbps}k", coded])
            else:
                cmd = [CLI, "eac3-encode", source, coded, str(kbps)]
                if tools:
                    cmd.append(tools)
                run(cmd)
            snr, lsd, hf = decode_scores(original, coded,
                                         BUILD / f"race_e_{label}_{kbps}.wav",
                                         strict=tools != "ffmpeg")
            rate = measured_kbps(coded, seconds)
            print(f"{kbps:>5} | {label:<10} | {snr:>7.2f} | {lsd:>6.2f} | "
                  f"{hf:>+6.1f} | {rate:>6.1f}")
        print()


DRP = Path(r"C:\Program Files\Dolby\Dolby Reference Player")


def dolby_decode(coded, wav):
    """Decode with Dolby's own decoder, through the reference player's
    GStreamer elements. Returns False when the player is not installed.

    This is a better oracle than FFmpeg for the Annex E tools, because it is
    the implementation the standard is written to describe rather than one
    more reading of it. It is also the only one that can settle a question
    FFmpeg cannot: whether a tool it does not implement is being emitted
    correctly.
    """
    launch = DRP / "gst-launch-1.0.exe"
    if not launch.exists():
        return False
    env = dict(os.environ)
    env["GST_PLUGIN_PATH"] = str(DRP / "gst-plugins")
    env["PATH"] = f"{DRP};{env.get('PATH', '')}"
    # gst-launch parses one pipeline token per argv entry, so "filesrc" and
    # its property have to arrive separately.
    result = subprocess.run(
        [str(launch), "-q", "filesrc", f"location={Path(coded).as_posix()}",
         "!", "dlbac3parse", "!", "dlbac3dec", "!", "audioconvert",
         "!", "audio/x-raw,format=F32LE", "!", "wavenc",
         "!", "filesink", f"location={Path(wav).as_posix()}"],
        capture_output=True, text=True, env=env)
    if result.returncode != 0:
        print(f"  (dolby decode failed: {result.stderr.strip().splitlines()[:1]})")
        return False
    return Path(wav).exists()


def agreement_db(a, b):
    """How closely two decodings of the same bits match, in dB."""
    a, b = a[:, 0].astype(np.float64), b[:, 0].astype(np.float64)
    probe = b[200000:232768]
    corr = np.correlate(a[199000:234000], probe, mode="valid")
    lag = int(np.argmax(np.abs(corr))) - 1000
    n = min(len(b), len(a) - lag) - 300000
    if n < RATE:
        return float("nan"), lag
    diff = a[200000 + lag:200000 + lag + n] - b[200000:200000 + n]
    return 10 * np.log10(np.sum(b[200000:200000 + n] ** 2)
                         / max(np.sum(diff ** 2), 1e-30)), lag


def crosscheck(original, source):
    """Put every tool set through both decoders and compare them.

    Agreeing with one decoder proves the stream is readable. Agreeing with
    two independent ones - where the second is the reference implementation -
    is the difference between "FFmpeg accepts this" and "this is right".
    """
    print(f"{'tools':<10} | {'ffmpeg dB':>9} | {'dolby dB':>8} | {'agree dB':>8}")
    print("-" * 46)
    for tools in ("none", "cpl", "spx", "aht", "all"):
        coded = BUILD / f"x_{tools}.ec3"
        cmd = [CLI, "eac3-encode", source, coded, "128"]
        if tools != "none":
            cmd.append(tools)
        run(cmd)
        ff_wav = BUILD / f"x_{tools}_ff.wav"
        run(["ffmpeg", "-v", "error", "-y", "-err_detect",
             "crccheck+bitstream+buffer+explode", "-i", coded,
             "-c:a", "pcm_f32le", ff_wav])
        ff = read_wav_f32(ff_wav)
        o, d, _ = align(original, ff)
        ff_snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
        dlb_wav = BUILD / f"x_{tools}_dlb.wav"
        if not dolby_decode(coded, dlb_wav):
            print(f"{tools:<10} | {ff_snr:>9.2f} | {'n/a':>8} | {'n/a':>8}")
            continue
        dlb = read_wav_f32(dlb_wav)
        o, d, _ = align(original, dlb)
        dlb_snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
        agree, _ = agreement_db(dlb, ff)
        print(f"{tools:<10} | {ff_snr:>9.2f} | {dlb_snr:>8.2f} | {agree:>8.1f}")


def seam_check(source, spxbegf=4):
    """Is the spectral extension notch where the standard says, and how deep?

    The banded scores cannot see this: the notch removes energy from a handful
    of bins and the band's own scale factor puts it straight back, so LSD and
    SNR move by hundredths either way. What CAN be checked is the decoded
    spectrum itself - the dip has to sit on the first synthesized coefficient
    and deepen with spxattencod. That also answers whether the decoder
    implements the tool at all, which is not a given.
    """
    seam_bin = 25 + 12 * (spxbegf + 2 if spxbegf < 6 else spxbegf * 2 - 3)
    seam_hz = seam_bin * (RATE / 2) / 256
    nfft = 4096
    depths = {}
    for code in (None, 2, 8, 16, 31):
        label = "off" if code is None else f"cod{code}"
        tools = f"spx:{spxbegf}+" + ("noatten" if code is None else f"atten:{code}")
        coded = BUILD / f"seam_{label}.ec3"
        run([CLI, "eac3-encode", source, coded, "128", tools])
        wav = BUILD / f"seam_{label}.wav"
        run(["ffmpeg", "-v", "error", "-y", "-i", coded, "-c:a", "pcm_f32le", wav])
        d = read_wav_f32(wav)[RATE:-RATE, 0]
        frames = len(d) // nfft
        acc = np.zeros(nfft // 2 + 1)
        for i in range(frames):
            acc += np.abs(np.fft.rfft(d[i * nfft:(i + 1) * nfft] * np.hanning(nfft))) ** 2
        depths[label] = acc / frames
    freqs = np.fft.rfftfreq(nfft, 1.0 / RATE)
    base = depths["off"]
    print(f"spectral extension notch, seam at coefficient {seam_bin} = {seam_hz:.0f} Hz")
    print(f"{'spxattencod':>12} | {'notch dB':>9} | {'at Hz':>7}")
    for label, spec in depths.items():
        if label == "off":
            continue
        rel = 10 * np.log10(np.maximum(spec, 1e-30) / np.maximum(base, 1e-30))
        window = (freqs > seam_hz - 500) & (freqs < seam_hz + 500)
        at = int(np.argmin(np.where(window, rel, 0.0)))
        print(f"{label:>12} | {rel[at]:>9.2f} | {freqs[at]:>7.0f}")
        # The dip has to land on the seam, not somewhere else in the band.
        if abs(freqs[at] - seam_hz) > 250:
            print(f"  WARNING: notch is {freqs[at] - seam_hz:+.0f} Hz off the seam")




CPL_HZ = 85 * (RATE / 512.0)


def snr_db(o, d):
    noise = d - o
    return 10 * np.log10(np.sum(o**2) / max(np.sum(noise**2), 1e-30))


def band_measures(o, d, size=2048):
    """SNR below the coupling frequency, level error above it.

    Above it a coupled decoder restores the band's envelope rather than its
    waveform, so a waveform SNR up there measures the tool's premise, not its
    correctness. The level per short window is the thing that must survive -
    and the thing a coupling coordinate carrying the wrong block's scale
    destroys, in exactly the three blocks of six that reuse a coordinate.
    """
    hop = size // 2
    win = np.hanning(size)
    freqs = np.fft.rfftfreq(size, 1.0 / RATE)
    low = freqs < CPL_HZ
    high = ~low
    low_signal = 0.0
    low_noise = 0.0
    pairs = []
    for start in range(0, len(o) - size, hop):
        for ch in range(o.shape[1]):
            spec_o = np.fft.rfft(o[start:start + size, ch] * win)
            spec_d = np.fft.rfft(d[start:start + size, ch] * win)
            low_signal += float(np.sum(np.abs(spec_o[low]) ** 2))
            low_noise += float(np.sum(np.abs(spec_d[low] - spec_o[low]) ** 2))
            pairs.append((float(np.sum(np.abs(spec_o[high]) ** 2)),
                          float(np.sum(np.abs(spec_d[high]) ** 2))))
    # Score only the windows that carry real high-frequency content. A window
    # 40 dB below the loudest one is inaudible up there, and counting it would
    # drown the measurement in the near-silence between events.
    loudest = max((p[0] for p in pairs), default=0.0)
    errors = [abs(10 * np.log10(max(got, 1e-30) / want))
              for want, got in pairs if want > loudest * 1e-4]
    return (10 * np.log10(low_signal / max(low_noise, 1e-30)),
            float(np.mean(errors)) if errors else float("nan"))


def encode_and_decode(source, tag, kbps, couple=False):
    """Our encoder, then FFmpeg as the neutral decoder."""
    ac3 = BUILD / f"race_{tag}_{kbps}.ac3"
    wav = BUILD / f"race_{tag}_{kbps}.wav"
    cmd = [CLI, "encode", source, ac3, str(kbps)]
    if couple:
        cmd.append("couple")
    run(cmd)
    run(["ffmpeg", "-v", "error", "-y",
         "-err_detect", "crccheck+bitstream+buffer+explode",
         "-i", ac3, "-c:a", "pcm_f32le", wav])
    return read_wav_f32(wav)


def race_coupling(source, original):
    print(f"{'kbps':>5} | {'mode':>6} | {'all dB':>7} | "
          f"{'<8.0k dB':>9} | {'>8.0k err dB':>13}")
    print("-" * 56)
    for kbps in (96, 128, 192, 256):
        scores = {}
        for mode, couple in (("plain", False), ("couple", True)):
            decoded = encode_and_decode(source, f"cpl_{mode}", kbps, couple)
            o, d, _ = align(original, decoded)
            low_snr, high_err = band_measures(o, d)
            scores[mode] = (snr_db(o, d), low_snr, high_err)
            print(f"{kbps:>5} | {mode:>6} | {scores[mode][0]:>7.2f} | "
                  f"{low_snr:>9.2f} | {high_err:>13.2f}")
        low_gain = scores["couple"][1] - scores["plain"][1]
        print(f"{'':>5} | {'delta':>6} | {'':>7} | {low_gain:>+9.2f} | "
              f"{scores['couple'][2] - scores['plain'][2]:>+13.2f}")
    print("\nBaseband delta positive = coupling bought precision where it should.")
    print("Coupled-band error is |level error|, so lower is better either way.")

def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "ac3"
    BUILD.mkdir(exist_ok=True)
    left, right = make_material()
    source = BUILD / "race_src.wav"
    write_wav_f32(source, left, right)
    original = read_wav_f32(source)
    seconds = len(left) / RATE
    if which == "eac3":
        race_eac3(original, source, seconds)
    elif which == "eac3-51":
        # Coupling's saving scales with the channel count - five high bands
        # collapse into one, where stereo only collapses two - so 5.1 is where
        # it has the most to prove.
        source = BUILD / "race_src51.wav"
        write_wav_f32(source, make_material_51())
        race_eac3(read_wav_f32(source), source, seconds, rates=(192, 256, 384))
    elif which == "seam":
        seam_check(source)
    elif which == "crosscheck":
        crosscheck(original, source)
    elif which == "couple":
        race_coupling(source, original)
    elif which == "ac3":
        race_ac3(original, source, seconds)
    else:
        raise SystemExit(f"unknown race '{which}' (ac3 | couple | eac3 | eac3-51 | seam | crosscheck)")


if __name__ == "__main__":
    main()
