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
| `ac3/io/dec3.hpp` | `build_codec_config_box` — the ISOBMFF `dac3`/`dec3` sample-entry payload (ETSI TS 102 366 Annex F), Dolby Atmos extension included, built from a `ScannedStream`. What `mp4::AudioTrack::codec_config` is filled in with. |
| `ac3/io/wav.hpp` | WAV read/write (PCM16 and float32) and the WAV↔Table 5.8 permutation. |
| `ac3/meta/drc.hpp`, `loudness.hpp`, `mixing.hpp`, `qc.hpp` | `dynrng`, `compr`, BS.1770, downmix levels, named QC delivery-gate presets. |
| `ac3/spatial/spatial.hpp` | `BedRenderer`, `pan_azimuth`, `pan_room`. |
| `ac3/oba/atmos.hpp`, `joc.hpp`, `oamd.hpp` | The object layer. |
| `ac3/oba/motion.hpp` | `Keyframe`, `KeyframePath`, `OrbitPath`, `ObjectPath` (a `std::variant` of the two), `evaluate_placements`. Turns authored keyframes or a closed-form orbit into the `ObjectPlacement` span `AtmosEncoder::encode_frame` already took per-frame — a placement-generation layer in front of the existing API, not a change to it. Backs `ac3cli atmos-path` and `live`'s `atmos` mode. |
| `ac3/emdf/emdf.hpp` | The TS 102 366 Annex H container. |
| `ac3/sinks/iec61937.hpp`, `passthrough.hpp` | S/PDIF burst packing (AC-3 and E-AC-3); exclusive-mode/direct bitstream output — WASAPI on Windows, ALSA on Linux, CoreAudio on macOS. |
| `ac3/audio/monitor.hpp` | `MonitorSink` — shared-mode PCM playback (WASAPI/ALSA/CoreAudio, resampled and mixed like any other app), for previewing a decode without a bitstream-capable receiver. The non-exclusive counterpart to `PassthroughSink`. Backs `ac3cli monitor` and `live`'s monitor leg. |
| `ac3/audio/capture.hpp`, `ring_buffer.hpp` | Live input/loopback capture — WASAPI on Windows, ALSA on Linux, CoreAudio on macOS (input-only: no loopback) — and the lock-free SPSC ring behind it. |
| `ac3/audio/resampler.hpp`, `watchdog.hpp` | `DriftResampler` and `ClockDriftEstimator` — the audio-thread-safe linear resampler plus the servo that corrects tens-of-ppm clock drift between two live devices; `SilenceWatchdog`, which tells a read loop when a capture device has stopped delivering rather than merely starved. |
| `ac3/dsp/biquad.hpp`, `resampler.hpp` | `Biquad` and `LfeLowpass` — the 4th-order Butterworth band-limit applied when a full-bandwidth source channel is hand-assigned onto an LFE position; `ac3::dsp::resample`/`resample_planar` — the offline windowed-sinc polyphase SRC that converts a loaded second source's native rate to the session's (distinct from `DriftResampler`, which trades kernel quality for hot-path safety). |
| `ac3/audio/audio_backend.hpp` | `ac3::audio::audio_backend()` — whether capture, monitor playback and passthrough are available at all on this build's platform, and why not when they aren't. Backs the CLI's `UNAVAILABLE HERE` messaging for `devices`, `record`, `monitor`, `live`, `outputs` and `play`. |
| `ac3/analysis/levels.hpp` | Peak/RMS metering and the Gerzon energy vector. |
| `matroska/matroska.hpp` | `mux`, `AudioTrack`, `MuxOptions`, `Writer` — the batch muxer plus the incremental create/`header`/`push`/`finalize` writer a live (unknown-length) session records through. |
| `mp4/mp4.hpp` | `mux`, `AudioTrack`, `MuxOptions` — same shape as `matroska/matroska.hpp`, plus `AudioTrack::codec_config` for the `dac3`/`dec3` box payload (see `ac3/io/dec3.hpp` above) — and `fragment`, `FragmentOptions`, `MediaSegment`, `FragmentedOutput` for fragmented MP4/CMAF output. |
| `mp4/hls.hpp` | `HlsOptions`, `hls_codec_string`, `build_hls_media_playlist`, `build_hls_master_playlist` — HLS signalling for `mp4::fragment`'s segments. |
| `mp4/dash.hpp` | `DashOptions`, `build_dash_adaptation_set` — the DASH `<AdaptationSet>` snippet for the same segments. |
| `mpegts/mpegts.hpp` | `mux`, `AudioTrack`, `AudioCodec`, `MuxOptions` — the single-programme MPEG-2 TS muxer, DVB descriptors. |
| `ac3adm/ac3adm.hpp`, `model.hpp` | `parse_bw64`, `AdmDocument`, `AdmError` — the standalone BW64/RF64 + Audio Definition Model reader, `ac3adm::ac3adm`. Opt-in: only built with `-DAC3FORGE_BUILD_ADM=ON` (see [ADM / BW64 reading](adm.md)). |
| `ac3/signing/signing_key.hpp`, `emdf_atmos_signer.hpp` | `SigningKey`, `load_signing_key`, `decode_signing_key`, `sign_atmos_stream`, `sign_atmos_frame` — the EMDF object-signing library, `ac3::signing`. A separate CMake target from `ac3::forge`, not part of the distributed package's link line: signing is a strictly optional step a front end applies to already-encoded frames. |
