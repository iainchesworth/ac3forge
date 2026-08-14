# Muxing & sinks

## Muxing: `matroska::mux`

`matroska/matroska.hpp`, library `matroska::matroska`. It links nothing from `ac3::forge` and
takes frames as opaque bytes. Pairing it with `ac3::io::scan` is what keeps the track header
honest.

```cpp
// One Matroska frame per access unit. For E-AC-3 an access unit is the
// independent substream plus its dependents, which is exactly what scan
// groups — a player must receive them together.
std::vector<std::vector<std::byte>> frames;
for (const auto unit : scanned->access_units) {
    frames.emplace_back(unit.begin(), unit.end());
}

const matroska::AudioTrack track{
    .codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3
                                ? matroska::kCodecAc3
                                : matroska::kCodecEac3},
    .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
    .channels = scanned->channels,
    .samples_per_frame = ac3::kSamplesPerFrame,
};

const auto file = matroska::mux(track, frames);
```

Full program: [`examples/mux_mkv.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_mkv.cpp).

`mux` returns the whole file as bytes and does no file I/O, which keeps it testable without a
disk. It writes one audio track, one SimpleBlock per frame, clusters closed on a time budget,
and Info with TimestampScale and Duration. No SeekHead, no Cues, no chapters, no tags — those
matter for seeking in large files, not for playing back what this project produces.

## Bitstream sinks (`ac3::sinks`)

The pieces below are audio-hardware-facing rather than example-driven, so there's no compiled
`examples/` program to excerpt — this is reference prose pointing at the relevant header, plus
the platform and hardware-verification caveats [Validation](../verification.md) states about
each. All of them are gated by `ac3::platform::audio_backend()`
(`ac3/platform/audio_backend.hpp`), which reports whether capture, monitor playback and
passthrough are available on this build's platform, and why not when they aren't — this backs
the CLI's `UNAVAILABLE HERE` messaging for `devices`, `record`, `monitor`, `live`, `outputs`
and `play`.

### `ac3::sinks::iec61937` — S/PDIF burst packing

`ac3/sinks/iec61937.hpp`. Packs AC-3 or E-AC-3 elementary-stream frames into IEC 61937 burst
framing — the wrapper a compressed bitstream needs over PCM-shaped hardware/interfaces (S/PDIF,
HDMI) so a receiver recognizes it as AC-3/E-AC-3 rather than treating it as noisy PCM. AC-3
burst packing is byte-exact against FFmpeg's `spdif` muxer. E-AC-3 packing (`Eac3BurstPacker`)
— data type 0x15, the 24576-byte/4x-carrier-rate burst, multi-syncframe accumulation, `Pd` in
bytes not bits — is independently verified against both FFmpeg's `spdif_header_eac3` and
Microsoft's own IEC 61937 documentation (both fetched live and cross-checked against each
other, not recalled), plus round-trip and real-audio unit tests. This header only produces the
framed bytes; getting them onto real hardware is `PassthroughSink`, below.

### `ac3::sinks::PassthroughSink` — exclusive-mode passthrough

`ac3/sinks/passthrough.hpp`. Exclusive-mode/direct bitstream output, AC-3 or E-AC-3 — WASAPI on
Windows, ALSA on Linux — the path an AV receiver needs to see the raw compressed bitstream
rather than decoded PCM.

Stated plainly, because this project's docs don't soften verification gaps: **exclusive-mode
passthrough — AC-3 and E-AC-3 alike — has never been confirmed against real bitstreaming
hardware.** The development machine has no S/PDIF or HDMI endpoint behind a real Dolby-capable
AV receiver, and `IsFormatSupported` correctly rejects both Dolby IEC 61937 subtypes everywhere
it has been tried. What *is* verified: the exclusive-mode path itself works (a Realtek endpoint
accepts an exclusive-mode PCM format), and the burst framing it carries is verified as
described above under `ac3::sinks::iec61937`. But no bitstream-capable receiver has been
confirmed to lock onto output from this sink specifically — the one receiver-locking check that
has been done used a different code path (bursts played as a PCM16 WAV through a passthrough
output), not `PassthroughSink` itself, and that check has only been tried for AC-3, not E-AC-3.

### `ac3::sinks::MonitorSink` — shared-mode monitor playback

`ac3/sinks/monitor.hpp`. The non-exclusive counterpart to `PassthroughSink`: shared-mode PCM
playback — WASAPI or ALSA, resampled and mixed like any other app — that decodes what is being
encoded and plays it back on an ordinary output, for previewing a decode without a
bitstream-capable receiver. Backs `ac3cli monitor` and `live`'s monitor leg.

Unlike passthrough, **this one is confirmed against real hardware.** It has actually played
decoded AC-3 and E-AC-3 (including an Atmos stream's 5.1 bed) through real Windows (Realtek)
hardware in real time, and a live microphone capture → encode → monitor session has run
end-to-end. Building this path against real hardware surfaced two genuine bugs that neither
unit tests nor silent/synthetic input would have caught — see the README's verification-gaps
section for the details, and `src/audio/src/platform/windows/monitor.cpp` for the fixes.

## Capture: `ac3::capture`

`ac3/capture/capture.hpp`, `ring_buffer.hpp`. Live input/loopback capture — WASAPI on Windows,
ALSA on Linux — through the lock-free SPSC ring in `ring_buffer.hpp`, which sits between the
audio callback and whatever consumes the samples (an encoder, a monitor sink, or both). This is
what backs `ac3cli record`/`live` and the GUI's live-session tab.

## Metering: `ac3::analysis`

`ac3/analysis/levels.hpp`. Peak/RMS metering with console ballistics, plus the Gerzon energy
vector computed over the BS.775 ring — the metering `ac3cli` and the GUI share so their two
displays never disagree about what a signal contains. One `LevelMeter` instance drives both: the
moving display (`levels()`, ballistic) and the exact end-of-run report (`summary()`,
unweighted), fed by the same pass over the samples.

```cpp
ac3::analysis::LevelMeter meter{acmod, lfe, 48000};
meter.process(decoded_views);   // once per frame, planar A/52 order
```

```cpp
const auto& stats = meter.summary()[static_cast<std::size_t>(ch)];  // exact, not ballistic
std::printf("peak %.1f dBFS  rms %.1f dBFS\n", stats.peak_db(), stats.rms_db());

const auto energy = ac3::analysis::energy_vector(meter.levels(), acmod);
```

Full program: [`examples/level_metering.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/level_metering.cpp)
— decodes a 5.1 stream and reports both the per-channel peak/RMS and the soundfield's energy
vector.

This is a separate concern from the BS.1770 integrated-loudness measurement in
`ac3::meta::LoudnessMeter` (see [Metadata](metadata.md)): one is instantaneous display
metering, the other the gated whole-programme measurement `dialnorm` is derived from.
`energy_vector` is computed from the integrated RMS of the full-bandwidth channels only — the
LFE has no direction to contribute, and a subwoofer's level would otherwise swamp the sum.

---

See also: [Decoding](decoding.md) — `ac3::io::scan` is what feeds both `matroska::mux` and the
sinks above their access units; [Header map](header-map.md) — every header referenced on this
page in one table.
