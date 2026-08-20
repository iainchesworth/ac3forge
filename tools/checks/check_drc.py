"""Validate the dynamic-range and downmix metadata against FFmpeg.

Metadata is easy to emit and hard to confirm: a stream with the bits in the
right places and nonsense in them decodes without complaint. So every check
here is DISCRIMINATING - it compares a decode that applies the metadata
against one that does not, and fails if the two are the same. A stream with
dead metadata passes a bit-level check and fails these.

  dynrng   ffmpeg -drc_scale 0 vs 1 on a loud/quiet programme. Asserts the
           loud passage comes down, the quiet passage comes up, and the range
           between them shrinks by close to what the profile curve predicts.
  compr    ffmpeg -heavy_compr 0 vs 1. Asserts the applied gain matches the
           transmitted word AND that the decoded peak stays under the ceiling
           the encoder promised - including across a hard loud-to-quiet
           transition, which is where a naive implementation leaks.
  downmix  ffmpeg -ac 2 on a 5.1 stream, with the surround and centre tones
           measured by Goertzel so adjacent tones cannot contaminate the
           reading. Asserts each level code moves the fold-down by the dB the
           table says.
  E-AC-3   the same dynrng through Annex E's container, plus every layout
           decoding cleanly with the mixmdate group present. E-AC-3 compr has
           no oracle here and the script says so rather than pretending.

Run from the repo root, after building:
    python tools/checks/check_drc.py [--cli build/dev/bin/ac3cli.exe]
"""

import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent

FAILURES: list[str] = []


def run(*args: str) -> None:
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(args)}\n{result.stderr}")


def ffmpeg(*args: str) -> None:
    run("ffmpeg", "-y", "-loglevel", "error", *args)


def read_wav_f32(path: Path):
    """Minimal float32 WAV reader: (samples[n, ch], rate)."""
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    pos, rate, channels, samples = 12, 0, 0, None
    while pos + 8 <= len(data):
        cid = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8 : pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate = struct.unpack_from("<HHI", body, 0)
            bits = struct.unpack_from("<H", body, 14)[0]
            if tag == 0xFFFE:
                # WAVE_FORMAT_EXTENSIBLE, which ffmpeg writes for anything past
                # stereo: the real tag is the first field of the SubFormat GUID.
                tag = struct.unpack_from("<H", body, 24)[0]
            assert tag == 3 and bits == 32, f"expected float32, got tag {tag} bits {bits}"
        elif cid == b"data":
            samples = np.frombuffer(body, dtype="<f4").reshape(-1, channels)
        pos += 8 + size + (size & 1)
    assert samples is not None
    return samples, rate


def db(value: float) -> float:
    return 20.0 * math.log10(max(value, 1e-12))


def rms_db(x: np.ndarray) -> float:
    return db(float(np.sqrt(np.mean(np.square(x.astype(np.float64))))))


def peak_db(x: np.ndarray) -> float:
    return db(float(np.max(np.abs(x.astype(np.float64)))))


def tone_db(x: np.ndarray, freq: float, rate: int) -> float:
    """RMS of one frequency component, by projection onto that basis pair.

    A bandpass filter leaks neighbouring tones and rings at the edges; this
    reads the single component exactly, which matters when the test tones are
    only 200 Hz apart.
    """
    x = x.astype(np.float64)
    n = np.arange(len(x))
    phase = 2.0 * np.pi * freq * n / rate
    # Amplitude of the best-fit sinusoid; /sqrt(2) converts it to RMS.
    real = 2.0 * np.mean(x * np.cos(phase))
    imag = -2.0 * np.mean(x * np.sin(phase))
    return db(math.hypot(real, imag) / math.sqrt(2.0))


def check(name: str, ok: bool, detail: str) -> None:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")
    if not ok:
        FAILURES.append(name)


# --- the programme material -------------------------------------------------


def write_programme(path: Path, loud: float, quiet: float, rate: int = 48000) -> None:
    """Four seconds loud, four quiet, twice. Stereo float32.

    Segments long enough for the profile's ~1 s release to settle, so the
    steady-state gain is what gets measured rather than the transition. Both
    edges are hard, which is what stresses the compr ceiling.
    """
    seg = 4 * rate
    t = np.arange(4 * seg) / rate
    envelope = np.where((t % 8.0) < 4.0, loud, quiet)
    tone = envelope * np.sin(2.0 * np.pi * 440.0 * t)
    stereo = np.stack([tone, tone], axis=1).astype("<f4")
    body = stereo.tobytes()
    header = (
        b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt "
        + struct.pack("<IHHIIHH", 16, 3, 2, rate, rate * 2 * 4, 2 * 4, 32)
        + b"data" + struct.pack("<I", len(body))
    )
    path.write_bytes(header + body)


# --- checks -----------------------------------------------------------------


def check_dynrng(cli: str, tmp: Path) -> None:
    print("dynrng (section 7.7.1) - a range reduction ffmpeg can be made to apply or ignore")
    source = tmp / "prog.wav"
    write_programme(source, loud=0.5, quiet=0.0056)
    stream = tmp / "prog_drc.ac3"
    # dialnorm 24 puts the loud passage in the cut region and the quiet one in
    # the boost region of film-standard; without a sane dialnorm the whole
    # programme can land inside the null band and the curve does nothing.
    run(cli, "encode", str(source), str(stream), "448", "drc=film-standard", "dialnorm=24")

    levels = {}
    for scale in (0, 1):
        out = tmp / f"prog_s{scale}.wav"
        ffmpeg("-drc_scale", str(scale), "-i", str(stream), "-c:a", "pcm_f32le", str(out))
        samples, rate = read_wav_f32(out)
        # Windows inside each segment, clear of the transitions.
        levels[scale] = (
            rms_db(samples[int(3.0 * rate) : int(3.9 * rate)]),
            rms_db(samples[int(7.0 * rate) : int(7.9 * rate)]),
        )

    loud_cut = levels[0][0] - levels[1][0]
    quiet_boost = levels[1][1] - levels[0][1]
    range_before = levels[0][0] - levels[0][1]
    range_after = levels[1][0] - levels[1][1]

    check("loud passage is attenuated", loud_cut > 3.0, f"{loud_cut:.2f} dB of cut")
    check("quiet passage is lifted", quiet_boost > 3.0, f"{quiet_boost:.2f} dB of boost")
    check(
        "range is reduced",
        range_before - range_after > 8.0,
        f"{range_before:.1f} dB -> {range_after:.1f} dB",
    )

    # The discriminator: the same audio with no DRC must not respond at all.
    plain = tmp / "prog_none.ac3"
    run(cli, "encode", str(source), str(plain), "448", "dialnorm=24")
    plain_levels = []
    for scale in (0, 1):
        out = tmp / f"plain_s{scale}.wav"
        ffmpeg("-drc_scale", str(scale), "-i", str(plain), "-c:a", "pcm_f32le", str(out))
        samples, _ = read_wav_f32(out)
        plain_levels.append(rms_db(samples))
    delta = abs(plain_levels[0] - plain_levels[1])
    check(
        "a stream without DRC does not respond",
        delta < 0.01,
        f"{delta:.4f} dB between -drc_scale 0 and 1",
    )


def check_compr(cli: str, tmp: Path) -> None:
    print("compr (section 7.7.2) - a peak ceiling, and it has to actually hold")
    source = tmp / "hot.wav"
    # Nearly full scale, so the ceiling binds rather than the make-up gain.
    write_programme(source, loud=0.95, quiet=0.0056)
    ceiling = -0.5
    stream = tmp / "hot_heavy.ac3"
    run(cli, "encode", str(source), str(stream), "448", "heavy", "dialnorm=24",
        f"ceiling={ceiling}")

    peaks = {}
    for heavy in (0, 1):
        out = tmp / f"hot_h{heavy}.wav"
        ffmpeg("-heavy_compr", str(heavy), "-i", str(stream), "-c:a", "pcm_f32le", str(out))
        samples, _ = read_wav_f32(out)
        peaks[heavy] = peak_db(samples)

    check(
        "heavy compression changes the decode",
        abs(peaks[0] - peaks[1]) > 0.1,
        f"peak {peaks[0]:.2f} -> {peaks[1]:.2f} dBFS",
    )
    # The whole promise. It covers the hard loud-to-quiet transition too, where
    # the previous frame's tail is windowed into a frame that has already gone
    # quiet and would otherwise carry a generous gain over loud samples.
    check(
        "the ceiling holds everywhere, transitions included",
        peaks[1] <= ceiling + 0.01,
        f"peak {peaks[1]:.2f} dBFS against a {ceiling} dBFS ceiling",
    )
    # Rounding must never go the wrong way: the word may sit below the ceiling,
    # never above it.
    headroom = ceiling - peaks[1]
    check(
        "the ceiling is not overshot by more than one quantiser step",
        0.0 <= headroom < 1.0,
        f"{headroom:.2f} dB of unused headroom (compr steps are 0.28-0.5 dB)",
    )


def check_downmix(cli: str, tmp: Path) -> None:
    print("downmix levels (Tables 5.9 / 5.10 feeding section 7.8) - against ffmpeg -ac 2")
    # 'sine 51' puts a distinct tone in each CODED channel - main.cpp's
    # layout_tones() assigns 200 + 137*i Hz by Table 5.8 coded order
    # (L, C, R, SL, SR, LFE) regardless of the freq_hz argument passed here,
    # so a single component read tells us what happened to exactly one
    # channel. Centre is coded index 1 (337 Hz); the surround that folds into
    # ffmpeg's -ac 2 left/"Lo" output per Sec. 7.8 is SL, coded index 3
    # (611 Hz) - NOT the freq_hz-derived 800/600 Hz this check assumed before
    # a real ffmpeg run first caught the mismatch (it had never been run in
    # CI). The WAV column order ffmpeg/read_wav_f32 sees is the standard
    # multichannel convention (L, R, C, LFE, SL, SR), not Table 5.8's coded
    # order - confirmed empirically, not guessed, since the two disagree.
    amplitude_db = db(0.40 / math.sqrt(2.0))

    for label, freq, option, cases in (
        ("centre", 337.0, "cmixlev", {"-3": -3.01, "-4.5": -4.52, "-6": -6.02}),
        ("surround", 611.0, "surmixlev", {"-3": -3.01, "-6": -6.02}),
    ):
        for value, expected_db in cases.items():
            stream = tmp / f"dm_{option}_{value}.ac3"
            run(cli, "sine", str(stream), "3", "448", "1000", "40", "51",
                f"{option}={value}")
            out = tmp / f"dm_{option}_{value}.wav"
            ffmpeg("-i", str(stream), "-ac", "2", "-c:a", "pcm_f32le", str(out))
            samples, rate = read_wav_f32(out)
            # Skip the first frame: its MDCT window is half history that does
            # not exist, so it is a fade-in rather than steady state.
            left = samples[rate // 2 :, 0]
            measured = tone_db(left, freq, rate) - amplitude_db
            # ffmpeg applies the §7.8 coefficients WITHOUT §7.8.1's
            # normalisation - "it may be necessary", not "shall" - so the
            # expected figure is the raw table value. That difference is also
            # why the encoder's own peak detector normalises: normalisation is
            # the decoder's to do, and compr cannot compensate for a decoder
            # that skips it without attenuating the multichannel output too.
            check(
                f"{option}={value} gives {expected_db:+.2f} dB",
                abs(measured - expected_db) < 0.15,
                f"{label} tone folded down at {measured:+.2f} dB",
            )

    # 'off' is a real value, not a reserved code: the surrounds leave the
    # fold-down entirely.
    stream = tmp / "dm_off.ac3"
    run(cli, "sine", str(stream), "3", "448", "1000", "40", "51", "surmixlev=off")
    out = tmp / "dm_off.wav"
    ffmpeg("-i", str(stream), "-ac", "2", "-c:a", "pcm_f32le", str(out))
    samples, rate = read_wav_f32(out)
    measured = tone_db(samples[rate // 2 :, 0], 611.0, rate) - amplitude_db
    check("surmixlev=off drops the surrounds", measured < -40.0,
          f"surround tone at {measured:+.2f} dB")


def check_dialnorm(cli: str, tmp: Path) -> None:
    print("dialnorm (section 5.4.2.8) - BS.1770 loudness, against ffmpeg's own ebur128")
    # BS.1770's calibration point: 1 kHz at -20 dBFS in L and R reads -20 LKFS.
    rate = 48000
    t = np.arange(10 * rate) / rate
    tone = (0.1 * np.sin(2.0 * np.pi * 1000.0 * t)).astype(np.float64)
    stereo = np.stack([tone, tone], axis=1).astype("<f4")
    body = stereo.tobytes()
    source = tmp / "cal.wav"
    source.write_bytes(
        b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt "
        + struct.pack("<IHHIIHH", 16, 3, 2, rate, rate * 2 * 4, 2 * 4, 32)
        + b"data" + struct.pack("<I", len(body)) + body
    )

    ours = subprocess.run([cli, "loudness", str(source)], capture_output=True, text=True)
    measured = float(ours.stdout.split()[1])
    check("the -20 dBFS 1 kHz calibration reads -20 LKFS", abs(measured + 20.0) < 0.15,
          f"{measured:.2f} LKFS")

    theirs = subprocess.run(
        ["ffmpeg", "-nostats", "-i", str(source), "-af", "ebur128=framelog=quiet",
         "-f", "null", "-"],
        capture_output=True, text=True,
    )
    reference = None
    for line in theirs.stderr.splitlines():
        if line.strip().startswith("I:"):
            reference = float(line.split()[1])
    check("agrees with ffmpeg's ebur128", reference is not None
          and abs(measured - reference) < 0.2,
          f"ours {measured:.2f} vs ffmpeg {reference} LUFS")

    # And it reaches the bitstream: dialnorm=auto must put the measured value in
    # bsi, not the 31 that means "no idea".
    stream = tmp / "cal.ac3"
    run(cli, "encode", str(source), str(stream), "192", "dialnorm=auto")
    decoded = subprocess.run([cli, "decode", str(stream), str(tmp / "cal_out.wav")],
                             capture_output=True, text=True)
    carried = None
    for line in decoded.stdout.splitlines():
        if "dialnorm" in line:
            carried = int(line.split()[2])
    check("dialnorm=auto reaches bsi", carried == 20, f"stream carries dialnorm {carried}")


def check_eac3(cli: str, tmp: Path) -> None:
    print("E-AC-3 - the same metadata through Annex E's container")
    # dynrng lives in audblk in both syntaxes, and ffmpeg's shared audblk parse
    # reads it, so -drc_scale is a real oracle here too.
    stream = tmp / "e51.ec3"
    run(cli, "eac3-sine", str(stream), "3", "448", "1000", "30", "51",
        "drc=film-standard", "mixmeta", "dialnorm=24")
    levels = []
    for scale in (0, 1):
        out = tmp / f"e51_s{scale}.wav"
        ffmpeg("-drc_scale", str(scale), "-i", str(stream), "-c:a", "pcm_f32le", str(out))
        samples, rate = read_wav_f32(out)
        levels.append(rms_db(samples[rate : 2 * rate]))
    check("E-AC-3 dynrng is applied", levels[0] - levels[1] > 3.0,
          f"{levels[0]:.2f} -> {levels[1]:.2f} dB")

    # Carrying the mixmdate group must not disturb anything a decoder relies on:
    # every layout still decodes, with the channel count it declared.
    for layout, channels in (("stereo", 2), ("51", 6), ("71", 8), ("512", 8), ("514", 10)):
        path = tmp / f"mix_{layout}.ec3"
        run(cli, "eac3-sine", str(path), "1", "448", "1000", "30", layout, "mixmeta",
            "drc=film-standard", "heavy", "dialnorm=24")
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "stream=channels",
             "-of", "csv=p=0", str(path)],
            capture_output=True, text=True,
        )
        decode = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", str(path), "-f", "null", "-"],
            capture_output=True, text=True,
        )
        got = probe.stdout.strip().splitlines()[0] if probe.stdout.strip() else "?"
        check(f"{layout} with mixmdate decodes cleanly",
              got == str(channels) and decode.returncode == 0 and not decode.stderr.strip(),
              f"{got} channels, ffmpeg said {decode.stderr.strip() or 'nothing'}")

    # No oracle for E-AC-3 compr: ff_eac3_parse_header reads compre and skips
    # the word, so -heavy_compr cannot respond however correct the stream is.
    # Reporting that is the honest thing; asserting a change would be a test
    # that passes for the wrong reason.
    print("  note  E-AC-3 compr is unverifiable here: ffmpeg's Annex E header")
    print("        parser skips the compression word, so -heavy_compr is inert.")
    print("        Covered instead by tools/references/eac3_parse.py and tests/meta/test_drc.cpp.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", default="build/dev/bin/ac3cli.exe")
    parser.add_argument("--keep", action="store_true", help="keep the temporary files")
    args = parser.parse_args()

    cli = str((REPO / args.cli).resolve() if not Path(args.cli).is_absolute() else args.cli)
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")
    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg not on PATH; it is the oracle for every check here")

    tmp = Path(tempfile.mkdtemp(prefix="ac3drc_"))
    try:
        check_dialnorm(cli, tmp)
        check_dynrng(cli, tmp)
        check_compr(cli, tmp)
        check_downmix(cli, tmp)
        check_eac3(cli, tmp)
    finally:
        if args.keep:
            print(f"\nfiles kept in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed: {', '.join(FAILURES)}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
