# Changelog

*For end users tracking what has shipped. How releases and version numbers are cut lives in
[docs/releasing.md](docs/releasing.md); the project overview is in [README.md](README.md).*

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [docs/releasing.md](docs/releasing.md) for how releases and version numbers are cut.

## [Unreleased]

### WASM decode demo (roadmap F3)

- **`ac3::forge`'s decode path now builds under Emscripten.** A new `config-wasm-emscripten`
  CMake preset (`cmake/toolchains/wasm.emscripten.toolchain.cmake`) compiles `ac3::forge` to
  WebAssembly. Unlike every other platform preset, this one does not chainload through vcpkg:
  the codec's decode path has zero third-party dependencies (`vcpkg.json`'s own description
  says so — catch2 is tests-only, and tests are off for this preset), so going through vcpkg's
  community `wasm32-emscripten` triplet would only add cost for nothing it needs.
  `src/audio` (the project's one genuinely platform-specific library — WASAPI/ALSA/AAudio) is
  skipped entirely under this preset; a browser build gets live audio from the Web Audio API in
  JS instead.
- **A real browser demo**, `platform/wasm/` — a platform app in the same shape as
  `platform/android/`'s Shield demo, not an `examples/` snippet: an Embind wrapper
  (`decoder_bindings.cpp`) around the existing `FrameDecoder`/`Eac3Decoder` API, plus a page
  (`index.html`/`demo.js`) that fetches a raw `.ec3`/`.ac3` elementary stream (bundled, or
  user-uploaded), decodes it entirely client-side, plays the decoded bed audio through the Web
  Audio API, and visualizes real per-channel RMS energy on two speaker rings (ear-level and
  ceiling) — the coordinate model and screen-space transform ported from the desktop GUI's
  `SoundfieldView.qml`. A seek bar lets a viewer scrub the real decoded timeline. The bundled
  fixture is a real 8-second, 3-object Atmos-in-DD+ stream encoded with the existing
  `AtmosEncoder`. See [docs/platforms/wasm.md](docs/platforms/wasm.md).
- **Real object motion, not bed energy standing in for it.** With OAMD/JOC decode landed (see
  "Atmos object decode" below), the demo grew a second visualization — a top-down/elevation room
  view, the same layout as the desktop GUI's Objects tab — driven entirely by real decoded
  positions (`DecodedAccessUnit::object_metadata`) read back out of the bitstream, not an
  authored/preview model. A "solo object" control switches playback to that object's own real
  JOC-reconstructed audio (`::object_audio`) — its actual isolated waveform, not a re-panned
  approximation of its slice of the bed.
- **Embedded live in the docs site** at `docs/wasm-demo.md` (nav: Project → "Live decode demo
  (WASM)") via an iframe over a copy of the demo under `docs/assets/wasm-decode-demo/`. That copy
  is committed (a working fallback for a plain local `mkdocs build`), but the *published* site
  never ships it stale: `.github/workflows/docs.yml`'s `deploy` job installs Emscripten and
  rebuilds `platform/wasm/` fresh from source before publishing. `_build.yml`'s `build-wasm` job
  build-verifies the same target on every push, the same role `build-android`'s always-on debug
  APK plays. Both share a new pinned composite action, `.github/actions/setup-emscripten`.

### Atmos object decode

- **`Eac3Decoder` now reads OAMD, closing a real gap between what this library can encode and
  what it can decode.** Until now the E-AC-3 decoder read the EMDF-bearing block skip field
  (§5.4.3.58-60) and simply discarded it — every "object decode" claim about this project was
  true for the encoder only. `ac3::emdf::parse_container` is the decode-side inverse of
  `build_container` (ETSI TS 102 366 Annex H): it scans the skip field for the sync word, walks
  the payload list into `{id, bytes}` pairs, and refuses (rather than mis-parses) an
  `emdf_payload_config` outside the one shape TS 103 420 Table 56 mandates. `ac3::oba::parse_payload`
  is the matching inverse of OAMD's `build_payload` (TS 103 420 §5.5), reconstructing the
  `Program`/`DynamicObject`s a stream actually declared. `DecodedSubstream`/`DecodedAccessUnit`
  gain a new `object_metadata` field — `std::nullopt` for plain E-AC-3 with no object audio, or a
  skip field this decoder found but declined to interpret, so existing callers that only read
  `.channels` are unaffected either way. JOC's own per-object audio reconstruction is not part of
  this yet; OAMD alone already recovers each object's position and gain, verified end-to-end
  against `AtmosEncoder`'s own output.
- **`Eac3Decoder` now reads JOC too, and actually reconstructs each object's audio.**
  `ac3::joc::parse_payload` is the decode-side inverse of JOC's `build_payload` (TS 103 420 §6.2):
  the Huffman-coded, differentially-predicted coefficient matrix comes back dequantized and ready
  to use. `ac3::joc::reconstruct` then applies §6.6.6's reconstruction — this codebase has no
  complex QMF filterbank, so it runs the operation in the same 512-sample MDCT domain
  `AtmosEncoder`'s own `band_energy` already uses to derive the matrix in the first place (see
  that function's comment for why the substitution is legitimate), carrying real state
  (`ac3::joc::ReconstructionState`) frame to frame so §6.6.5's matrix ramp and each object's own
  overlap-add both have real continuity instead of restarting cold every frame. `DecodedSubstream`/
  `DecodedAccessUnit` gain a new `object_audio` field, one waveform per object parallel to
  `object_metadata->objects` — populated only for the dynamic-object-only (no bed) program shape
  where the two orderings are guaranteed to line up, which is the only shape `AtmosEncoder` itself
  ever produces. Verified against a genuinely moving three-object scene (`examples/atmos_objects.cpp`,
  now decodes what it encodes): mean recovered position error of 0.02 room units — essentially the
  quantizer's own step size — and 18-22 dB audio-tracking SNR per object across 59 frames of real
  circular motion, not a single static frame.

### Delivery

- **MP4/ISOBMFF muxer** (`mp4::mp4`, `ac3cli mp4 <in.ac3|in.ec3> <out.mp4>`) — a second
  standalone container writer alongside `matroska::matroska`, same shape: no dependency on
  `ac3::forge`, frames taken as opaque bytes. The `ec-3`/`ac-3` sample entry's `dec3`/`dac3`
  configuration box (ETSI TS 102 366 Annex F) is built straight off the bitstream by a new
  `ac3::io::build_codec_config_box`, Dolby Atmos signalling included —
  `flag_ec3_extension_type_a`/`complexity_index_type_a` (TS 103 420 §8.3) are set whenever
  `ac3::io::scan` finds the marker in the stream's own `addbsi`, which is exactly the signal
  FFmpeg's own MKV→MP4 remux path is documented to drop or mis-signal
  ([jellyfin-ffmpeg#584](https://github.com/jellyfin/jellyfin-ffmpeg/issues/584)).
- **`mpegts::mpegts`**, a third standalone container writer alongside `matroska::matroska` and
  `mp4::mp4` — PAT + PMT + one PES-wrapped AC-3/E-AC-3 elementary stream, PCR stamped
  every access unit, identified per the DVB profile (`stream_type` 0x06 plus the
  `AC3_descriptor`/`Enhanced_AC3_descriptor` ETSI EN 300 468 Annex D defines, not ATSC's). New
  `ac3cli ts` command and `examples/mux_ts.cpp`, same shape as `mkv`/`mux_mkv.cpp`.

- **Fragmented MP4/CMAF segmenting plus HLS/DASH signaling** (`mp4::fragment`, `mp4/hls.hpp`,
  `mp4/dash.hpp`, `ac3cli fmp4 <in.ac3|in.ec3> <out_dir> [frames_per_fragment]`) — the streaming-
  delivery follow-up `mp4::mux`'s own header comment deliberately left for later. `fragment()`
  lays out an initialization segment (`ftyp`+`moov`, `mvex`/`trex` in place of a populated sample
  table) and one CMAF media segment (`styp`+`moof`+`mdat`) per fragment (ISO/IEC 14496-12 §8.8,
  ISO/IEC 23000-19), from the same batch AudioTrack/frame shape `mux()` already takes.
  `build_hls_media_playlist`/`build_hls_master_playlist` emit a spec-correct HLS playlist pair
  (RFC 8216) and `build_dash_adaptation_set` a DASH `AdaptationSet` snippet (ISO/IEC 23009-1) for
  the identical segments — one CMAF segment format, two manifest flavors. Both get `CODECS`/
  `codecs` right (the bare `ac-3`/`ec-3` sample-entry fourcc, RFC 6381 §3 — neither codec
  registers a dot-separated profile suffix) and, for Dolby Digital Plus with Atmos objects, HLS's
  `CHANNELS="<N>/JOC"` (Apple's HLS Authoring Specification for Apple Devices, reiterated with a
  worked example by Dolby's own Online Delivery Kit docs and AWS MediaLive's HLS+Atmos
  documentation) — the same category of packaging correctness as the `dec3` box work above, and
  the same category of bug Shaka Packager and jellyfin-ffmpeg#584 have both hit around
  E-AC-3/Atmos-in-HLS signaling. The DASH snippet uses an exact
  `SegmentTemplate`/`SegmentTimeline` (ISO/IEC 23009-1 §5.3.9.6) rather than one nominal
  `duration`, avoiding a real gap found against FFmpeg's own `dash` demuxer while writing this: a
  flat nominal duration with a shorter final segment (the normal case) let it compute one too many
  segments from `mediaPresentationDuration` and request a segment number past the end.

- **Live sessions mux straight to Matroska.** `ac3cli record`/`ac3cli live` take a new
  `container=mkv` trailing token, writing the take directly to `.mkv` in the one command instead of
  needing a separate `ac3cli mkv` wrap afterward. The GUI's Live session tab already offers the same
  choice through its existing Container combo (Format tab) — its own live write path is rebuilt on
  top of a new incremental API, `matroska::Writer`, so a Matroska take is now written straight to
  its real destination as it is captured rather than spooled as an elementary stream and muxed only
  at a clean stop: Segment is written with EBML's reserved "unknown size" pattern (the standard way
  a streamed Matroska declares a length it cannot know yet), so a crash mid-session now leaves a
  genuinely playable, if truncated, `.mkv` behind instead of a recoverable elementary-stream
  companion file. `matroska::mux()` (the existing whole-file API `ac3cli mkv` and file-based GUI
  encodes use) is unchanged.
- **The GUI's Format tab now offers MP4, fragmented MP4/CMAF and MPEG-TS in its Container combo**,
  wired through the same `mp4::`/`mpegts::` writers the CLI's `mp4`/`fmp4`/`ts` commands use — for
  file encodes only; live sessions keep their incremental Matroska path.

### Production ingest

- **BW64/RF64 + Audio Definition Model parser** (`ac3adm::ac3adm`, roadmap item B1, phase 1 of 3)
  — a new standalone module reading the professional delivery format Netflix's and Apple's own
  Atmos ingest pipelines require. Same shape as `matroska::matroska`/`mp4::mp4`/`mpegts::mpegts`
  (no dependency on `ac3::forge`, codec-blind) except in the opposite direction — a reader, not a
  writer. Parses the container (Recommendation ITU-R BS.2088-1: `<ds64>`/`<fmt >`/`<data>`/
  `<chna>`/`<axml>`, RF64/BW64 64-bit chunk sizing and plain sub-4GB `RIFF` alike) and the ADM
  object graph inside `<axml>` (Recommendation ITU-R BS.2076-2: `audioProgramme`/`audioContent`/
  `audioObject`/`audioPackFormat`/`audioChannelFormat`/`audioBlockFormat`/`audioStreamFormat`/
  `audioTrackFormat`/`audioTrackUID`, including per-block position/gain/width/height/depth,
  `channelLock`, `jumpPosition` and HOA order/degree/normalization). Built on the EBU's own
  reference implementations, vendored via CMake `FetchContent` — `libbw64` (header-only) for the
  container layer and `libadm` for ADM XML parsing/validation — rather than a hand-rolled parser;
  neither library's own types cross this module's public API (its own namespace is `ac3adm`, not
  `adm`, specifically to avoid colliding with libadm's). `ac3adm::parse_bw64` returns the parsed
  graph, the `<chna>` track↔ID join table and the decoded PCM together as one `AdmDocument`.
  Unlike every other module in this project, this one is **opt-in**: `AC3FORGE_BUILD_ADM`
  defaults off, since libadm needs several Boost header libraries resolved through a new,
  also opt-in `adm` vcpkg feature (`-DVCPKG_MANIFEST_FEATURES=adm`) — the only third-party
  dependency anywhere in this codebase. Every other target (`ac3cli`, `ac3gui`, tests, every
  other example) builds identically whether it's on or off, and it is not wired into the
  Android/NDK build or the installed `find_package(ac3forge)` package. Mapping this graph onto
  `ac3::oba::AtmosEncoder` (phase 2) and a worked ADM→E-AC-3 pipeline example (phase 3) are not
  part of this change — see [docs/library/adm.md](docs/library/adm.md) and
  `examples/read_adm.cpp`.
- **ADM → Atmos bridge** (`ac3::admbridge`, roadmap item B1, phase 2 of 3) — a new module
  (`src/adm_bridge/`) mapping the object graph `ac3adm::ac3adm` parses onto
  `ac3::oba::AtmosEncoder`'s input shape: one `ac3::oba::ObjectPath` plus one mono PCM span per
  bed speaker feed or dynamic object. Classifies each `audioObject` as a DirectSpeakers bed or a
  dynamic object via its resolved `audioPackFormat`'s `TypeDefinition` (Matrix/HOA/Binaural/User
  Custom/mismatched packs are rejected with a clear `BridgeError` rather than mishandled); builds
  each channel's motion/gain timeline from its `audioBlockFormat` sequence per BS.2076-2 §10.3's
  `jumpPosition`/`interpolationLength` hold-vs-glide state machine (checked directly against the
  standard's own Figs. 7–10 — the two cases run backwards from what a first, name-only reading
  suggests); converts BS.2076-2's polar and Cartesian position conventions (§8) to
  `ac3::oba::Position`'s own room-anchored one, verified against the standard's cardinal-point
  axis directions and empirically against this project's own existing 5.1 ring-position test
  constants; and resolves each channel's audio via `<chna>`. An LFE bed channel (Table 12's
  `LFE`/`LFE1`/`LFE2` speakerLabel) routes via `lfe_send` rather than panning, the same convention
  `ac3cli`'s and the GUI's own object-mode encoders already use. Depends on both `ac3adm::ac3adm`
  and `ac3::forge` — the one module allowed to bridge them, since neither side may depend on the
  other (`ac3adm` stays codec-blind by design; `ac3::forge` is always built and cannot gain a
  Boost dependency) — gated by the same `AC3FORGE_BUILD_ADM` flag rather than a new option of its
  own. Tested against a real byte-level BW64 fixture parsed by the real `ac3adm::parse_bw64()` and
  driven through a real `AtmosEncoder`/`Eac3Decoder` round trip, confirming the decoded bitstream's
  channel energy lands where the authored ADM positions and hold/jump timing say it should — not
  just against hand-built graphs. Not part of the installed `find_package(ac3forge)` package, for
  the same reason `ac3adm::ac3adm` itself is not.
- **`ac3cli atmos-adm`, roadmap item B1 phase 3 of 3 — the last piece.** A real ADM BWF master
  straight to a DD+ JOC E-AC-3 elementary stream: `ac3adm::parse_bw64` reads the container and ADM
  graph, `ac3::admbridge::build` maps it onto `AtmosEncoder`'s object-list input shape, and a
  per-frame loop drives `ac3::oba::evaluate_placements`/`AtmosEncoder::encode_frame` the same way
  `atmos-path`/`atmos-encode` already do — no WAV, no hand-authored keyframe file, since the master
  already carries every channel's own position/gain automation. Every `AdmError`/`BridgeError`
  prints a real diagnosis via that error's own `describe()`. `dialnorm=auto` is refused with a
  clear message rather than silently ignored — an ADM document's bed/object channels have no single
  fixed layout to measure loudness against the way `atmos-encode`'s WAV input does. A minimal,
  standalone illustration of the same pipeline ships as
  [`examples/encode_adm.cpp`](examples/encode_adm.cpp), writing its own small BW64/ADM fixture
  (bed speaker feed plus one moving object) rather than requiring a real production master. Both
  are the first code outside `tests/` to link `ac3adm::ac3adm`/`ac3::admbridge` — `src/cli/
  CMakeLists.txt` links them only when the same flag turned the two libraries on; `ac3cli` itself
  still builds and works identically with the flag off, just without this one command. Reached with
  no preprocessor conditional anywhere (`scripts/check-platform-macros.ps1`, CI-enforced, forbids
  any `#if`/`#ifdef` under `src/` — feature flags included, not just platform ones): `atmos-adm` is
  always one row of `main.cpp`'s command table, refused at dispatch time by a new `Needs::kAdm`
  case in the existing `unmet()` capability gate (the same mechanism
  `Needs::kCapture`/`kPassthrough`/`kMonitor` already use for platform audio capability) when
  `ac3cli::adm_capability()` reports unavailable — backed by a small pair of CMake-selected files,
  `src/cli/adm/{enabled,disabled}/atmos_adm.cpp`, the identical "exactly one file compiled in,
  chosen by CMake" shape this file's own `platform/{windows,posix}/stdio_binary.cpp` split already
  uses for an OS difference, applied here to a library-linked-or-not one instead — see
  `src/cli/adm/atmos_adm.hpp`'s own top comment for the full reasoning. Tested against a real
  byte-level BW64 fixture run through the actual built `ac3cli` binary as a subprocess, decoding
  what it wrote and confirming the channel energy lands where the authored ADM positions and
  hold/jump timing say it should, plus two CLI-level error-path cases. See
  [docs/library/adm-bridge.md](docs/library/adm-bridge.md) and
  [docs/cli/commands.md](docs/cli/commands.md).

### Codec depth

- **Decoder dither substitution** (roadmap D2) — both decoders now implement A/52 §7.3.4: a
  zero-bap mantissa reconstructs as a dither sample when its channel's `dithflag` is set, and as
  a true zero when it is clear, instead of always silence regardless of the flag. AC-3 reads
  `dithflag[ch]` fresh every block (§5.4.3.2); E-AC-3's Annex E equivalent is frame-gated
  (`dithflage`) and, per Table E1.4, defaults every channel's `dithflag` to **on** for the whole
  frame when that gate is clear — the opposite of "absent means off" most of Annex E's other
  optional syntax uses, so this needed its own default rather than reusing the general pattern. A
  coupled channel's shared high band is dithered per §7.3.4's own "applied after the individual
  channels are extracted from the coupling channel ... uncorrelated" requirement: each receiving
  channel draws its own independent sample, gated by its own `dithflag`, rather than dithering the
  shared coupling-channel coefficient once and letting every coupled channel inherit a scaled copy
  of the same noise. The generator (`ac3::DitherGenerator`) is a deterministic xorshift32 mapped
  onto the spec's own "uniform distribution ... scale this by 0.707" — the same class of
  unspecified-generator freedom this codebase's spectral-extension and enhanced-coupling noise
  already use. This project's own encoders still always transmit `dithflag = 0` (true silence),
  so no existing encoder output changes; the new behavior is reachable today by any other
  encoder's stream, or a hand-built one.

### Loudness and QC

- **Full R128 metering in `ac3::meta::LoudnessMeter`** (roadmap C1): momentary (400 ms) and
  short-term (3 s) loudness — BS.1770-4 §2's own un-gated block power, read directly instead of
  gated the way `integrated_lkfs()` already was; Loudness Range (`loudness_range()`) — EBU Tech
  3342 §3.1's cascaded gate (-70 LUFS absolute, then -20 LU relative to what survives it, a
  *different* relative threshold to integrated loudness's own -10 LU) over the short-term series,
  reported as the 95th minus 10th percentile of what is left; and true peak (`true_peak_dbtp()`)
  — ITU-R BS.1770-4 Annex 2's 4x-oversampled peak estimator, built from the Annex's own tabulated
  order-48/4-phase FIR rather than the project's general-purpose offline `dsp::resampler` (that
  one allocates and designs its own kernel; this is a fixed, tiny, allocation-free per-sample
  filter, and Annex 2 gives no formula to redesign it from in the first place). True peak is the
  one measure of the four that does not exclude the LFE channel or apply the surround
  weighting — it is about physical overload headroom, not perceived loudness. All four share the
  existing K-weighting/channel-summing machinery rather than duplicating it.
- **`ac3cli qc` (roadmap C2), bitstream-aware loudness QC** — decodes a whole AC-3/E-AC-3 stream,
  measures it with the real BS.1770-4/EBU Tech 3342 meter (the same `ac3::meta::LoudnessMeter`
  `dialnorm=auto` already uses), and reports it against the stream's own embedded `dialnorm`/
  `compr` — for E-AC-3, against the independent substream's own bed audio only, never a
  dependent's, matching the scope the encoder's own pre-encode `measured_dialnorm` pass already
  uses for the same programme. A new `ac3::meta::qc.hpp` names three delivery gates
  (`preset=ebu-r128-s2 | atsc-a85 | netflix`, or `preset=all` for every one), each preset's
  target loudness, tolerance and true-peak ceiling cited from its own primary source rather than
  recalled from memory: EBU R 128 s2 "Loudness in Streaming" (Nov 2023 v3) plus EBU R 128 (Nov
  2023 v5) for the tolerance/ceiling it defers to, ATSC A/85:2013 §6, and Netflix's Sound Mix
  Specifications & Best Practices v1.6. Exit code is 0 only when the stream decodes cleanly and
  (if a preset was given) every requested gate passes — the whole point being a usable CI/pipeline
  QC step. Along the way, the E-AC-3 decoder gained a genuinely missing piece its own bitstream
  parsing already walked past: the independent/convertible substream's own `compr` word was
  parsed (to keep the bit count right) but discarded rather than exposed on `DecodedSubstream`/
  `DecodedAccessUnit` — `qc` needed it, so it is now reported like AC-3's decoder already reports
  its own `compr`.
- **`ac3gui`'s own QC surface (roadmap C3), completing Theme C** — a **QC a stream…** button in
  the header opens a new dialog that runs the exact measurement `ac3cli qc` does (decode, BS.1770-4
  measure, compare against the stream's own embedded `dialnorm`/`compr`) against a chosen
  `.ac3`/`.ec3` file, off the GUI thread the same way every encode already is. It is a standalone
  dialog rather than a sixth tab beside Format/Coding tools/Metadata/Objects/Live session
  deliberately — QC opens and verifies a file that already exists, with no source, plan or encoder
  involved anywhere in the path, so it has nothing in common with the five tabs beside it, which
  are all pages of controls for a plan still to come; see docs/gui/qc.md for the full reasoning.
  The report shows integrated loudness, loudness range and true peak as filled meters (the same
  track/fill shape the channel meters already use), a `1+1` dual-mono stream reported as its two
  independent programmes, the dialnorm-vs-measured delta `ac3cli qc` already prints, and a
  pass/fail verdict per delivery preset (`EBU R 128 s2`/`ATSC A/85`/`Netflix`, or all three at
  once) — picking a single preset also draws its target tolerance band / true-peak ceiling as
  lines on the meters, so a failing gate shows why. A new `QcController` singleton carries this
  state rather than `EncoderController`, which stays exactly what it already was: encode-workflow
  state only.

### CLI

- **`-` as a file argument means stdin/stdout**, the conventional Unix pipe convention, for
  `encode`/`eac3-encode`/`atmos-encode`'s WAV input and AC-3/E-AC-3 output and for `decode`'s
  stream input and WAV output — e.g. `ac3cli encode - - 448 couple < in.wav > out.ac3` and
  `ac3cli decode - - < out.ac3 > out.wav`. Windows needs no special handling from the caller:
  ac3cli puts stdin/stdout into binary mode itself before the first byte crosses either one (the
  CRT defaults both to text mode, which otherwise corrupts compressed audio). With `-` as the
  output path, the usual per-run status text (frame count, routing, per-channel levels) prints to
  stderr instead of stdout, so it never lands inside the piped stream.
- **`dialnorm=auto`/`dialnorm2=auto` now work with `src=`/`map=` multi-source encodes**
  (`encode`/`eac3-encode`), instead of being unconditionally refused the moment more than one
  source was in play. The whole programme is routed once as a BS.1770-4 measurement pre-pass —
  over what `map=` actually assembles (post-routing, post-trim), not each source's own raw file —
  before the real per-frame encode loop routes it again to encode it, mirroring the two-pass
  measure-then-encode shape the single-file path already used. Along the way, a real bug in that
  single-file path was found and fixed: `dialnorm` (Ch1) under a `1+1` dual-mono bed was silently
  measuring the COMBINED BS.1770 loudness of both channels together (as if Ch1/Ch2 were a coherent
  stereo pair) rather than Ch1's own channel alone, while `dialnorm2` (Ch2) was already correct —
  both now measure their own programme independently, as §E1.3 requires (no downmix between the
  two). The GUI's own encoder controller now does the same — `dialnorm=auto`/`dialnorm2=auto`
  measure each dual-mono programme's own coded channel independently there too, and the
  Metadata tab's Programme 2 **measure** checkbox is enabled accordingly.
- Five small CLI/docs accuracy fixes found during the docs sweep above:
  - **`sign-objects`/`signing-key=` now reach `atmos-path` and `atmos-encode`, not just `atmos`**:
    all three commands parsed the flags (nothing in the trailing-options parser is
    command-specific), but only `atmos`'s own run function called into the signer —
    `atmos-path` and `atmos-encode` silently accepted and ignored both. All three sign
    identically now.
  - **`eac3-sine` now honors `fast-mdct=off`**: unlike `eac3-encode` it has no `[tools]`
    positional to spell it through, and previously never read the option at all, so there was
    no way to force the direct §8.2.3.2 transform for it. `eac3-silence` still has no use for
    either spelling — it builds a silent access unit directly, with no forward transform in the
    loop to choose a path for — so the built-in usage text no longer implies otherwise.
  - **`eac3-encode`/`eac3-encode-multi` now honor `fast-mdct=off` directly**: every other encode
    path (`atmos*`, `record`, `live`, `eac3-sine`, `encode`) applied `fast-mdct=off` before
    resolving the tools set, but these two never did — only their `[tools]` positional's bare
    `nofastmdct` token reached `Tools::fast_mdct`, so `fast-mdct=off` parsed and was silently
    ignored. Both now apply it the same way as everywhere else; the `[tools]` positional's own
    token still wins if both are given on the same command line.
  - **`map=` rows aimed at `obj`/`objm` (or `p1`/`p2` outside a dual-mono target) now warn
    instead of silently vanishing**: this CLI has no object-assembly path of its own (that's
    the GUI's), so `encode`/`eac3-encode` print a warning naming the source/channel and
    destination for every row `route()` cannot carry, rather than that channel's audio just
    disappearing with no diagnostic.
  - Corrected the built-in usage text's wide-layout inference line (`8 -> 7.1`, `10 -> 5.1.4`,
    `12 -> 7.1.4`), which read as if `encode` shared it with `eac3-encode` — `encode` refuses
    anything wider than 3/2 + LFE, so the inference is `eac3-encode`-only.

### AC-3 encoding

- **Delta bit allocation alongside coupling** (roadmap D3): the encoder previously withheld
  delta bit allocation (§7.2.2.6) from every stream the instant coupling was active for the
  frame — not just the coupling channel itself, but every fbw channel too, even the narrow
  band each still codes independently below the coupling frequency. §7.2.2.6 places no such
  restriction ("the delta bit allocation option is available for each fbw channel and the
  coupling channel"), so both are now eligible whenever coupling is on. Doing this blindly
  regressed a standing invariant — "coupling must not cost more bits than the channels it
  replaces" — at 96 kbit/s stereo, the moment delta's own side-info cost and per-band
  precision shifts started eating into the same budget coupling was supposed to free up; the
  encoder now runs its SNR-offset search a second time with delta cleared whenever coupling
  is active and something is queued to send, and keeps whichever pass reaches the higher
  composite offset. LFE was already correct as it stood: A/52 defines no delta bit allocation
  field for the LFE channel at all (§7.2.2.6, §5.4.3.49's own `for (ch = 0; ch < nfchans;
  ch++)` loop bound never reaches it), so nothing changed there.

### Raspberry Pi (arm64 Linux) — new platform

- **Raspberry Pi 4/5 support**: a new `arm64-linux-{gcc,llvm}` vcpkg triplet pair and matching
  `config-linux-{gcc,llvm}-arm64[-debug]` presets, backed by two new required CI legs on real ARM
  hardware (GitHub's `ubuntu-24.04-arm` runner) and validated for real over SSH against a
  Raspberry Pi 4B — full build, 440/440 tests, and the real-time encode gate all pass on both
  compilers, and `cpack` produces a correctly arm64-labeled `.deb`. No platform-tree code changed:
  Raspberry Pi OS hits the exact same `if(LINUX)`/ALSA/HDMI passthrough path any x86_64 Debian
  box does. Pi 3 is explicitly not a supported target (real-time budget risk on its weaker CPU).
- Two real, previously-latent bugs this validation found and fixed along the way: a GCC 14
  `-Wnull-dereference` false positive at `-O2`/`-O3` inside libstdc++'s own headers, and a missing
  `clang-tools` toolchain dependency (`clang-scan-deps`) that broke every `find_package` check
  under Clang's Ninja module scan.

### Packaging

- **A vcpkg port is staged in-tree** at `packaging/vcpkg-port/ac3forge` (portfile, usage text,
  and a `matroska` feature for the optional Matroska writer), pending submission to the curated
  `microsoft/vcpkg` registry. The submission and per-release update flow, and how to validate the
  port locally, are documented in [docs/releasing.md](docs/releasing.md#vcpkg-port).

### Validation

- **A perceptual-quality column alongside SNR** (roadmap G1) on `tools/quality_race.py`'s `ac3`/
  `eac3`/`eac3-51`/`trend` tables and on the [docs/tool-comparison-trend.md](docs/tool-comparison-trend.md)
  and [docs/landscape.md](docs/landscape.md) pages: MOS-LQO (Mean Opinion Score - Listening
  Quality Objective) from [ViSQOL](https://github.com/google/visqol)'s audio mode, chosen over
  PEAQ (ITU-R BS.1387) — PEAQ itself is FRAND-licensed ITU IP, not a dependency this clean-room
  project wants, and the one credible open implementation (GstPEAQ) documents its own
  non-conformance to the BS.1387-1 tolerance and has no Python surface at all. Google's own
  `google/visqol` has no published PyPI wheel (its Python API needs a Bazel + TensorFlow source
  build), so this uses `visqol-python`, a pure-Python Apache-2.0 reimplementation with prebuilt
  wheels that scores within 0.0002 MOS-LQO of the reference C++ implementation on its own
  conformance suite. Optional throughout, the same AUTO shape as `AC3FORGE_WITH_ALSA`
  (`src/audio/CMakeLists.txt`): a missing `visqol-python` install skips the column with one clear
  message rather than failing the run, and CI does not install it either, so the fidelity gate
  (`quality_race.py ci`) gains no new dependency or runtime cost — only the reporting tables and
  `trend`'s JSON output request a score at all.
- **Test coverage backfill for the WAV reader and the §7.8 downmix** (roadmap G2).
  `ac3::io::read_wav` (`tests/test_wav.cpp`, new) had no dedicated test at all despite being the
  fixture-loading path every codec test in the suite depends on: its RIFF/WAVE validation, PCM16
  decode scaling, `WAVE_FORMAT_EXTENSIBLE` handling, and its documented clamp-not-error behaviour
  when a data chunk's declared size overruns the bytes actually present were all unexercised.
  `ac3::meta::mixing` (`tests/test_mixing.cpp`, new) — `stereo_downmix`/`mono_downmix`'s per-acmod
  routing, `mono_downmix_peak_dbfs`'s phase behaviour, and the `coefficient()`/
  `valid_surround_mix_level()`/`lfe_mix_level_db()` tables — had only the aggregate `<=1`
  normalization-bound check `test_drc.cpp` already carried; the mix-level tables and several
  per-acmod branches (discrete-vs-spread surround routing, 1+1 dual mono) had none.
  `tests/test_cli.cpp` gained coverage for the `silence`/`eac3-silence` commands' own argv wiring
  (seconds/bitrate/layout threading, default application, invalid-bitrate rejection) — the
  frame-building code underneath was already covered unit-level, but the CLI dispatch row itself,
  the thing the command table's own comment credits with catching six real argv-index bugs during
  its refactor, was not. Roadmap item G2's original "seven tests / four thousand lines" CLI
  estimate was already stale by the time work started (the CLI has grown past 5,000 lines and 30
  test cases since); `meta/loudness` and `silent_frame`, the roadmap's other two named gaps,
  turned out to already have solid dedicated coverage (`tests/test_loudness.cpp`,
  `tests/test_frame.cpp`) and were left alone.

### Developer tooling

- **Differential decoder fuzzing against FFmpeg** (roadmap G3): two new libFuzzer harnesses,
  `fuzz_differential_ac3_decode`/`fuzz_differential_eac3_decode`, that drive the same
  `split_frames`/`split_access_units` mutation surface `fuzz_ac3_decode`/`fuzz_eac3_decode`
  already crash-fuzz, but additionally decode each mutated stream a second time with FFmpeg
  (the same `-xerror -err_detect crccheck+bitstream+buffer+explode` strict-decode invocation
  already used everywhere else this project treats FFmpeg as an oracle) and diff the PCM. A
  mismatch is only reported when BOTH decoders accept the whole input and produce comparably-
  shaped audio — FFmpeg's own error-concealment on a malformed frame legitimately differs from
  this project's spec-strict decode, so declining a mutated frame is never itself a divergence
  (`fuzz/differential_oracle.hpp` has the full reasoning). The agreement floor (6 dB) was
  calibrated empirically, not guessed: a sweep of every currently-committed seed found real,
  unmutated, already-shipping content — a panning source, and pure single-tone material — that
  legitimately disagrees with FFmpeg by as much as 12–24 dB, traced to A/52 leaving bap-0
  (unallocated) coefficient reconstruction implementation-defined ("any reasonably random
  sequence"): this decoder always reconstructs zero there for deterministic parity, while
  FFmpeg synthesizes real dither noise. Verified against a real, deliberately-introduced decode
  bug (a dropped overlap-add term) before considering it done. New `fuzz-differential` CI job
  in `.github/workflows/fuzz.yml`, its own 60-second-per-harness push-only budget and
  `continue-on-error: true` (same experimental convention as `fuzz-short`/`fuzz-nightly`) — not
  folded into either existing job, since it needs `ffmpeg` on PATH and is much slower per-exec
  (a real FFmpeg process per comparable input). The two harnesses share their crash-only
  siblings' seed corpora rather than duplicating them.
- **A public [ROADMAP.md](ROADMAP.md)**: the candidate list of possible future work, each item
  carrying a stable ID (`A1`–`G3`) that pull requests and this changelog can reference; mirrored
  on the docs site.
- **Static-analysis hardening, continuing 0.3.0-beta.1's round**: the remaining MSVC `/analyze`
  C6262 stack-usage findings fixed (large codec state heap-allocated instead of declared on
  worker stacks), plus a follow-up correcting the first pass's own regressions.

### macOS — real CoreAudio audio backend (roadmap E1)

- **`src/audio/src/platform/macos/`**: live capture, IEC 61937 bitstream passthrough and
  shared-mode monitor playback are now real on macOS, replacing the no-backend stub `if(APPLE)`
  fell back to. Built on the Audio HAL (`AudioObjectID`/`AudioDeviceIOProc`) rather than
  `AVAudioEngine`, the same layer WASAPI and ALSA occupy on their own platforms, for the same
  reason ALSA rather than PulseAudio/PipeWire is the Linux backend: passthrough's exclusive-mode
  format switch has to happen at that layer, so device enumeration, hog mode and format
  negotiation live in one place (`coreaudio_support.hpp`) all three capabilities share. Capture
  reads any HAL input endpoint; monitor plays ordinary float PCM back through a HAL output
  endpoint, retuning the device's nominal sample rate when it doesn't already match (no engine-
  level resampler exists at this layer the way WASAPI shared mode has one); passthrough takes hog
  mode on a digital (HDMI/optical) output and retunes its *physical* stream format to
  `kAudioFormat60958AC3` (AC-3) or `kAudioFormatEnhancedAC3` (E-AC-3, probed the same way though
  the constant has no comparable IEC 60958-wrapped documentation history — see the file's own
  header) before feeding it raw bursts, confirmed against three independent real-world
  implementations of the same mechanism (MythTV, mpv, VLC) while researching it. No loopback
  capture: unlike WASAPI's any-render-endpoint-in-loopback-mode, the HAL has no equivalent short
  of Apple's Objective-C-only, permission-gated Core Audio Process Tap API (macOS 14.4+), so this
  is the same honest gap ALSA already documents for a machine without `snd-aloop`. Linked via
  `-framework CoreAudio -framework CoreFoundation`; `AC3FORGE_AUDIO_BACKEND` reports `macos`.
  Verified via the `macos-llvm` CI leg only — no Mac is available to this project locally.

## [0.5.0-beta.1] - 2026-08-15

Fourth tagged release. The main change is a fast-transform performance initiative: an opt-in
FFT-based MDCT was introduced, taken default-on, and then progressively hardware-optimized down
through every transform kernel the encoder touches — the long transform, both block-switched
short transforms, and the opt-in enhanced-coupling tool's DFT — alongside an algorithmic
warm-start for the bit-allocation rate-control search. Measured on the same 5950X release build
throughout, default 5.1 encoding drops from 0.4.0-beta.1's ~3.0 ms/frame to ~0.47 ms/frame (about
6.4×) and 8-object Atmos from ~4.8 ms/frame to ~0.43 ms/frame (about 11×), with SNR held at
+0.000 dB against an independent FFmpeg oracle at every step along the way. Alongside the
performance work: two GUI fixes (object-drag losing its mouse grab mid-gesture, and ambiguous
plan/elevation axis labeling), a quality-trend dashboard fix, and Linux packaging now ships real
`libFOO`/`libFOO-dev`-style system packages instead of one `.deb`/`.rpm` silently bundling the
CLI together with the entire library SDK.

### Performance: fast transforms, default-on and hardware-optimized

- **A new FFT-based fast forward MDCT**, landed opt-in behind `fast_mdct` (off by default): the
  §7.9.4 N/4-FFT structure replaces the direct §8.2.3.2 O(N²) evaluation for the long transform,
  ~25× faster at the kernel level (76.8 µs → 3.1 µs/call) with the direct form kept in-tree as
  the permanent reference/validation oracle. Verified bit-identical-class agreement (peak-relative
  ~3e-15) against the direct form on goldens, random data and real audio, plus **+0.000 dB**
  through an independent FFmpeg oracle at 192–448 kbps.
- **The inverse transform and enhanced coupling's windowing step got the equivalent fix**: `std::cos`/
  `std::sin` calls inside `imdct512_windowed`, `imdct256_pair_windowed` and `ecpl_channel_spectrum`'s
  windowing loop, previously recomputed fresh every call, are now one-time tables. Bit-exact by
  construction (the naive periodic-index shortcut is provably *not* bit-exact for the IMDCT's
  un-reduced angles — documented as a trap so it isn't re-attempted). A real 5.1 E-AC-3 decode
  drops from ~640 ms to ~145 ms (~4.4×).
- **The fast MDCT is now the default everywhere**, with `band_energy` (Atmos's JOC reconstruction
  solve) wired through the same flag — the gap that had capped Atmos's win at ~2.0×. Whole-frame:
  plain 5.1 3.0 → 0.67 ms/frame (~4.5×), 8-object Atmos 4.8 → 0.64 ms/frame (**~7.6×**, up from
  ~2.0× before `band_energy` rode the flag). `fast-mdct=off` (AC-3 commands) / `tools=nofastmdct`
  (E-AC-3) force the direct form back; the old opt-in spellings still parse as no-ops so existing
  run history keeps working.
- **The fast MDCT kernel itself closed to its standalone-prototype speed** (3.09 µs → 903 ns/call,
  a further 3.4×) by moving every angle-dependent value in the §7.9.4 fold — pre/post twiddles and
  the FFT's own butterfly twiddles/bit-reversal — into one-time tables, and switching the FFT to
  split real/imaginary arrays so the auto-vectorizer can see the butterfly's independent
  multiply-add chains.
- **Both block-switched short transforms get their own fast folds**, closing the last kernels still
  running direct-form O(N²) sums under the default `fast_mdct`. Each derives to the same scaled
  DCT-IV core the long transform already uses (877 ns/call vs. 35.8 µs direct — ~41×), removing the
  worst-case real-time hazard on transient-heavy material: a fully block-switched 5.1 frame's
  transform stage drops from ~1.3 ms-class to ~32 µs-class.
- **The opt-in enhanced-coupling tool's `dft512` gets the same FFT treatment** as the long MDCT
  (both now share one `fft_radix2.hpp` core): `ecpl_channel_spectrum`, still the single most
  expensive kernel measured, drops from 277 µs to 47 µs/call (~5.9×). Not run by any default
  encode, but a real-time hazard whenever `ecpl` is enabled.
- **The bit-allocation rate-control search now warm-starts from the previous frame's converged
  offset** instead of a fixed bracket, exploiting that consecutive frames of real material converge
  to the same or a neighbouring value. A stationary frame's ~11 full bit-allocation evaluations
  drop to 2–3; whole-frame time falls a further 18% (5.1) / 11% (Atmos) on top of the kernel work
  above. Brute-force verified against the plain binary search over 4,355 monotone-predicate cases
  with zero mismatches; outputs are byte-identical on every monotone path, and the one path where
  they can legitimately differ (AHT's locally non-monotone cost function) was already
  probe-order-dependent before this change — decoded PCM agrees at 102–115 dB SNR per channel.
- **New performance observability**: Tracy zones across every previously
  unzoned encoder stage, a standalone `ac3kernelbench` micro-benchmark harness timing kernels in
  isolation against real audio, and a per-kernel trend history (non-gating, `::warning::`-only)
  alongside the existing whole-frame performance trend — see
  [docs/performance-trend.md](docs/performance-trend.md).

### Packaging

- **Linux `.deb`/`.rpm` now ship a real `libFOO`/`libFOO-dev` split** instead of one package
  silently bundling `ac3cli` together with the entire library SDK (headers, static archives, the
  CMake package config — confirmed against real `dpkg-deb -c` output, not assumed). `libac3forge0`
  carries just the versioned shared library a linked binary loads at runtime; `libac3forge-dev`/
  `ac3forge-devel` carries everything a builder needs, version-pinned to its exact matching
  `libac3forge0`. Installable with a plain `apt install`/`dnf install` rather than a manual archive
  download — see [docs/releasing.md](docs/releasing.md#what-gets-published). ZIP/TGZ downloads are
  unaffected: `library`+`libruntime` still merge into one `ac3forge-dev-*` archive, exactly as
  before.

### GUI fixes

- **Object-drag no longer loses the mouse grab mid-gesture.** The Objects tab's plan/elevation/
  live-session `MouseArea`s sit inside a `Flickable`-based `ScrollView`, which could steal the grab
  from a child `MouseArea` once movement looked flick-like — most reproducible on the elevation
  view's vertical drag, the same axis `Flickable` watches for scrolling. `preventStealing: true`
  on all five affected `MouseArea`s holds the grab for the whole gesture.
- **The plan and elevation views in the Objects tab are now labeled as what they are** — "(top-down)"
  / "(side-on)" headers, a one-line caption naming which screen axis maps to which room axis, and a
  corrected elevation hint ("drag: depth + height" rather than "drag for height", since the plan
  view's marker moves too during an elevation drag — correct behaviour, previously unexplained).

### Developer tooling

- **The quality-trend dashboard's table no longer conflates unrelated checks.** The chart already
  scoped rows by codec and `isPrimaryCheck`; the table below it rendered the raw, unfiltered record
  list, which let a steady ~25 dB interop fixture read as a crash relative to an unrelated ~68 dB
  series. The table now follows the same Codec scoping as the chart, with a `Check` column and a
  tooltipped `†` marker on non-primary checks.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.

## [0.4.0-beta.1] - 2026-08-14

Third tagged release. The GUI is rebuilt to the canon design handoff — a numbered-rail workflow,
a single assignment table driving all channel routing, an audible timeline with per-source
offsets and motion editing, live capture (including two-device parallel capture with software
clock-drift correction), per-source gain/LFE/resample controls, dual-mono independent DRC,
S/PDIF-wrapped WAV output, four selectable colour palettes with a native system-accent theme, and
a full round of dark-mode fixes. Alongside the GUI work: enhanced coupling's encoder now fits
real angle/chaos coordinates instead of sending them as zero, the decoder accepts Annex E's
default coupling band structure, the EMDF object signer is a committed clean-room library, eight
new library examples ship, an external-encoder (FFmpeg/DEE) comparison joins the quality
dashboards, and Android release builds sign with a real keystore.

### E-AC-3 encoding and decoding

- **Enhanced coupling's encoder now fits real angle and chaos coordinates**, closing the last
  known gap from 0.3.0-beta.1's enhanced coupling work — it no longer sends angle/chaos as zero.
  Amplitude and angle are solved as an exact 2-variable linear least squares per band (§3.5.5.4's
  reconstruction is linear in the complex gain a band's amplitude/angle pair expresses); chaos is
  chosen by searching its 8 legal codes directly against the decoder's own deterministic
  de-correlation sequence and keeping whichever reconstructs closest to the source, rather than
  estimated from a statistical proxy. Quality on ordinary material is unchanged (a correlated
  signal's best fit lands near angle zero anyway); the case the amplitude-only fit could not
  represent at all — two channels' different content forced into the same narrow coupling band —
  improves measurably, from a ~3 dB floor to ~6 dB, without threatening the coding tool's own
  structural limit on how much a single coordinate per band can ever separate.
- **E-AC-3 stereo (2/0) rematrixing** — the bitstream syntax and decoder undo path have existed
  since 0.2.0-beta.1; only the encoder's own §7.5.3 minimum-power decision was missing, and it
  turned out to need no new logic at all, just the same rule AC-3's own encoder already makes,
  over the same Table 7.25 bands (Annex E only changes how many of the four are active, not their
  boundaries or the rule itself).
- **The decoder now accepts Annex E's legal default coupling band structure (`cplbndstrce=0`)**
  instead of rejecting it with `DecodeError::kUnsupported`. This project's own encoder always
  transmits an explicit band structure, so the default path had only ever been exercised against
  the encoder's own output — decoding FFmpeg's E-AC-3, which legally chooses the default, failed
  immediately. The root cause was a stale assumption that Table E2.12's array needed
  relative-to-`cplbegf` indexing; cross-checked against FFmpeg's own `decode_band_structure()`,
  the table is indexed absolutely from `cplbegf == 0`. A permanent regression fixture (a real
  FFmpeg 8.0.1 encode with nonzero `cplbegf`) now covers this in the gold-reference gate.

### Atmos object signing

- **The EMDF object signer is now a committed, clean-room library (`ac3::signing`)** rather than a
  gitignored overlay. The HMAC-SHA-256 construction and the layout of what gets signed are in-tree
  and dependency-free; the **key** is the only secret and is supplied by the operator at runtime,
  never embedded and never written to disk. `ac3cli atmos` gains `sign-objects` with
  `signing-key=<path>` (or the `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` env vars);
  signing engages only when both a request and a key are present. The Shield app reads its key from
  a bundled `signing.key` asset written from the `ATMOS_SIGNING_KEY` CI secret at build
  time. See [docs/concepts/object-signing.md](docs/concepts/object-signing.md).

### GUI: canon workbench redesign

- **The desktop GUI is rebuilt to the canon design handoff**, replacing the earlier workbench that
  had drifted from it — the numbered rail (01 Input / 02 Levels / 03 Soundfield), plan strip,
  two-tier bitrate picker, routing strip and command bar now match the handoff, landed via a
  6-agent conformance sweep against the mockup (~70 fixes across CLI parity, run history, timeline
  editing, live-tab truth and guided copy).
- **A single assignment table now drives channel routing everywhere**, replacing the free-text
  token field that only appeared once a second source was loaded. Each source channel gets one
  destination dropdown (bed position / a new object / programme / nothing); sending a channel to
  an object turns object mode on, fixes the 5.1 bed, and raises the rate to ≥384 kbps atomically.
  In object mode, a channel assigned to a bed position becomes a static object pinned at that
  speaker's azimuth; unassigned channels drop with a named warning, and encoding enforces the
  sixteen-object cap over dynamic + pinned together.
- **Meter and soundfield redraws no longer tear down and rebuild ~30x/second.** The 30 Hz level
  stream previously rebuilt fresh JS arrays (and every delegate) on every tick; meter/soundfield
  models are now layout-keyed and read by index, and encode-progress/object-drag updates are
  coalesced onto the ~30–60 Hz publish cadence instead of flooding the GUI event queue per frame.
- **A real first-run screen, Preferences dialog and honest run history** round out the shell: first
  run synthesizes a bundled 5.1 test signal into a real WAV; Preferences persists via `QSettings`;
  and run history, failure-banner actions, and the live tab now reflect actual encoder/session
  state rather than mockup placeholders.

### GUI: timeline & time model

- **Timeline length is now derived, not fixed** — `max(offset + duration)` over every loaded
  source, rather than a hardcoded 8 s.
- **Each source gets an independent start offset**, settable from a rail numeric field or by
  dragging its clip band, applied as leading silence in both the channel and object encode loops
  and the meter preview — and reproducible on the command line via a new `offset=` CLI token.
- **Keyframes stay programme-absolute when a clip is dragged**; Shift-drag explicitly carries a
  source's object keyframes along by the same delta (clamped at 0), so a plain drag no longer
  silently drags authored motion with it.
- **Zoom (wheel/button, up to 40x) and snap** — ruler-tick and drag-snap tiers at 1 s / 0.1 s / a
  32 ms floor — move together as the view scales.
- **The Preview button is now audible**: it renders every object through the Atmos encoder and
  plays the 5.1 bed back live through the monitor sink, paced in real time with the playhead
  following the audio clock.
- **Object identity is now keyed by (source, channel)** instead of position in the dynamic-object
  list, so reassigning a channel or removing a non-primary source no longer silently migrates or
  destroys motion belonging to a different or surviving channel.
- **`atmos-encode` gains an optional keyframes-file argument**, matching `atmos-path`'s grammar;
  the GUI's "Export paths…" writes that exact format, closing the last gap in object-mode CLI
  reproducibility.

### GUI: live session and two-device capture

- **A live take now streams to disk incrementally** instead of buffering the whole session in RAM:
  an elementary-stream take *is* the growing output file, muxed to Matroska once at a clean stop,
  so a crash still leaves the elementary take behind. An optional raw-WAV safety copy streams the
  untouched captured PCM alongside it.
- **A silence watchdog fails a session ~3 s after a capture device goes quiet**, instead of the
  transport reading "Running" forever against a vanished device, with a "Choose another device"
  recovery action on the resulting failure banner.
- **Live Atmos sessions pre-allocate a fixed object-slot budget** rather than baking the capture
  device's channel count straight into the JOC stream, so objects can be added or reassigned to a
  different capture channel mid-session.
- **Changing the receiver — or toggling passthrough — mid-session now hot-swaps the passthrough
  sink** on the worker thread between frames, without restarting capture or encode.
- **A live session can now pace a second capture device off the first's clock in software.** The
  master device's delivery paces the frame loop as before; the second device is conformed to the
  master's clock via a streaming linear-interpolation fractional resampler and a proportional
  drift-correction servo, since there's no shared hardware clock between two independent capture
  endpoints. Available from the GUI and from `ac3cli`'s new `live capture2=<index>` token, with
  the slave device's measured drift correction visible in the chain's capture cell. A plain
  channel-mode session's bed still comes from the master device alone — there is no principled
  default position to auto-pan a second, independent device's audio into.

### GUI: source gain, metering, and format/output controls

- **Per-assignment gain/trim** on the channel routing table, applied inside the same routing
  matrix that drives encode, meter preview, and fed-channel flags.
- **Source-side metering pips**: a whole-programme, pre-routing peak/RMS reading per loaded file
  source.
- **Resample-on-load**: adding a source at a different sample rate than the primary no longer
  refuses outright — it resamples to the primary's rate via an offline windowed-sinc polyphase
  resampler and labels the row accordingly; the refusal survives only when the primary's own rate
  has no legal AC-3 target at all.
- **LFE low-pass filtering**: a full-bandwidth channel explicitly routed onto LFE through the
  assignment table now runs through a 120 Hz 4th-order Butterworth low-pass in preview and
  channel-encode. Automatic single-source routing (a file's own dedicated LFE channel) stays
  bit-exact.
- **CLIP latches per channel** in the meters — once lit, stays lit until clicked or a new
  transport starts.
- **`objm` fold-to-mono**: the range grammar (`0.1-2:objm`) can now fold a contiguous run of one
  source's channels into a single dynamic object.
- **Dual-mono programmes get independent DRC.** A/52 §7.7.1/§7.7.2.2 give 1+1's two programmes
  independent DRC curves and heavy-compression ceilings, but the encoder was building the second
  programme's controller from the first's own config. CLI gains `drc2=`/`heavy2`/`ceiling2=`/
  `dialogue2=`; GUI gains a Programme 2 DRC combo and a "Heavy compression — programme 2" card.
- **A third container option: S/PDIF-wrapped WAV**, reusing the existing IEC 61937 burst-wrapping
  machinery. Works for both codecs — E-AC-3's carrier runs at 4x rate.
- **An advisory bit-rate floor for wide layouts**: a muted hint under Bit rate when the CBR rate
  works out to fewer than ~77 kbps per full-bandwidth coded channel. A hint, not a gate.
- **Guided now applies measured loudness and film-standard DRC automatically** while it's driving
  and Loudness/Metadata is untouched this session; dual mono gets the DRC-only half of the
  contract on both programmes, since loudness measurement is refused there.

### GUI: guided-mode workflow polish

- **Finished run chips now carry their own Play action**, sending that run's own output to a
  receiver — not whatever the most recent encode happened to produce.
- **Run history now survives a restart.** The last 30 completed runs persist to Settings as JSON;
  clicking a run chip opens a details popover with status, rate, duration, size, frames, failure
  text, and the `ac3cli` command line snapshotted when that run started.
- **Guided's amp destination now auto-picks a bitstream-capable output device** — the first device
  that can carry the prospective encode plan — with a "Choose a different device" override and a
  stated reason when nothing qualifies.
- **Guided's Movement step, once object mode is on, offers two cards**: *Everything moves* (every
  loaded channel becomes an object) and *Keep the bed, add movers* (only claims still-unassigned
  channels).
- **Good/Better/Best now maps to VBR quality, not a fixed bitrate**, when a VBR default or an
  already-selected Variable rate mode applies — Guided's Quality step rate cards set a VBR quality
  target (40/75/90) instead of a CBR number.
- **Preferences defaults apply on Save to untouched fields only**, generalising the existing
  loudness-touched contract to container/rate mode/bit rate/VBR quality.
- **The guided wizard's Back/Next footer no longer disappears off-screen.** It previously shared
  the tab `StackLayout`, whose implicit height is the max over every page — inheriting the Format
  tab's height let the footer stretch a full screen below the visible content. The wizard now owns
  its own surface outside the tab stack: the step bar and footer stay pinned, only the step content
  scrolls between them.
- **The always-on `ac3cli` command bar is now a popover.** Encode runs the encoder in-process, so
  the full command line is reference material, not the primary act: a compact chip opens a popover
  with the complete line, wrapped, with Copy.
- Fixed the runs lane's empty-state text riding the top edge instead of centring in the strip.

### CLI

- **Fixed: a bare `heavy2` token was silently misparsed** as `encode`/`eac3-encode`'s optional
  `in2.wav` positional instead of enabling Ch2 heavy compression — `run_main`'s bare-token
  classifier was missing it alongside `couple`/`heavy`/`mixmeta`/`sign-objects`/`keep-partial`.
- **`keep-partial` token**: a bare trailing-options token that keeps whatever frames
  `encode`/`eac3-encode`/`atmos-encode` already produced before a failure, at
  `<name>.partial.<ext>` — mirrors the GUI's own keep-partial-output preference.

### GUI: theming

- **Four selectable colour palettes, including a native system-accent theme.** *Signal* (the
  design system's red, default), *Ink* (cool greys, cobalt accent), and *Console* (warm greys,
  studio amber) join *System* — a new `SystemTheme` singleton that reads the platform's native
  accent colour and re-announces on OS colour-scheme changes, so changing the OS accent colour
  restyles the running app live. All four are selectable in Preferences → Appearance.
- **Dark mode is now hand-tuned per palette instead of a mechanical inversion of the light ramp.**
  The previous approach turned near-white accent tints into murky red-blacks and left the
  fully-saturated accent glaring against near-black; each palette now defines both modes by hand.

### GUI: dark-mode audit fixes

- **A round of dark-mode fixes found by auditing every tab across all four palettes.** Smoke-mode
  screenshot captures are now hermetic — session restore previously ran at window creation, so a
  screenshot inherited whatever session the last run saved, and closing the smoke binary could
  clobber the user's real saved session with smoke state. The Coding tools tab now explains itself
  instead of rendering a bare void when object mode or plain AC-3 hides its contents. The runs
  lane's hard-capped height had exposed a horizontal scrollbar overlaying the chips and eating
  their clicks — the scrollbar is now off, wheel/drag still pan. The Encode button's `.ac3`/`.ec3`
  suffix no longer goes stale after the codec moves the plan between containers.

### Quality & verification tooling

- **Added an external-encoder landscape comparison against FFmpeg and Dolby DEE**, giving the
  encoder a real point of reference beyond its own gold-reference gate. A new stereo fixture
  exercises coupling, enhanced coupling, spx, AHT, transient pre-noise, and rematrixing together; a
  local-only baseline tool encodes fixed legs through FFmpeg, DEE, and `ac3cli`, while CI itself
  runs a compute-only trend mode scoring against those legs using only this project's own decoder —
  no FFmpeg or DEE invocation at CI time. Results render in two new docs pages,
  `docs/tool-comparison-trend.md` (per-commit, per-variant detail) and `docs/landscape.md`
  (release-over-release headline table). This work directly surfaced the `cplbndstrce=0` decoder
  gap fixed above, and found that the installed DEE build silently drops the Ls channel on discrete
  5.1 input — the affected rows are honestly marked `"status": "unverified"` rather than reporting
  a fabricated score.
- **The gold-reference gate now checks a real Annex E tool-enabled stream (`tools=cpl`)**, not just
  the `tools=none` baseline, at the existing 55 dB SNR floor. `spx`/`aht`/`all` are deliberately
  left off this specific check: those tools are approximate/generative reconstruction where two
  independent spec-correct decoders legitimately diverge much further, so a 55 dB floor would
  false-fail on normal divergence rather than catch a real regression.
- **The quality trend chart and tool-comparison trend chart both gained a per-series breakdown
  view** ("Worst of legs, by branch" / "By platform leg", and "By branch" / "By variant"), so one
  CI leg — or one Annex E tool-set — quietly drifting relative to its siblings is visible as a
  trend line instead of only by scanning table rows.

### Android (Shield)

- **Android release builds now sign with a real release keystore instead of the debug key**, once
  a maintainer has provisioned the `ANDROID_KEYSTORE_*` secrets per
  [docs/releasing.md](docs/releasing.md). Local dev, ordinary CI, and any release run with no
  keystore provisioned all still degrade to the debug keystore exactly as before.

### Bug fixes

- **Windows audio backends no longer list a blank row in the device picker.** A real WASAPI
  endpoint that never fills in its friendly-name property was enumerated with an empty display
  string, and both the capture and passthrough front ends put that straight into a combo box as an
  unlabeled entry. The fix resolves a display name through a fallback chain (friendly name → device
  description → an endpoint-id-carrying stand-in), and an endpoint whose id can't be read is now
  skipped entirely rather than listed.

### Library examples & documentation

- **Eight new `examples/` programs**, each a build target and `ctest` entry like every other
  example: `wav_roundtrip` (real WAV file I/O, not just in-memory PCM), `custom_layout` (a
  channel selection no named `LayoutId` covers, via `Plan::custom_locations`),
  `multi_source_assignment` (combining separate sources via `ac3::plan::Assignment`),
  `scripted_object_motion` (authored `KeyframePath`/`OrbitPath` driving `AtmosEncoder`),
  `object_signing` (`ac3::signing::sign_atmos_stream`, previously undemonstrated),
  `level_metering` (`ac3::analysis::LevelMeter`/`energy_vector`), `decode_robustness`
  (recovering from one damaged frame in an otherwise-good stream via `ac3::split_frames`), and
  `atmos_fallback` (`AtmosConfig::emit_object_metadata`'s objects-or-nothing design decision,
  side by side). Three new library reference pages —
  [Channel plans & routing](docs/library/channel-plans-and-routing.md),
  [File I/O](docs/library/file-io.md) and [Object signing](docs/library/signing.md) — and new
  sections on the existing [Spatial & Atmos objects](docs/library/spatial-and-atmos.md),
  [Decoding](docs/library/decoding.md) and [Muxing & sinks](docs/library/muxing-and-sinks.md)
  pages are written from them.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.

## [0.3.0-beta.1] - 2026-08-11

Second tagged release. Adds the two remaining Annex E coding tools (enhanced coupling,
transient pre-noise processing), a native Android app on NVIDIA Shield TV, packaged
`find_package(ac3forge)` libraries for third-party consumers, explicit multi-source channel
assignment, and a GUI tier split for first-time users through experts.

### E-AC-3 encoding and decoding

- **Enhanced coupling (§E3.5)** and **transient pre-noise processing (§3.7)**, the two Annex E
  tools the decoder previously recognised but refused (`DecodeError::kUnsupported`) — now
  implemented end to end, encoder and decoder, each behind its own tool token (`cpl+ecpl`,
  `tpn`). Enhanced coupling round-trips at the same ~20dB near-transparent bar as standard
  coupling for realistic content; transient pre-noise processing follows the spec's own
  time-scaling synthesis pseudocode, reusing the existing block-switch transient detector rather
  than a second one.
- Fixed two real conformance bugs found implementing the above: a missing §3.3.2 `nrematbd`
  formula for `ecplinu` (both encoder and decoder), and a systematic 2:1 gain error in enhanced
  coupling's FFT-based reconstruction pathway.
- `Eac3Decoder::decode_substream` now returns an optional decoded substream plus a new
  `flush()`, since transient pre-noise processing can hold a frame back until the next one
  confirms whether a correction reaches into it. Streams that never use the tool see no
  behavioural change.

### Dolby Atmos objects and multi-source encoding

- **Explicit multi-source channel assignment** alongside automatic routing — `ac3cli`'s encode
  commands take `src=`/`map=` to assign specific input files/channels to specific output
  channels and objects, instead of relying purely on automatic layout inference.
- Object mode now addresses objects by source, not a stale positional index, so multi-source
  sessions keep object identity stable as sources are added or reordered.

### GUI

- **Guided/Advanced/Expert tier split**: a real step-by-step wizard for first-time users, with
  Advanced and Expert tiers exposing the same controls power users had before.
- Multi-source input and an explicit per-channel assignment surface in the GUI, mirroring the
  CLI's `src=`/`map=`.
- **Dual mono (1+1) as a bed**, not a distinct layout — it now feeds the same object/motion
  pipeline as any other bed.
- **Variable bit rate** as a selectable GUI rate mode (a quality target with optional min/max
  kbps bounds), alongside CBR.
- Live sessions no longer clobber a file's authored objects when a live capture starts, and warn
  before silently dropping VBR settings that don't apply live.
- A Qt Quick Test harness drives the real `EncoderController` end-to-end, not a mock, for GUI
  regression coverage.

### Android (Shield) — new platform

- **ac3forge on NVIDIA Shield TV**: a native Android app (`platform/android/`) pairing
  `ac3::forge`/`ac3::audio` via JNI with a live Atmos demo — authored object trajectories,
  deflection, and ambient object motion, encoded and rendered on-device.
- HDMI receiver resilience hardening for the Shield demo, so a receiver renegotiating format
  mid-playback doesn't drop the session.
- Ships as a debug-signed `.apk` this release — see Known gaps.

### Library and packaging

- **`find_package(ac3forge)` support**: `ac3::forge` and `matroska::matroska` now build as
  proper static and shared CMake targets with `install()`/export support, so a third-party
  project can consume them without vendoring the source tree. `ac3::audio` (live capture/
  monitor/passthrough) stays CLI/GUI-internal, not part of what's installed.
- `ac3::forge` split into a platform-independent codec core plus `ac3::audio`, clearing the way
  for the library package above and for platforms — like Android — that only want the codec.

### Quality and packaging infrastructure

- Quality-trend dashboard redesign (readability, tightened gate thresholds) and a fix for CI
  concurrency dropping quality data mid-run.
- A round of security hardening prompted by OpenSSF Scorecard: hash-pinned CI tool installs,
  commit-SHA-pinned GitHub Actions (replacing tag-pinned ones), a `SECURITY.md`
  vulnerability-reporting policy, patched CVEs in docs dependencies, branch-protection scoring
  wired up, and build provenance republished as `.intoto.jsonl` for Scorecard to read.
- Several MSVC `/analyze` and clang-tidy findings fixed for real: heap-allocating large
  encoder/decoder objects out of worker-thread stacks, reusing MDCT scratch buffers instead of
  stack-declaring them per call, and a couple of static-analysis false-positive suppressions.
- macOS packaging now stays a single `.dmg` bundling both the runtime and library components,
  matching the archive packages' intent — CPack's DragNDrop generator defaulted to splitting
  per component the first time this leg actually ran on real macOS CI, caught by this release's
  own packaging dry run.

### Known gaps

- The Shield `.apk` ships debug-signed via Android's default debug keystore — no release
  keystore is provisioned in this repo yet, so it's a sideload-only build, not one suited for
  store distribution.
- Enhanced coupling's encoder always sends angle/chaos as zero (an amplitude-only fit) — quality
  degrades if two channels' content shares one narrow coupling band. Closed in
  [0.4.0-beta.1](#040-beta1---2026-08-14).
- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.

## [0.2.0-beta.1] - 2026-08-10

First tagged release. ac3forge is a clean-room AC-3 and E-AC-3 encoder and decoder in C++23,
implemented from the published standards — no FFmpeg or other codec library is linked, only
used during development as an independent oracle to check output against.

### AC-3 and E-AC-3 encoding

- Every AC-3 coding mode (1+1 dual mono, 1/0 through 3/2, each with or without LFE) at 48,
  44.1 and 32 kHz, CBR only, across all 19 nominal Table 5.18 bit rates. Exact 44.1 kHz timing
  via Bresenham alternation between the two legal frame lengths.
- E-AC-3, all of the above plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams, and
  either CBR or VBR (a quality target with optional min/max kbps bounds) per substream.
- Per-block, per-channel block switching (§8.2.2 transient detector, long 512-point vs. switched
  256-point transform pairs), automatic delta bit allocation (§7.2.2.6), and 2/0 rematrixing
  (§7.5.3) on AC-3.
- Channel coupling (§7.4 / §E3.3), spectral extension (§E3.6) and the adaptive hybrid transform
  with gain-adaptive quantization (§E3.4) on E-AC-3, each opt-in per stream.
- `fscod2`, Annex E's half sample rates (24, 22.05, 16 kHz).

### Dolby Atmos objects (Joint Object Coding)

- Mono sources placed and moved in 3D space, panned into a 5.1 bed with OAMD + JOC metadata
  carried in an EMDF container (ETSI TS 103 420) — playable as plain 5.1 by any decoder, and
  reconstructible as discrete objects by one that understands the container.
- Authored keyframe paths and closed-form orbits for object motion, both file-driven
  (`ac3cli atmos-path`) and live per-frame (`ac3cli live --atmos`).
- Syntax checked field-for-field against Dolby's own Reference Player and Dolby Media Encoder.

### Decoding

- A single in-repo decoder core shared with the encoder, reading both AC-3 and E-AC-3 —
  dependent substreams, `chanmap`, and the §E3.8.2 render — at float32-precision parity with
  FFmpeg on every layout FFmpeg itself can read.
- All three Annex E coding tools (coupling, spectral extension, AHT) decode individually or all
  stacked together, at every channel layout including 7.1.4 — the one combination FFmpeg cannot
  check at all, since its parser refuses a second dependent substream.
- Block switching and dual mono decode on both formats; decoded switch decisions are reported
  back (`DecodedFrame::blksw`), the same tier of diagnostic as `dynrng`.

### Metadata

- `dynrng` (five DRC profiles: film-standard, film-light, music-standard, music-light, speech),
  `compr` heavy compression, measured `dialnorm` (ITU-R BS.1770-4 gated loudness), and downmix
  levels (`cmixlev`/`surmixlev`, the E-AC-3 `mixmdate` group) — verified against FFmpeg applying
  the metadata, not just against the encoded bits.

### Live audio, capture and passthrough

- WASAPI (Windows) and ALSA (Linux) backends for live input/loopback capture, shared-mode
  monitor playback, and exclusive-mode S/PDIF (IEC 61937) bitstream passthrough — AC-3 and
  E-AC-3/Atmos alike.
- A lock-free SPSC ring carries samples from capture into the encoder; `ac3cli live` wires
  capture → encode → monitor/passthrough continuously.
- `MonitorSink` playback confirmed against real Windows hardware, including a live
  microphone-capture-to-monitor session; ALSA verified headless (WSL2 has no sound devices) plus
  under AddressSanitizer/UndefinedBehaviorSanitizer with leak detection.

### Tools and formats

- `ac3::io::scan`: derives stream format, access-unit boundaries and channel count directly from
  the bitstream.
- `matroska::matroska`: a standalone MKV muxer, independent of the codec library.
- `ac3::sinks::iec61937`: S/PDIF burst packing, byte-exact against FFmpeg's `spdif` muxer for
  AC-3 and independently verified against Microsoft's own IEC 61937 documentation for E-AC-3.
- `ac3::analysis`: peak/RMS/loudness metering with console ballistics and the Gerzon energy
  vector, shared by both front ends.
- `ac3cli`, a 21-command command-line front end, and `ac3gui`, a Qt Quick GUI with file and
  live-capture encoding, an object placement/motion view, and channel-level metering.

### Quality and packaging infrastructure

- CI across Windows (MSVC, clang-cl), Linux (GCC, Clang) and macOS (Homebrew LLVM) — CLI and GUI
  on Windows/Linux, CLI on macOS — plus a dedicated AddressSanitizer+UndefinedBehaviorSanitizer
  leg, clang-tidy static analysis, a coverage gate, a per-platform gold-reference quality gate,
  and an independent FFmpeg-validation leg.
- libFuzzer harnesses over every untrusted-input entry point (stream scanning, both decoders,
  WAV reading), run on every push and nightly with deeper mutation.
- Signed, attested release packages (Windows `.zip`/`.exe`, Linux `.tar.gz`/`.deb`/`.rpm`, macOS
  `.tar.gz`/`.dmg`) with SHA-512 checksums, keyless Sigstore/OIDC build provenance, and an SPDX
  SBOM — see [docs/releasing.md](docs/releasing.md).

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.
