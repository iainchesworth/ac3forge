# Using ac3::forge

The public API is the headers under `src/lib/include/ac3/`. Link `ac3::forge`; link
`matroska::matroska` as well if you want the container writer.

```cmake
target_link_libraries(your_target PRIVATE ac3::forge)
```

Every code block below is an excerpt from a program in [`examples/`](../examples/). Those are
build targets and `ctest` entries, so a snippet that stopped compiling would break the build
rather than sit here being wrong.

## Contents

- [Conventions](#conventions)
- [AC-3: `ac3::FrameEncoder`](#ac-3-ac3frameencoder)
- [E-AC-3: `ac3::eac3::FrameEncoder`](#e-ac-3-ac3eac3frameencoder)
- [Wide layouts: `ac3::eac3::AccessUnitEncoder`](#wide-layouts-ac3eac3accessunitencoder)
- [Reading a stream: `ac3::io::scan`](#reading-a-stream-ac3ioscan)
- [Decoding](#decoding)
- [The spatial object layer](#the-spatial-object-layer)
- [Objects with metadata: `ac3::oba::AtmosEncoder`](#objects-with-metadata-ac3obaatmosencoder)
- [Muxing: `matroska::mux`](#muxing-matroskamux)
- [Metadata](#metadata)
- [Header map](#header-map)

## Conventions

These hold across the whole API.

**Errors are `std::expected`.** Nothing throws for a stream-level or configuration problem.
`FrameError` covers encoding, `DecodeError` decoding, `ScanError` scanning, `WavError` file
I/O, `MuxError` muxing. `DecodeError`, `ScanError`, `WavError` and `MuxError` each have a
`describe()` returning a `std::string_view`; `FrameError` does not.

**Audio is `float`, nominally in [-1, 1).** Internally the transform runs in `double`.

**Channels are passed as `std::span<const std::span<const float>>`.** The inner spans must
outlive the call. Build the outer vector once and refill the buffers underneath it — a fresh
vector of spans per frame is a pure waste.

**Channel order is A/52 Table 5.8, not WAV order.** That is `L, C, R, SL, SR` with LFE last,
against WAVE_FORMAT_EXTENSIBLE's `FL, FR, FC, LFE, BL, BR`. `ac3::io::ac3_layout_for` and
`ac3::io::wav_channel_order` give you the permutation both ways; use them rather than writing
it out again.

**Encoders are stateful and per-stream.** They carry MDCT overlap, the 44.1 kHz rate
accumulator, and the DRC and heavy-compression controllers, all of which smooth across frames.
One encoder per stream, fed in order.

**Each `encode_frame` call takes exactly `ac3::kSamplesPerFrame` (1536) samples per channel.**
Short-changing it is a programming error, not a runtime one.

## AC-3: `ac3::FrameEncoder`

`ac3/encoder/encoder.hpp`. One call, one syncframe.

```cpp
ac3::FrameEncoder encoder{{
    .bitrate_kbps = 448,
    .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
    .lfe = true,
}};

// Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
// encode_frame takes a span of spans, so the views must outlive the call.
// Build them once and refill the buffers underneath each frame.
const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

std::vector<std::byte> stream;
for (int frame = 0; frame < 31; ++frame) {  // 48000 / 1536, near enough
    fill_with_audio(pcm, frame, 48000.0);

    const auto encoded = encoder.encode_frame(views);
    if (!encoded) {
        std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
        return 1;
    }
    write(stream, *encoded);  // one complete syncframe
}
```

Full program: [`examples/encode_ac3.cpp`](../examples/encode_ac3.cpp).

### `EncoderConfig`

| Field | Default | Notes |
|---|---|---|
| `sample_rate` | `k48000` | Also `k44100`, `k32000`. |
| `bitrate_kbps` | 192 | Must be one of the 19 Table 5.18 rates; `ac3::is_valid_bitrate` checks. |
| `dialnorm` | 31 | 1–31 (§5.4.2.8). 31 means "no attenuation", which is a claim about your content. |
| `chbwcod` | -1 | Coded bandwidth, 0–60. -1 derives it from the bit rate. |
| `acmod` | `k2_0` | Table 5.8. `kDualMono` is rejected. |
| `lfe` | `false` | Adds one channel, coded last. |
| `coupling` | `false` | §7.4. Needs ≥ 2 full-bandwidth channels. |
| `cplbegf`, `cplendf` | -1, -1 | Sub-band indices; -1 lets the encoder choose from the per-channel rate. |
| `drc` | none | `std::optional<meta::Profile>`. Absent leaves `dynrnge` clear in every block. |
| `heavy` | none | `std::optional<meta::HeavyConfig>`. Independent of `drc`. |
| `cmixlev`, `surmixlev` | −4.5 dB, −6 dB | Tables 5.9/5.10. Always define the §7.8 downmix, whatever `acmod` is. |

Coupling is what makes 5.1 viable below 448 kbit/s: above the coupling frequency the
full-bandwidth channels stop carrying their own coefficients and share one coupling channel
plus per-band coordinates.

## E-AC-3: `ac3::eac3::FrameEncoder`

`ac3/encoder/eac3_frame.hpp`. Same shape, different container. E-AC-3 is not an AC-3 variant:
no `crc1`, an arbitrary 11-bit `frmsiz` instead of a size table (so the 44.1 kHz padding
alternation disappears), and exponent strategies for all six blocks hoisted into a frame-level
`audfrm`.

```cpp
ac3::eac3::FrameEncoder encoder{{
    .bitrate_kbps = 192,
    .acmod = ac3::Acmod::k2_0,
}};

std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
const auto views = views_of(pcm);

std::vector<std::byte> stream;
for (int frame = 0; frame < 31; ++frame) {
    fill_tones(pcm, tones, frame, 48000.0);
    const auto encoded = encoder.encode_frame(views);
    if (!encoded) {
        return 1;
    }
    stream.insert(stream.end(), encoded->begin(), encoded->end());
}
```

`FrameConfig` carries everything `EncoderConfig` does, plus the Annex E tools:

| Field | Default | Notes |
|---|---|---|
| `spx`, `spxbegf` | `false`, -1 | Spectral extension (§E3.6). Above the extension frequency nothing is coded: the decoder copies a lower band up, blends noise, and scales to a transmitted envelope. Cheaper and cruder than coupling, so the two stack. |
| `spx_atten`, `spxattencod` | `true`, -1 | The §E3.6.4.2.3 notch across the seam. Six bits per channel per frame. |
| `aht`, `gaqmod` | `false`, -1 | Adaptive hybrid transform (§E3.4): a second 6-point DCT down each bin across the frame's six blocks. Decided per channel per frame — setting the flag permits it, not forces it. |
| `coupling`, `cplbegf` | `false`, -1 | §E3.3. With `spx` also on, §E3.3.1 derives the coupling end frequency from `spxbegf`. |
| `mixing` | none | The `mixmdate` group (Table E1.2). E-AC-3 dropped `cmixlev`/`surmixlev` from `bsi` entirely, so without this the stream carries no downmix levels at all. |
| `strmtyp`, `substreamid`, `chanmap`, `last_dependent` | independent, 0, none, false | Substream identity. Set by `AccessUnitEncoder`; you rarely touch these directly. |
| `oba_complexity_index` | none | TS 103 420 §8.3 object count in `addbsi`. This is the marker FFmpeg keys its "Dolby Digital Plus + Dolby Atmos" report off. |

> The in-repo decoder refuses `spx`, `aht` and Annex E coupling. FFmpeg reads all three. See
> the verification-gap table in the [README](../README.md#verification-gaps) before relying on
> a round trip.

## Wide layouts: `ac3::eac3::AccessUnitEncoder`

Anything past 5.1 rides in *dependent substreams* beside a self-sufficient 5.1 bed. Every
substream codes the same 1536 samples of the same programme; a dependent contributes only its
own channels, its `chanmap`, and its share of the bit rate.

```cpp
// The bed is self-sufficient: a decoder that reads only the independent
// substream gets a complete 5.1 programme.
ac3::eac3::AccessUnitConfig config;
config.independent = {
    .bitrate_kbps = 384,
    .acmod = ac3::Acmod::k3_2,
    .lfe = true,
};
// Each dependent gets its own slice of the rate — substreams share a frame
// period, not a frame — and a Table E2.5 chanmap naming where its channels
// belong. Per §E3.8.2 the locations that collide with the bed replace it
// and the rest extend the layout.
config.dependents.push_back({
    .bitrate_kbps = 192,
    .acmod = ac3::Acmod::k2_2,
    .chanmap = ac3::eac3::chanmap::k71Rear,  // Ls, Rs, Lrs, Rrs
});
config.dependents.push_back({
    .bitrate_kbps = 192,
    .acmod = ac3::Acmod::k2_2,
    .chanmap = ac3::eac3::chanmap::kTopQuad,  // Vhl, Vhr, Lts, Rts
});

ac3::eac3::AccessUnitEncoder encoder{config};
```

Channels are grouped by substream in transmission order: the independent's first in Table 5.8
order with LFE last, then each dependent's in the order its `chanmap` names them.
`encoder.channel_count()` is the total, and the span count `encode_access_unit` expects.

```cpp
const auto unit = encoder.encode_access_unit(views);
// unit->bytes is the wire order already; substream_bytes records the
// per-substream boundaries, which crc2 is computed over.
stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
```

Full program: [`examples/encode_eac3.cpp`](../examples/encode_eac3.cpp).

Constraints: at most 8 dependents; a dependent may not disagree with its parent on sample
rate; and the locations a `chanmap` names must add up to the channels its `acmod` and `lfeon`
actually code, or you get `FrameError::kInvalidChannelMap`. A single dependent codes at most 5
full-bandwidth channels, which is why 7.1.4 — six channels beyond the bed — needs two.

Useful `chanmap` constants (`ac3/core/eac3_tables.hpp`, Table E2.5):

| Constant | Channels | Gives you |
|---|---|---|
| `k71Rear` | Ls, Rs, Lrs, Rrs | 7.1 (the surrounds replace the bed's, the rears are new) |
| `k512Height` | Vhl, Vhr | 5.1.2 |
| `kTopQuad` | Vhl, Vhr, Lts, Rts | 5.1.4 |
| `k71Rear` + `kTopQuad` | both of the above | 7.1.4, in two dependents |

`chanmap::expand(map)` turns a map into a `Layout` you can iterate, and `chanmap::name`
gives each location's short name.

## Reading a stream: `ac3::io::scan`

`ac3/io/elementary.hpp`. Finds access-unit boundaries in raw bytes and reports what the stream
carries, without being told. This is what a muxer needs, and deriving it from the bitstream
beats asking a caller who can be wrong.

```cpp
// Spans in the result point into `stream`, so it has to outlive them.
const auto scanned = ac3::io::scan(stream);
if (!scanned) {
    std::printf("scan failed: %.*s\n",
                static_cast<int>(ac3::io::describe(scanned.error()).size()),
                ac3::io::describe(scanned.error()).data());
    return 1;
}
std::printf("%s, %u Hz, %d channels, %zu access units\n",
            scanned->kind == ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3",
            ac3::sample_rate_hz(scanned->sample_rate), scanned->channels,
            scanned->access_units.size());
```

`ScannedStream::channels` is what the stream **renders**, which for E-AC-3 folds in every
dependent substream's `chanmap` — it is not the bed's channel count. `access_units` holds one
span per AC-3 syncframe, or per E-AC-3 independent substream together with the dependents
following it.

Both formats put `bsid` at bit 40 deliberately, so a reader can tell them apart before
committing to a layout. `ac3::stream_bsid` exposes that on its own.

## Decoding

`ac3/decoder/decoder.hpp`. Two classes, one per generation.

```cpp
// AC-3: one syncframe per access unit. For E-AC-3 use ac3::Eac3Decoder and
// decode_access_unit, which applies the §E3.8.2 render across substreams.
ac3::FrameDecoder decoder;
for (const auto unit : scanned->access_units) {
    const auto decoded = decoder.decode_frame(unit);
    if (!decoded) {
        std::printf("decode failed: %.*s\n",
                    static_cast<int>(ac3::describe(decoded.error()).size()),
                    ac3::describe(decoded.error()).data());
        return 1;
    }
    samples += decoded->channels.front().size();
}
```

Full program: [`examples/decode_stream.cpp`](../examples/decode_stream.cpp).

Both decoders keep overlap-add state, so feed frames in order. `Eac3Decoder` keys its state on
`strmtyp` and `substreamid` together, because a dependent's id lives in its own numbering
space — stepping through syncframes by hand with `decode_substream` gives the same audio as
calling `decode_access_unit`.

`DecodedFrame` reports the metadata separately from applying it, which is the point of the
decoder as a check on the encoder: a test can assert on the `dynrng` words the encoder chose
*and* on the level change they cause, and those are two different claims.

| `DecoderConfig` | Default | Notes |
|---|---|---|
| `drc_scale` | 0.0 | §7.7.1 partial compression. 0 ignores `dynrng`; 1 applies it as encoded. A/52 says a consumer decoder should default to applying it — this one defaults to 0 because a reference that silently rescales its output is not a reference. |
| `heavy_compression` | `false` | §7.7.2: prefer `compr` where it exists, falling back on `dynrng` for syncframes that carry none. |

What both decoders refuse, cleanly, rather than mis-decoding: block switching, delta bit
allocation, dual mono. The E-AC-3 decoder additionally refuses Annex E coupling, spectral
extension, AHT, transient pre-noise processing, and `fscod2` half sample rates.

## The spatial object layer

`ac3/spatial/spatial.hpp`. Mono sources placed on the ITU-R BS.775 ring, rendered to a 5.1
bed. This is the plain-AC-3 object path: the output is an ordinary 5.1 stream and *nothing
survives about where the object was*.

```cpp
ac3::spatial::BedRenderer renderer;
// add_object allocates, so call it before rendering starts.
const std::size_t object = renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7});
```

Rendering is clocked at the 256-sample block, because that is the rate automation runs at;
gains ramp linearly within a block, so moving an object does not click. The render path itself
does not allocate.

```cpp
// One full turn every two seconds.
renderer.set_target(object, {.azimuth_deg = 180.0 * seconds, .gain = 0.7});

// Six writable 256-sample spans into this block of the frame:
// L, C, R, SL, SR, LFE. render_block overwrites them.
std::array<std::span<float>, 6> block_out{};
for (std::size_t ch = 0; ch < 6; ++ch) {
    block_out[ch] = std::span<float>{bed[ch]}.subspan(
        static_cast<std::size_t>(block * ac3::spatial::kBlockSamples),
        ac3::spatial::kBlockSamples);
}
const std::array<std::span<const float>, 1> audio{std::span<const float>{source}};
renderer.render_block(audio, block_out);
```

Full program: [`examples/spatial_objects.cpp`](../examples/spatial_objects.cpp).

`pan_azimuth(deg)` and `pan_room(x, y)` expose the panner directly if you want the gains
without the renderer. Both are energy-normalized pairwise (VBAP on the horizontal ring),
Σg² = 1.

There is no `z`. A 5.1 ring has no height speakers, so a raised source folds onto the ring at
its azimuth, at full level. Objects never reach the LFE by panning — `lfe_send` is the only
route.

## Objects with metadata: `ac3::oba::AtmosEncoder`

`ac3/oba/atmos.hpp`. The same objects, but their positions survive: the output is one ordinary
5.1 E-AC-3 stream with OAMD and JOC payloads riding beside it in an EMDF container
(TS 102 366 Annex H, carried in a block skip field). A decoder that knows about neither plays
the bed unchanged, at full level — that is the design target, not a fallback.

```cpp
constexpr int kObjects = 3;
// Object metadata competes with the mantissas for the same frame, so an
// object stream wants more headroom than a plain 5.1 one.
ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
```

```cpp
// Positions are room-anchored per §4.2.1: x 0 at the left wall to 1 at
// the right, y 0 front to 1 back, z 0 floor to 1 ceiling.
std::array<ac3::oba::ObjectPlacement, kObjects> placement{};
placement[obj] = {
    .position = {.x = 0.5 + 0.45 * std::cos(angle),
                 .y = 0.5 + 0.45 * std::sin(angle),
                 .z = 0.25 * static_cast<double>(obj)},
    .gain = 1.0,
};

const auto unit = encoder.encode_frame(views, placement);
```

Full program: [`examples/atmos_objects.cpp`](../examples/atmos_objects.cpp).

| `AtmosConfig` | Default | Notes |
|---|---|---|
| `bitrate_kbps` | 448 | Per substream, as everywhere else. |
| `num_bands_idx` | 4 | Index into `joc::kNumBands` (Table 50). More bands cost codewords without giving the matrix anything new to say. |
| `fine_quant` | `false` | §6.3.3.7's half-step quantizer, roughly one more bit per coefficient. Worth it when objects are nearly degenerate. |

At most 16 objects (`joc::kMaxObjects`, per TS 103 420 §8.3.2.2). `encoder.bed()` returns the
5.1 bed the last frame encoded — what a legacy decoder hears, and the thing most worth
checking — and `encoder.parameters()` the pre-quantization reconstruction matrix.

The matrix is the minimum mean-square estimate `M = P Dᵀ (P D Dᵀ + εI)⁻¹`. Because the encoder
built the downmix it knows `D` exactly rather than estimating it, which makes the solve
near-exact for well-separated objects. Two limits are structural, not bugs: objects sharing a
direction cannot be separated by any linear combination of the bed, and Dolby's decoder will
not treat these as objects at all. Both are covered in the
[README](../README.md#verification-gaps).

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

Full program: [`examples/mux_mkv.cpp`](../examples/mux_mkv.cpp).

`mux` returns the whole file as bytes and does no file I/O, which keeps it testable without a
disk. It writes one audio track, one SimpleBlock per frame, clusters closed on a time budget,
and Info with TimestampScale and Duration. No SeekHead, no Cues, no chapters, no tags — those
matter for seeking in large files, not for playing back what this project produces.

## Metadata

`ac3/meta/`. Everything here is optional; leaving it out produces a stream bit-identical to
one from before the metadata layer existed. An AV receiver reads exactly these bits to set
level, compress dynamics and fold down to fewer speakers than the stream carries.

`dialnorm` cannot be derived from the frame being encoded — BS.1770 gating is defined over the
whole programme — so measure first and configure second:

```cpp
// Weights follow BS.1770 Table 3: unity front, +1.5 dB surrounds, LFE
// excluded outright.
ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, kAcmod, kLfe};
for (int frame = 0; frame < kFrames; ++frame) {
    fill(pcm, frame);
    meter.push(views);
}

// nullopt until at least one 400 ms block has passed the absolute gate:
// silence has no meaningful loudness, and inventing one would put a wrong
// dialnorm on the stream.
const auto lkfs = meter.integrated_lkfs();
const int dialnorm = lkfs ? ac3::meta::dialnorm_from_lkfs(*lkfs) : 31;
```

```cpp
ac3::FrameEncoder encoder{{
    .bitrate_kbps = 448,
    .dialnorm = dialnorm,
    .acmod = kAcmod,
    .lfe = kLfe,
    // §7.7.1. A/52 fixes the wire format and the intent but never the
    // curve, so the profile is this project's reading of it.
    .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
    // §7.7.2, independent of drc: the two answer different questions, so a
    // stream may carry either, both or neither.
    .heavy = ac3::meta::HeavyConfig{.dialogue_target_dbfs = -20.0,
                                    .peak_ceiling_dbfs = -0.5},
    // Tables 5.9 / 5.10. These always define the §7.8 downmix, whatever
    // acmod is, so the heavy-compression peak detector consults them too.
    .cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB,
    .surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB,
}};
```

Full program: [`examples/metadata.cpp`](../examples/metadata.cpp).

Note `meta::Profile` is the curve and `meta::ProfileId` the name of one; `meta::profile(id)`
converts. The five ids are `kFilmStandard`, `kFilmLight`, `kMusicStandard`, `kMusicLight` and
`kSpeech`.

The K-weighting in `ac3/meta/loudness.hpp` is designed analytically rather than tabulated, so
44.1 and 32 kHz work too. Calibration: a 1 kHz tone at −20 dBFS reads −19.99 LKFS, matching
FFmpeg's `ebur128` to 0.01 LU.

`ac3/meta/mixing.hpp` holds the downmix levels — `CentreMixLevel` and `SurroundMixLevel` for
AC-3, `MixMetadata` for the E-AC-3 `mixmdate` group. In `MixMetadata`, an absent
`lfemixlevcod` means LFE mixing is *disabled*, which per §E2.3.1.10 is a decision in its own
right and not the same as sending code 31.

## Header map

| Header | Contents |
|---|---|
| `ac3/core/tables.hpp` | `SampleRate`, `Acmod`, `ExpStrategy`, frame constants, Table 5.18. Nearly everything includes it. |
| `ac3/core/eac3_tables.hpp` | Annex E: `StreamType`, the Table E2.5 `chanmap` namespace, `Layout`. Also the general channel model: `chanmap::ChannelPlan` (a bed acmod/lfe plus however many dependent chanmaps it takes) and `chanmap::allocate(locations)`, which partitions an arbitrary Table E2.5 location bitmask into one — the algorithm `ac3::plan::LayoutId`'s seven named layouts are now themselves expressed through. |
| `ac3/core/bitreader.hpp`, `bitwriter.hpp` | MSB-first bit I/O. |
| `ac3/core/mdct.hpp`, `window.hpp` | The 512-point MDCT and the KBD window. |
| `ac3/core/bitalloc.hpp`, `exponents.hpp`, `mantissas.hpp` | The §7.2 allocation model and §7.1/§7.3 coding, shared by encoder and decoder. |
| `ac3/encoder/encoder.hpp` | `EncoderConfig`, `FrameEncoder`. |
| `ac3/encoder/eac3_frame.hpp` | `FrameConfig`, `FrameEncoder`, `AccessUnitConfig`, `AccessUnitEncoder`, `AccessUnit`. |
| `ac3/encoder/silent_frame.hpp` | `FrameError`, and pure-syntax silent frames. |
| `ac3/encoder/coupling.hpp`, `eac3_tools.hpp` | Coupling, spectral extension, AHT. |
| `ac3/encoder/plan.hpp` | `ac3::plan` — `Plan`, `Codec`, `LayoutId`, `Tools`, `Metadata`, `Routing`. The layout table, Annex E tool tokens, metadata defaults and source-to-coded-channel routing, shared by `ac3cli` and `ac3gui` so the two front ends cannot disagree about what a layout or a tools token means. `Plan::custom_locations`, `resolve(plan)` and `parse_channels`/`format_channels` are the escape hatch onto `chanmap::allocate` for a channel set none of the seven named `LayoutId`s cover. |
| `ac3/decoder/decoder.hpp` | `FrameDecoder`, `Eac3Decoder`, `split_frames`, `split_access_units`, `stream_bsid`. |
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
