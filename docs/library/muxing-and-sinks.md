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

## Muxing: `mp4::mux`

`mp4/mp4.hpp`, library `mp4::mp4`. Same shape as `matroska::matroska`: it links nothing from
`ac3::forge` and takes frames as opaque bytes. The one place MP4 needs codec-specific bytes that
Matroska's plain CodecID string does not is the sample entry's `dac3`/`dec3` configuration box
(ETSI TS 102 366 Annex F) — so `mp4::AudioTrack::codec_config` carries that box's payload as
opaque bytes too, built by `ac3::io::build_codec_config_box` (`ac3/io/dec3.hpp`) straight off
whatever `ac3::io::scan` read out of the bitstream, fscod/bsid/bsmod/acmod/lfeon and, when the
stream carries Dolby Atmos objects, the `flag_ec3_extension_type_a`/`complexity_index_type_a`
extension (TS 103 420 §8.3.1/§8.3.2.2) alike.

```cpp
// One MP4 sample per access unit. For E-AC-3 an access unit is the
// independent substream plus its dependents, which is exactly what scan
// groups — a player must receive them together.
std::vector<std::vector<std::byte>> frames;
frames.reserve(scanned->access_units.size());
for (const auto unit : scanned->access_units) {
    frames.emplace_back(unit.begin(), unit.end());
}

const mp4::AudioTrack track{
    .codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3 ? mp4::kCodecAc3
                                                                        : mp4::kCodecEac3},
    .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
    .channels = scanned->channels,
    .samples_per_frame = ac3::kSamplesPerFrame,
    // The dac3/dec3 sample-entry box, built from the same scan result -
    // see ac3/io/dec3.hpp for why this lives in ac3::io rather than in
    // mp4::mp4 itself.
    .codec_config = ac3::io::build_codec_config_box(*scanned),
};

const auto file = mp4::mux(track, frames);
```

Full program: [`examples/mux_mp4.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_mp4.cpp).

`mux` returns the whole file as bytes and does no file I/O, the same as `matroska::mux`. It
writes `ftyp`/`moov`/`mdat` for one audio track, one sample per chunk, `stts`/`stsz`/`stco` built
straight off the frame sizes handed in. No fragmentation, no edit lists, no multiple tracks —
ROADMAP.md's A2 (fMP4/CMAF) is the deliberate follow-up for streaming delivery, not something
this first cut tries to also be.

Getting the `dec3`/`dac3` box right from the spec is the point: FFmpeg's own MKV→MP4 remux path
is documented to silently drop or mis-signal the Atmos extension
([jellyfin-ffmpeg#584](https://github.com/jellyfin/jellyfin-ffmpeg/issues/584)) — building it
from `ac3::io::scan`'s own read of the bitstream, rather than by copying another tool's output,
is what this module avoids that bug by construction rather than by patching it after the fact.

## Muxing: `mpegts::mux`

`mpegts/mpegts.hpp`, library `mpegts::mpegts`. Same shape as `matroska::mux` above — it links
nothing from `ac3::forge` beyond the AC-3/E-AC-3 choice it is told, and takes access units as
opaque bytes.

```cpp
// One PES-wrapped access unit per TS access unit. For E-AC-3 an access
// unit is the independent substream plus its dependents, which is
// exactly what scan groups — a player must receive them together.
std::vector<std::vector<std::byte>> frames;
frames.reserve(scanned->access_units.size());
for (const auto unit : scanned->access_units) {
    frames.emplace_back(unit.begin(), unit.end());
}

const mpegts::AudioTrack track{
    .codec = scanned->kind == ac3::io::StreamKind::kAc3 ? mpegts::AudioCodec::kAc3
                                                         : mpegts::AudioCodec::kEac3,
    .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
    .channels = scanned->channels,
    .samples_per_frame = ac3::kSamplesPerFrame,
};

const auto file = mpegts::mux(track, frames);
```

Full program: [`examples/mux_ts.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_ts.cpp).

`mux` returns the whole 188-byte-aligned Transport Stream as bytes, no file I/O, same testability
reasoning as `matroska::mux`. It writes a single program — one PAT, one PMT (repeated
periodically so a receiver tuning in mid-stream doesn't wait for byte zero), and one PES-wrapped
elementary stream carrying PCR every access unit. No video, no other elementary streams, no PID
remapping: a general-purpose multiplexer is out of scope, this is enough for a player or
`ffprobe` to recognize one AC-3/E-AC-3 programme.

**Broadcast profile.** Two standards register AC-3/E-AC-3 for MPEG-TS carriage — ATSC and DVB —
with different, non-interoperable signalling. This module implements DVB only: `stream_type` 0x06
(audio carried as PES private data) plus the `AC3_descriptor` (tag `0x6A`) or
`Enhanced_AC3_descriptor` (tag `0x7A`) DVB defines in ETSI EN 300 468 Annex D.3/D.5, chosen per
this project's clean-room sourcing rules as the more completely specified of the two registries.
Every optional identification field in either descriptor (`component_type`/`bsid`/`mainid`/`asvc`
and, for the enhanced form, `substream1`-`3`) is left unset — `ac3::io::scan` doesn't expose the
bsmod/full-service/associated-service granularity those fields carry, and a guessed value would
be actively misleading where an absent optional field is not; a decoder still gets everything it
needs to play the stream from the AC-3/E-AC-3 bitstream's own `bsmod`/`acmod`.

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
