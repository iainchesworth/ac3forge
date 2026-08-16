# Example programs

Every program in [`examples/`](https://github.com/iainchesworth/ac3forge/tree/main/examples)
builds by default (`AC3FORGE_BUILD_EXAMPLES=ON`) and registers as a `ctest` entry named
`example.<name>`, so "the examples still work" is checked by the same command as everything else
(`read_adm` additionally needs `-DAC3FORGE_BUILD_ADM=ON`). These programs are also the source
the library pages excerpt from — each page's "Full program" link lands on one of them.

## Encoding

| Example | What it shows | Discussed in |
|---|---|---|
| [`encode_ac3`](https://github.com/iainchesworth/ac3forge/blob/main/examples/encode_ac3.cpp) | Encode synthesized 5.1 PCM to an AC-3 elementary stream. | [Encoding AC-3](encoding-ac3.md) |
| [`encode_eac3`](https://github.com/iainchesworth/ac3forge/blob/main/examples/encode_eac3.cpp) | E-AC-3 in both shapes: `FrameEncoder` up to 5.1, and `AccessUnitEncoder` bed + dependents for 7.1.4. | [Encoding E-AC-3](encoding-eac3.md) |
| [`metadata`](https://github.com/iainchesworth/ac3forge/blob/main/examples/metadata.cpp) | A DRC profile, heavy compression, and a measured BS.1770 dialnorm. | [Metadata](metadata.md) |
| [`custom_layout`](https://github.com/iainchesworth/ac3forge/blob/main/examples/custom_layout.cpp) | A layout no named `LayoutId` covers, via `Plan::custom_locations`. | [Channel plans & routing](channel-plans-and-routing.md) |
| [`multi_source_assignment`](https://github.com/iainchesworth/ac3forge/blob/main/examples/multi_source_assignment.cpp) | Two sources onto one stream by explicit assignment — what backs the CLI's `src=`/`map=`. | [Channel plans & routing](channel-plans-and-routing.md) |

## Decoding & analysis

| Example | What it shows | Discussed in |
|---|---|---|
| [`decode_stream`](https://github.com/iainchesworth/ac3forge/blob/main/examples/decode_stream.cpp) | Scan an unknown elementary stream, then decode it with the right decoder. | [Decoding](decoding.md) |
| [`decode_robustness`](https://github.com/iainchesworth/ac3forge/blob/main/examples/decode_robustness.cpp) | Skip a damaged mid-stream frame and keep decoding. | [Decoding](decoding.md) |
| [`level_metering`](https://github.com/iainchesworth/ac3forge/blob/main/examples/level_metering.cpp) | Decode and meter: per-channel peak/RMS plus the speaker-ring energy vector. | [Muxing & sinks](muxing-and-sinks.md) |
| [`qc_report`](https://github.com/iainchesworth/ac3forge/blob/main/examples/qc_report.cpp) | Decode, measure BS.1770-4/Tech 3342, and check named QC delivery gates. | [Metadata](metadata.md) |

## Spatial & Atmos

| Example | What it shows | Discussed in |
|---|---|---|
| [`spatial_objects`](https://github.com/iainchesworth/ac3forge/blob/main/examples/spatial_objects.cpp) | Pan a moving mono object onto the BS.775 ring, into plain 5.1 AC-3. | [Spatial & Atmos objects](spatial-and-atmos.md) |
| [`atmos_objects`](https://github.com/iainchesworth/ac3forge/blob/main/examples/atmos_objects.cpp) | Panned objects as Atmos-in-DD+, atop a legacy-playable 5.1 bed. | [Spatial & Atmos objects](spatial-and-atmos.md) |
| [`scripted_object_motion`](https://github.com/iainchesworth/ac3forge/blob/main/examples/scripted_object_motion.cpp) | Objects driven by authored `OrbitPath`/`KeyframePath` motion. | [Spatial & Atmos objects](spatial-and-atmos.md) |
| [`atmos_fallback`](https://github.com/iainchesworth/ac3forge/blob/main/examples/atmos_fallback.cpp) | The same programme with and without the EMDF container — the either/or 5.1-fallback tradeoff. | [Spatial & Atmos objects](spatial-and-atmos.md) |
| [`object_signing`](https://github.com/iainchesworth/ac3forge/blob/main/examples/object_signing.cpp) | Sign an Atmos stream's EMDF container with an operator key. | [Object signing](signing.md) |
| [`station_broadcast`](https://github.com/iainchesworth/ac3forge/blob/main/examples/station_broadcast.cpp) | A fully worked 115-second diegetic Atmos scene with authored flight paths. | [A worked scene — station broadcast](station-broadcast.md) |

## Containers

| Example | What it shows | Discussed in |
|---|---|---|
| [`mux_mkv`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_mkv.cpp) | Wrap in Matroska, the track header kept honest by a bitstream scan. | [Muxing & sinks](muxing-and-sinks.md) |
| [`mux_mp4`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_mp4.cpp) | Wrap in MP4, with the `dec3`/Atmos box built from the bitstream. | [Muxing & sinks](muxing-and-sinks.md) |
| [`mux_fmp4`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_fmp4.cpp) | Fragment into CMAF and emit HLS/DASH manifests. | [Muxing & sinks](muxing-and-sinks.md) |
| [`mux_ts`](https://github.com/iainchesworth/ac3forge/blob/main/examples/mux_ts.cpp) | Wrap in MPEG-2 TS with the right PMT descriptor. | [Muxing & sinks](muxing-and-sinks.md) |

## File I/O & ADM

| Example | What it shows | Discussed in |
|---|---|---|
| [`wav_roundtrip`](https://github.com/iainchesworth/ac3forge/blob/main/examples/wav_roundtrip.cpp) | Real WAV in → encode → decode → WAV out, crossing the WAV↔A/52 channel order both ways. | [File I/O](file-io.md) |
| [`read_adm`](https://github.com/iainchesworth/ac3forge/blob/main/examples/read_adm.cpp) | Open an ADM BW64 file and print the parsed graph — needs `-DAC3FORGE_BUILD_ADM=ON`. | [ADM / BW64 reading](adm.md) |
