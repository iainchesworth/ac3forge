# Using ac3::forge

The public API is the headers under `src/lib/include/ac3/`. Link `ac3::forge`; link
`matroska::matroska` and/or `mp4::mp4` as well if you want a container writer, or `ac3adm::ac3adm`
if you want to read a professional ADM BWF master — the one module in this list that is a reader
rather than a writer, and so does not need `ac3::forge` linked alongside it at all. Unlike every
other module here, `ac3adm::ac3adm` is opt-in: it is only built with `-DAC3FORGE_BUILD_ADM=ON`
(default off), and needs several Boost header libraries pulled in via
`-DVCPKG_MANIFEST_FEATURES=adm` — see [ADM / BW64 reading](adm.md) for why.

**In-tree** (this repo `add_subdirectory`'d into a larger build, or as a git submodule):

```cmake
target_link_libraries(your_target PRIVATE ac3::forge)
```

`ac3::forge` resolves to whichever of the static or shared build the enclosing project's
`BUILD_SHARED_LIBS` asks for.

**Installed package**, from an `ac3forge-dev-*` package (see
[docs/releasing.md](../releasing.md#what-gets-published)) or a local `cmake --install`:

```cmake
find_package(ac3forge REQUIRED)
target_link_libraries(your_target PRIVATE ac3::forge_static)   # or ac3::forge_shared
```

An installed package has no ambient `BUILD_SHARED_LIBS` default to resolve against, so it
exports both variants explicitly rather than a bare `ac3::forge` — pick the one you want.
Neither the package nor the codec itself has any dependency of its own to find: no
`find_dependency()` calls, no system or third-party library, static or shared. (`ac3adm::ac3adm`
is the sole exception project-wide — see the note above — and for that reason is not part of the
installed `find_package(ac3forge)` package at all; consume it via `add_subdirectory` in-tree.)

**vcpkg.** A port lives in this repo at
[`packaging/vcpkg-port/ac3forge/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/vcpkg-port/ac3forge) and is pending
submission to the curated `microsoft/vcpkg` registry (see
[docs/releasing.md](../releasing.md#vcpkg-port)) — until that lands, point vcpkg at it directly
with `--overlay-ports`/`VCPKG_OVERLAY_PORTS` (works from any clone of this repo, no waiting on
the upstream PR):

```bash
vcpkg install ac3forge --overlay-ports=/path/to/ac3forge/packaging/vcpkg-port
```

```cmake
find_package(ac3forge CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ac3::forge)
```

The three container writers are the port's `matroska`/`mp4`/`mpegts` features, all on by default
(`matroska::matroska`/`mp4::mp4`/`mpegts::mpegts` become available); leave any or all out with
`vcpkg install ac3forge[core]` (none of them) or `ac3forge[core,mp4]` (just `mp4`) if you only
want a subset, or only the codec. `ac3adm::ac3adm` has no vcpkg feature — see the note above,
it isn't part of this installed package at all. Once merged into `microsoft/vcpkg`, the same two
snippets work with a plain `vcpkg install ac3forge` — no `--overlay-ports` needed.

Live audio — capture, monitor playback, IEC 61937 passthrough — is `ac3::audio`
(`src/audio/`), a separate target `ac3cli`/`ac3gui` link alongside `ac3::forge` for their own
live-audio commands. It is **not** part of the distributed package: it isn't installed, isn't
exported, and `find_package(ac3forge)` says nothing about it. A consumer wanting live capture
on their own platform provides their own audio I/O and feeds the resulting PCM to the codec API
below directly — `ac3::audio` exists to serve this project's own CLI/GUI, not as something a
third party is expected to link.

Nearly every code block in this section is an excerpt from a program in
[`examples/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/examples) — see
[Example programs](examples.md) for the full list. What the build compiles and `ctest` runs is
the programs, not the excerpts: an example cannot stop working silently, but an excerpt is
re-synced by hand and can drift. Each page's "Full program" link is the canonical form.

## In this section

- [Example programs](examples.md) — every `examples/` program, what it shows, and which page discusses it.
- [Encoding AC-3](encoding-ac3.md) — `ac3::FrameEncoder` and `EncoderConfig`.
- [Encoding E-AC-3](encoding-eac3.md) — `ac3::eac3::FrameEncoder` and wide layouts via `ac3::eac3::AccessUnitEncoder`.
- [Decoding](decoding.md) — scanning a stream with `ac3::io::scan` and decoding it.
- [Spatial & Atmos objects](spatial-and-atmos.md) — the plain-AC-3 object layer and `ac3::oba::AtmosEncoder`.
- [A worked scene — station broadcast](station-broadcast.md) — a complete 115-second authored Atmos scene built on the object APIs.
- [Channel plans & routing](channel-plans-and-routing.md) — custom channel selections and multi-source assignment.
- [Metadata](metadata.md) — loudness, DRC and downmix metadata.
- [Muxing & sinks](muxing-and-sinks.md) — `matroska::mux`, `mp4::mux`, fMP4/CMAF + HLS/DASH
  (`mp4::fragment`, `mp4/hls.hpp`, `mp4/dash.hpp`), metering, the IEC 61937/passthrough/monitor
  sinks, and capture.
- [File I/O](file-io.md) — reading and writing WAV.
- [ADM / BW64 reading](adm.md) — `ac3adm::ac3adm`, a standalone BW64/RF64 + Audio Definition Model
  parser (opt-in, `-DAC3FORGE_BUILD_ADM=ON`).
- [ADM → Atmos bridging](adm-bridge.md) — `ac3::admbridge`, mapping the parsed ADM graph onto
  `ac3::oba::AtmosEncoder` (same opt-in flag).
- [Object signing](signing.md) — `ac3::signing`, the EMDF protection tag.
- [Header map](header-map.md) — every public header and what lives in it.
- [C API](c-api.md) — `ac3::forge_c`, a stable, minimal C-callable surface over encode/decode for
  bindings and embedding (roadmap item F1).

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
One encoder per stream, fed in order. The decoders are stateful the same way (overlap-add and
dither state). No encoder or decoder instance is safe for concurrent calls on the same
instance — the headers note that per-frame scratch and history members are reused across
calls — but separate instances share nothing and are independent.

**Each `encode_frame` call takes exactly `ac3::kSamplesPerFrame` (1536) samples per channel.**
Short-changing it is a programming error, not a runtime one.
