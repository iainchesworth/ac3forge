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
straight off the frame sizes handed in. No edit lists, no multiple tracks — those matter for
large-file seeking and multi-track muxing, not for playing back what this project produces.

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

## Fragmented MP4/CMAF + HLS/DASH: `mp4::fragment`, `mp4/hls.hpp`, `mp4/dash.hpp`

ROADMAP.md's A2, the streaming-delivery follow-up `mp4::mux`'s own header deliberately left for
later: `mp4::fragment` lays out the same track and frames as `mux`, but as a fragmented movie
(ISO/IEC 14496-12 §8.8's `moof`/`mfhd`/`traf`/`tfhd`/`tfdt`/`trun`) split into CMAF-shaped pieces
(ISO/IEC 23000-19) — an initialization segment (`ftyp`+`moov`, whose one `trak` carries
`mvex`/`trex` instead of a populated sample table, since a fragmented track's own `stbl`
describes zero samples) plus one or more media segments (`styp`+`moof`+`mdat`, one per fragment).
Same batch shape as `mux`: every frame is known up front, so real durations/timestamps are
filled in throughout rather than the zero/unknown placeholders a true live fragmenter would need.

```cpp
const auto fragmented =
    mp4::fragment(track, frames, mp4::FragmentOptions{.frames_per_fragment = 8});
```

`FragmentedOutput::init_segment` and `::media_segments` are exactly the files a packager or CDN
origin wants (`init.mp4` plus `segment1.m4s`, `segment2.m4s`, ...) — see `ac3cli fmp4`, which
writes them out that way alongside the manifests below.

`mp4/hls.hpp` and `mp4/dash.hpp` build HLS/DASH signaling for those same segments — one CMAF
segment format, two manifest flavors, the entire point of CMAF:

```cpp
const auto media_playlist =
    mp4::build_hls_media_playlist(track, fragmented->media_segments, mp4::HlsOptions{});
const auto master_playlist = mp4::build_hls_master_playlist(
    track, fragmented->media_segments, "audio.m3u8", mp4::HlsOptions{});
const auto dash_snippet = mp4::build_dash_adaptation_set(track, fragmented->media_segments);
```

Full program: [`examples/mux_fmp4.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_fmp4.cpp).

Both manifest flavors get the `CODECS`/`codecs` attribute right: `mp4::hls_codec_string` (and
`build_dash_adaptation_set` internally) use the bare `ac-3`/`ec-3` sample-entry fourcc unmodified
as the RFC 6381 `'Codecs'` parameter — neither AC-3 nor E-AC-3 registers any of the
dot-separated profile/level fields RFC 6381 §3 makes room for (unlike e.g. `avc1.640028`), which
is confirmed against every real HLS manifest example
[Apple's HLS Authoring Specification for Apple
Devices](https://developer.apple.com/documentation/http-live-streaming/hls-authoring-specification-for-apple-devices)
shows. Dolby Digital Plus with Atmos objects additionally needs `CHANNELS="<N>/JOC"` on the HLS
media rendition instead of a plain channel count, where N is the decodable object count
(`ac3::io::ScannedStream::oba_complexity_index`, TS 103 420 §8.3.2's `complexity_index_type_a`)
— reiterated, with a worked example (`CHANNELS="12/JOC"`), by [Dolby's own Online Delivery Kit
documentation](https://ott.dolby.com/OnDelKits/DDP/Dolby_Digital_Plus_Online_Delivery_Kit_v1.5/Documentation/Content_Creation/SDM/help_files/topics/hls_c_hls_signal_atmos_ddp.html)
and shown verbatim in a real manifest (`CODECS="avc1.64001f,ec-3"` / `CHANNELS="12/JOC"`) by
[AWS MediaLive's own HLS+Atmos
documentation](https://docs.aws.amazon.com/medialive/latest/ug/feature-dolbyatmos.html). `mp4::`
itself never reads that TS 103 420 object-layer syntax — `HlsOptions::channels_attribute` is
opaque to it, the same way `AudioTrack::codec_config` is; `ac3cli fmp4` is the caller that
already has `oba_complexity_index` (it read it to build the `dec3` box) and supplies the string.

The DASH snippet describes exact per-segment durations with a `SegmentTemplate`/`SegmentTimeline`
(ISO/IEC 23009-1 §5.3.9.6) built from each segment's own duration, rather than one nominal
`duration` attribute assumed constant — segments are constant-duration except (as usual) a
possibly shorter final one, and a flat nominal duration is exactly what let a real player
(FFmpeg's own `dash` demuxer, while writing this module) compute one too many segments from
`mediaPresentationDuration` and request a segment number past the end.

`mp4::fragment`'s ISOBMFF output and the HLS media playlist round-trip cleanly through FFmpeg's
own strict decode (`ffmpeg -v error -xerror -err_detect crccheck+bitstream+buffer+explode`) —
both the fragmented file (init segment concatenated with every media segment) and `audio.m3u8`
read back the exact original frame count and duration.

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
