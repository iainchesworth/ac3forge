"""Spectral check of a decoded sine: frequency, amplitude, SNR vs ideal.

Usage: python tools/checks/check_sine.py decoded.wav [freq_hz] [amplitude]
Exit 0 iff the dominant frequency matches, amplitude is within 0.5 dB, and
the delay-compensated SNR clears 40 dB (an easy bar for a sine at 192 kbps).
"""

import sys
import wave

import numpy as np


def main():
    path = sys.argv[1]
    freq = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0
    amplitude = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5

    with wave.open(path, "rb") as f:
        rate = f.getframerate()
        channels = f.getnchannels()
        width = f.getsampwidth()
        raw = f.readframes(f.getnframes())
    if width == 2:
        pcm = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    else:
        raise SystemExit(f"unexpected sample width {width}")
    pcm = pcm.reshape(-1, channels)

    ok = True
    for ch in range(channels):
        x = pcm[:, ch]
        # Skip the encoder's leading transient (delay + first frame) and tail.
        x = x[4096:-4096]
        spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
        peak_bin = int(np.argmax(spectrum))
        peak_freq = peak_bin * rate / len(x)
        peak_amp = float(np.max(np.abs(x)))
        amp_db = 20 * np.log10(peak_amp / amplitude) if peak_amp > 0 else -999.0

        # Delay-compensated SNR: fit the ideal sine's phase via the analytic
        # signal at the target frequency.
        n = np.arange(len(x))
        ref_c = np.exp(-2j * np.pi * freq / rate * n)
        coeff = 2.0 * np.mean(x * ref_c)
        fit = np.real(coeff * np.exp(2j * np.pi * freq / rate * n))
        noise = x - fit
        snr_db = 10 * np.log10(np.sum(fit**2) / max(np.sum(noise**2), 1e-30))

        freq_ok = abs(peak_freq - freq) < 5.0
        amp_ok = abs(amp_db) < 0.5
        snr_ok = snr_db > 40.0
        ok &= freq_ok and amp_ok and snr_ok
        print(f"ch{ch}: peak {peak_freq:8.2f} Hz ({'OK' if freq_ok else 'FAIL'})  "
              f"amplitude {peak_amp:.4f} = {amp_db:+.3f} dB vs target "
              f"({'OK' if amp_ok else 'FAIL'})  SNR {snr_db:6.2f} dB "
              f"({'OK' if snr_ok else 'FAIL'})")

    print("VERDICT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
