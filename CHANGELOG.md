# Changelog

*For end users tracking what has shipped. How releases and version numbers are cut lives in
[docs/releasing.md](docs/releasing.md); the project overview is in [README.md](README.md).*

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [docs/releasing.md](docs/releasing.md) for how releases and version numbers are cut.

## [Unreleased]

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
  degrades if two channels' content shares one narrow coupling band. Enhanced coupling and
  spectral extension are never combined by this encoder, and `decode_access_unit` refuses rather
  than supports transient pre-noise processing across multiple substreams of one access unit.
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
