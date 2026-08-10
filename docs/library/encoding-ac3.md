# AC-3: `ac3::FrameEncoder`

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

Full program: [`examples/encode_ac3.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/encode_ac3.cpp).

## `EncoderConfig`

| Field | Default | Notes |
|---|---|---|
| `sample_rate` | `k48000` | Also `k44100`, `k32000`. |
| `bitrate_kbps` | 192 | Must be one of the 19 Table 5.18 rates; `ac3::is_valid_bitrate` checks. |
| `dialnorm` | 31 | 1–31 (§5.4.2.8). 31 means "no attenuation", which is a claim about your content. |
| `chbwcod` | -1 | Coded bandwidth, 0–60. -1 derives it from the bit rate. |
| `acmod` | `k2_0` | Table 5.8, including `kDualMono` (1+1) — see below. |
| `dialnorm2` | none | `std::optional<int>`. Ch2's own dialnorm (§5.4.2.16); required when `acmod` is `kDualMono`, meaningless otherwise. |
| `lfe` | `false` | Adds one channel, coded last. |
| `coupling` | `false` | §7.4. Needs ≥ 2 full-bandwidth channels. |
| `cplbegf`, `cplendf` | -1, -1 | Sub-band indices; -1 lets the encoder choose from the per-channel rate. |
| `drc` | none | `std::optional<meta::Profile>`. Absent leaves `dynrnge` clear in every block. |
| `heavy` | none | `std::optional<meta::HeavyConfig>`. Independent of `drc`. |
| `cmixlev`, `surmixlev` | −4.5 dB, −6 dB | Tables 5.9/5.10. Always define the §7.8 downmix, whatever `acmod` is. |

Coupling is what makes 5.1 viable below 448 kbit/s: above the coupling frequency the
full-bandwidth channels stop carrying their own coefficients and share one coupling channel
plus per-band coordinates.

### Block switching

Automatic, like delta bit allocation below — no config field toggles it. A §8.2.2 transient
detector runs per full-bandwidth channel per block; a channel that switches anywhere in the frame
is excluded from coupling for that whole frame, since `chincpl` here is frame-wide all-or-nothing
rather than a per-channel flag. The LFE never switches.

### Dual mono (`acmod` 0, "1+1")

Not a channel layout — two independent, single-channel programmes sharing one syncframe (a
second language track, a commentary track), each levelled and compressed on its own. Set
`acmod = ac3::Acmod::kDualMono` and `dialnorm2`; `channels[0]` is Ch1, `channels[1]` is Ch2
(`fullbw_channel_count(kDualMono)` is 2, same as stereo, but the two channels are never
downmixed, coupled or rematrixed together — coupling silently stays off even if `coupling` is
set, since averaging two unrelated programmes together would leak one into the other). `heavy`
and `drc`, if set, apply to both channels independently — each gets its own compressor/range
controller and its own `compr`/`compr2` and `dynrng`/`dynrng2` words. There's no LFE: 1+1 has no
soundfield for a subwoofer to sit in.

---

See also: [Metadata](metadata.md) — `dialnorm`, `drc`, `heavy` and the mix-level fields above
are all configured via the metadata layer; [Encoding E-AC-3](encoding-eac3.md) — same shape,
different container.
