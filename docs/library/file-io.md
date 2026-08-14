# File I/O: `ac3::io::wav`

`ac3/io/wav.hpp`. Minimal WAV reading and writing — PCM16 and float32 only, which is everything
this project produces or consumes — shared by the CLI and the GUI so neither carries its own
copy. Every other example in this section stays in memory: PCM is synthesized straight into an
encoder and decoded samples are only ever counted. A real pipeline reads a file a caller handed
it and writes one back, which means crossing WAV's own channel order
(`WAVE_FORMAT_EXTENSIBLE`: FL, FR, FC, LFE, BL, BR) against A/52 Table 5.8's (L, C, R, SL, SR,
LFE) twice — once on the way in, once on the way out.

```cpp
// Synthesize 5.1 in AC-3 order and write it out in WAV order -
// wav_channel_order says where each AC-3 channel belongs in the interleave.
const auto write_order = ac3::io::wav_channel_order(kAcmod, kLfe);
ac3::io::write_wav_f32(source_path, ac3_order, 48000, write_order);
```

```cpp
// Read it back - read_wav hands the samples back in WAV order, so
// ac3_layout_for's wav_index permutes them onto AC-3 channel k.
const auto read = ac3::io::read_wav(source_path);
const auto layout = ac3::io::ac3_layout_for(read->channels.size());
std::vector<std::vector<float>> from_wav(layout->wav_index.size());
for (std::size_t k = 0; k < layout->wav_index.size(); ++k) {
    from_wav[k] = read->channels[layout->wav_index[k]];
}
```

Full program: [`examples/wav_roundtrip.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/wav_roundtrip.cpp)
— writes a 5.1 WAV, reads it back, encodes and decodes it, and writes the decoded result out
as a second WAV.

`ac3_layout_for(wav_channels)` and `wav_channel_order(acmod, lfe)` are exact inverses of each
other for the six widths WAV convention actually names (mono through 5.1); a width neither
covers (2/1, 3/1, 1+1) falls back to the codec's own channel order unchanged, since there is no
WAV convention to translate against.

`WavData::channels` is one `std::vector<float>` per channel, normalized to `[-1, 1)`, in
whatever order the file itself interleaves — `read_wav` does not reorder for you. `WavError`
covers open/parse failure the same way every other module here reports errors:
`kCannotOpen`, `kNotRiffWave`, `kUnsupportedFormat` (not PCM16 or float32) and `kTruncated`.

`WavStreamWriter` is the incremental sibling for a take too long to hold in memory — it opens
once, takes interleaved samples as they arrive, and needs a periodic `flush_header()` call so a
process killed mid-session leaves a file whose header matches what was actually written rather
than claiming zero data.

---

See also: [Encoding AC-3](encoding-ac3.md) — what a WAV's samples are fed to once they're in
AC-3 channel order; [Decoding](decoding.md) — the other end of the round trip.
