# Encoding E-AC-3

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
> the verification-gap table in the [README](https://github.com/iainchesworth/ac3forge/blob/main/README.md#verification-gaps) before relying on
> a round trip.

`FrameConfig::dialnorm2` (see "Dual mono" in [Encoding AC-3](encoding-ac3.md)) works exactly
the same way here: set it alongside `dialnorm` when `acmod` is `kDualMono`. Dual mono is always a
lone independent substream with no dependents — 1+1 has no bed/dependent split to make — so
`AccessUnitEncoder` gives Ch2 its own `RangeController`/`HeavyCompressor` too, measured on the
independent substream's own two channels the same way it measures Ch1's.

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

Full program: [`examples/encode_eac3.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/encode_eac3.cpp).

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

---

See also: [Metadata](metadata.md) — mix-level and DRC fields shared with AC-3, plus the
E-AC-3-only `mixing` group; [Encoding AC-3](encoding-ac3.md) — the single-substream base case.
