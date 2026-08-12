# Spatial & Atmos objects

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

Full program: [`examples/spatial_objects.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/spatial_objects.cpp).

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

Full program: [`examples/atmos_objects.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/atmos_objects.cpp).

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
not treat these as objects at all. Both are covered in
[Atmos & JOC](../concepts/atmos-joc.md#two-honest-limitations).

---

See also: [Encoding E-AC-3](encoding-eac3.md) — Atmos objects ride inside an ordinary E-AC-3
stream; [Header map](header-map.md) — `ac3/oba/motion.hpp` covers authored/orbit object paths,
which this page doesn't; [A worked scene: the station broadcast](station-broadcast.md) — a
complete 115-second authored scene built on this API, from synthesis to `.ec3`.
