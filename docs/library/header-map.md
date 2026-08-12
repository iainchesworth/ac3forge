# Header map

| Header | Contents |
|---|---|
| `ac3/core/tables.hpp` | `SampleRate`, `Acmod`, `ExpStrategy`, frame constants, Table 5.18. Nearly everything includes it. |
| `ac3/core/eac3_tables.hpp` | Annex E: `StreamType`, the Table E2.5 `chanmap` namespace, `Layout`. Also the general channel model: `chanmap::ChannelPlan` (a bed acmod/lfe plus however many dependent chanmaps it takes) and `chanmap::allocate(locations)`, which partitions an arbitrary Table E2.5 location bitmask into a `ChannelPlan` — the same shape `ac3::plan::LayoutId`'s eight named layouts are themselves expressed in, via their own hand-picked constants rather than by running the allocator (dual mono's is a special case — see [Encoding AC-3](encoding-ac3.md)). |
| `ac3/core/bitreader.hpp`, `bitwriter.hpp` | MSB-first bit I/O. |
| `ac3/core/mdct.hpp`, `window.hpp` | The 512-point MDCT and the KBD window. |
| `ac3/core/bitalloc.hpp`, `exponents.hpp`, `mantissas.hpp` | The §7.2 allocation model and §7.1/§7.3 coding, shared by encoder and decoder. |
| `ac3/encoder/encoder.hpp` | `EncoderConfig`, `FrameEncoder`. |
| `ac3/encoder/eac3_frame.hpp` | `FrameConfig`, `FrameEncoder`, `AccessUnitConfig`, `AccessUnitEncoder`, `AccessUnit`. |
| `ac3/encoder/silent_frame.hpp` | `FrameError`, and pure-syntax silent frames. |
| `ac3/encoder/coupling.hpp`, `eac3_tools.hpp` | Coupling, spectral extension, AHT. |
| `ac3/encoder/transient.hpp` | `TransientDetector` — the §8.2.2 recipe that decides `blksw`, shared by both encoders; block switching itself has no config field, it is what this class's output drives. |
| `ac3/encoder/plan.hpp` | `ac3::plan` — `Plan`, `Codec`, `LayoutId`, `Tools`, `Metadata`, `Routing`. The layout table, Annex E tool tokens, metadata defaults and source-to-coded-channel routing, shared by `ac3cli` and `ac3gui` so the two front ends cannot disagree about what a layout or a tools token means. `Plan::custom_locations`, `resolve(plan)` and `parse_channels`/`format_channels` are the escape hatch onto `chanmap::allocate` for a channel set none of the eight named `LayoutId`s cover. |
| `ac3/encoder/assignment.hpp` | `ac3::plan::Assignment`, `Destination`, `SourceShape`, `parse_assignment`/`format_assignment`, `derive_codec`. The explicit, channel-by-channel alternative to `plan::route()` for multiple loaded sources — backs the CLI's `src=`/`map=` options and the GUI's multi-source controller. |
| `ac3/decoder/decoder.hpp` | `FrameDecoder`, `Eac3Decoder`, `split_frames`, `split_access_units`, `stream_bsid`. |
| `ac3/decoder/transient_prenoise.hpp` | `apply_transient_prenoise`, `transient_prenoise_range` — the §3.7.2 post-IMDCT pre-echo correction `Eac3Decoder` applies to decoded PCM; see [Decoding](decoding.md) for the buffering it forces on `decode_substream`/`decode_access_unit`. |
| `ac3/io/elementary.hpp` | `scan`, `ScannedStream`. |
| `ac3/io/wav.hpp` | WAV read/write (PCM16 and float32) and the WAV↔Table 5.8 permutation. |
| `ac3/meta/drc.hpp`, `loudness.hpp`, `mixing.hpp` | `dynrng`, `compr`, BS.1770, downmix levels. |
| `ac3/spatial/spatial.hpp` | `BedRenderer`, `pan_azimuth`, `pan_room`. |
| `ac3/oba/atmos.hpp`, `joc.hpp`, `oamd.hpp` | The object layer. |
| `ac3/oba/motion.hpp` | `Keyframe`, `KeyframePath`, `OrbitPath`, `ObjectPath` (a `std::variant` of the two), `evaluate_placements`. Turns authored keyframes or a closed-form orbit into the `ObjectPlacement` span `AtmosEncoder::encode_frame` already took per-frame — a placement-generation layer in front of the existing API, not a change to it. Backs `ac3cli atmos-path` and `live`'s `atmos` mode. |
| `ac3/emdf/emdf.hpp` | The TS 102 366 Annex H container. |
| `ac3/sinks/iec61937.hpp`, `passthrough.hpp` | S/PDIF burst packing (AC-3 and E-AC-3); exclusive-mode/direct bitstream output — WASAPI on Windows, ALSA on Linux. |
| `ac3/sinks/monitor.hpp` | `MonitorSink` — shared-mode PCM playback (WASAPI/ALSA, resampled and mixed like any other app), for previewing a decode without a bitstream-capable receiver. The non-exclusive counterpart to `PassthroughSink`. Backs `ac3cli monitor` and `live`'s monitor leg. |
| `ac3/capture/capture.hpp`, `ring_buffer.hpp` | Live input/loopback capture — WASAPI on Windows, ALSA on Linux — and the lock-free SPSC ring behind it. |
| `ac3/platform/audio_backend.hpp` | `ac3::platform::audio_backend()` — whether capture, monitor playback and passthrough are available at all on this build's platform, and why not when they aren't. Backs the CLI's `UNAVAILABLE HERE` messaging for `devices`, `record`, `monitor`, `live`, `outputs` and `play`. |
| `ac3/analysis/levels.hpp` | Peak/RMS metering and the Gerzon energy vector. |
| `matroska/matroska.hpp` | `mux`, `AudioTrack`, `MuxOptions`. |
