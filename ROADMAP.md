# Roadmap

Ideas under consideration — a candidate list, not a commitment.

Each item carries a stable ID (`A1`–`G3`) so pull requests and discussions can reference it.
An item is checked off when the work is merged to `develop`; partial progress is noted inline
rather than half-checked. Sizes are rough guesses: **S** (an afternoon), **M** (a day or two),
**L** (a focused week), **XL** (several PRs).

## A. Delivery

At drafting time Matroska was the only container; `docs/project/history.md` named MP4 and
MPEG-TS as the step the decoder was meant to lead toward.

- [x] **A1 (L)** — MP4/ISOBMFF muxer with a correct `dec3` box. A standalone module in the
  `matroska::` mould: E-AC-3 (and AC-3) into MP4 with spec-correct `dec3` generation, including
  the Atmos extension fields (`flag_ec3_extension_type_a`, `complexity_index`) derived from the
  bitstream. FFmpeg remuxes are known to drop Atmos signaling
  ([jellyfin-ffmpeg #584](https://github.com/jellyfin/jellyfin-ffmpeg/issues/584)), GPAC ships
  a repair option for it, and HandBrake has a long-open request for E-AC-3 7.1 in MP4
  ([HandBrake #1085](https://github.com/HandBrake/HandBrake/issues/1085)).
- [x] **A2 (M)** — fMP4/CMAF segmenting plus HLS/DASH signaling helpers. Depends on A1.
- [x] **A3 (M)** — MPEG-TS muxing with AC-3/E-AC-3 descriptors per ATSC/DVB.
- [x] **A4 (S)** — stdin/stdout streaming in `ac3cli`: the `-` filename convention for
  pipe-based workflows. Everything today is whole-file.
- [x] **A5 (S)** — Live sessions mux straight to Matroska. At drafting time, live captures
  wrote elementary streams only; `docs/gui/live-session.md` flagged containerised output as
  pending.

## B. Production ingest

The object path today starts from mono WAVs placed by the CLI or GUI; post-production Atmos
mixes are delivered as master files.

- [x] **B1 (XL)** — ADM BWF reader feeding the JOC encoder: parse BS.2076 ADM metadata plus
  RF64/BWF audio, map beds and objects onto `AtmosEncoder`, and encode a master file to a
  DD+ JOC bitstream. At drafting time no open implementation of this step existed; the Netflix
  and Apple delivery specifications both use ADM BWF masters. Natural split: the ADM/RF64 parser
  first, then object/bed mapping, then an end-to-end command and example. **Phase 1 (the
  standalone `ac3adm::ac3adm` BW64/RF64 + ADM parser, ITU-R BS.2088-1 + BS.2076-2) and phase 2
  (the `ac3::admbridge` object/bed mapping layer onto `AtmosEncoder`, including BS.2076-2 §10.3
  position/gain automation and the polar/Cartesian → room-anchored coordinate conversion) are both
  done** — see [`src/ac3adm/`](https://github.com/iainchesworth/ac3forge/tree/main/src/ac3adm) and
  `docs/library/adm.md`, and [`src/adm_bridge/`](https://github.com/iainchesworth/ac3forge/tree/main/src/adm_bridge)
  and `docs/library/adm-bridge.md`. **Phase 3 (driving both together end to end) is also done** —
  the `ac3cli atmos-adm` command (`src/cli/main.cpp`) and
  [`examples/encode_adm.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/encode_adm.cpp).
  `ac3cli` still builds and works identically whether `AC3FORGE_BUILD_ADM` is on or off, just with
  or without this one command — with no preprocessor conditional anywhere
  (`scripts/check-platform-macros.ps1` forbids one under `src/`, feature flags included):
  `atmos-adm` is always one row in `main.cpp`'s command table, gated at dispatch time by a new
  `Needs::kAdm`/`unmet()` case (the same mechanism `Needs::kCapture`/`kPassthrough`/`kMonitor`
  already use for platform audio capability), backed by a small CMake-selected file pair
  (`src/cli/adm/{enabled,disabled}/atmos_adm.cpp`) rather than an `#ifdef` — see
  `src/cli/adm/atmos_adm.hpp`'s own comment for the full reasoning. Built on the vendored
  libbw64/libadm (github.com/ebu) rather than a hand-rolled parser; opt-in via
  `-DAC3FORGE_BUILD_ADM=ON` (needs Boost, see `vcpkg.json`'s `adm` feature) — the only third-party
  dependency anywhere in this project, and deliberately not default-on.
- [ ] **B2 (M)** — DAMF reader: the `.atmos` / `.atmos.metadata` / `.atmos.audio` triple,
  sharing B1's mapping layer.
- [ ] **B3 (XL)** — IAMF / Eclipsa Audio interop: shared ADM ingest, possibly growing into
  E-AC-3+JOC ↔ IAMF conversion.

## C. Loudness and QC

The library measures BS.1770-4 gated loudness to set dialnorm; at drafting time no open tool
decoded an AC-3/E-AC-3 bitstream and verified its loudness metadata against measurement
(`ac3cli qc` now does).

- [x] **C1 (M)** — Full R128 metering: momentary and short-term loudness, loudness range
  (LRA), and oversampled true peak in `ac3::meta::loudness`.
- [x] **C2 (L)** — `ac3cli qc`: decode a stream, measure, and compare against its embedded
  dialnorm/DRC, with preset gates (EBU R128 s2, ATSC A/85, common platform specifications).
  Depends on C1.
- [x] **C3 (M)** — The same verification in the GUI: meters against gate lines, dialnorm
  delta, per-preset verdicts. Depends on C2.
- [x] **C4 (S)** — Finish `dialnorm=auto`: now measures each programme/source independently
  for dual mono (CLI and GUI) and for `src=`/`map=` multi-source.

## D. Codec depth

- [ ] **D1 (XL)** — Resume TrueHD/MLP. `feature/truehd-atmos-support` carries a started
  `ac3::mlp` module — sync and restart-header framing, the lossless matrix cascade, Rice
  coding, CRC, tests, and a design doc (`docs/concepts/truehd-mlp.md`). Next step is a full
  substream encode.
- [x] **D2 (S)** — Decoder dither substitution: zero-mantissa bands emitted silence instead
  of dither, in both decoders (at drafting time, the only two TODOs in the tree).
- [x] **D3 (M)** — Delta bit allocation alongside coupling: previously skipped whenever
  coupling was active that frame, for the coupling channel and every fbw channel alike.
  LFE was already correct as it stood - A/52 defines no delta bit allocation field for it
  at all (§7.2.2.6, §5.4.3.49), so nothing changed there.
- [ ] **D4 (L)** — AC-4 bitstream parser/inspector: ETSI TS 103 190 parse-and-inspect as a
  first step, not an encoder.

## E. Platforms and hardware

- [x] **E1 (L)** — macOS CoreAudio backend. At drafting time every live-audio command was a
  stub on macOS; the build and gold-reference gates passed, but there was no capture,
  playback, or passthrough.
- [x] **E2 (M)** — PipeWire backend, named in `docs/building.md` as the natural second Linux
  backend. Landed with a real, honest scope: capture and monitor playback are genuine native
  `pw_stream` PCM; IEC 61937 passthrough negotiates for real (`SPA_MEDIA_SUBTYPE_iec958`,
  confirmed against a real shipped client) but is contingent on the target node's `iec958Codecs`
  being enabled by the session manager, which this library cannot do on a caller's behalf — see
  `src/platform/pipewire/passthrough.cpp`. ALSA keeps first precedence when both are present;
  see `docs/building.md`'s "Why ALSA still comes first".
- [ ] **E3 (S)** — Confirm exclusive-mode passthrough against real bitstreaming hardware —
  the standing "Known gaps" bullet from 0.5.0 — and update `docs/verification.md`.
- [x] **E4 (M)** — Linux aarch64 CI leg, keeping the hardware-tuned transforms honest off
  x86. Landed as part of Raspberry Pi platform support (#139): `ubuntu-24.04-arm` GCC and
  LLVM legs in `_build.yml`.

## F. API reach and distribution

- [ ] **F1 (L)** — C API over the encode/decode core: a stable, minimal C-callable surface
  for bindings and embedding.
- [ ] **F2 (L)** — Python bindings on PyPI, with wheels for the three desktop platforms.
  Depends on F1, or goes pybind11-direct.
- [x] **F3 (L)** — WASM build plus a browser demo that decodes E-AC-3 + JOC and renders
  object motion; could double as the documentation site's live demo. `ac3::forge`'s
  AC-3/E-AC-3 decode path builds under Emscripten (`config-wasm-emscripten` preset), and a
  real browser demo (`platform/wasm/`, embedded live at `docs/wasm-demo.md`) decodes a
  genuine Atmos-in-DD+ stream, plays the real 5.1 bed, and renders each object's real
  decoded position (OAMD, #168) moving in a top-down/elevation room view — plus a "solo
  object" control that plays that object's own real JOC-reconstructed audio (#169), not its
  panned slice of the bed. Both #168 and #169 are merged to `develop`.
- [ ] **F4 (M)** — Package-manager presence: a vcpkg port, a Homebrew formula, a winget
  manifest. **The vcpkg port is staged in-tree** — see
  [`packaging/vcpkg-port/ac3forge/`](https://github.com/iainchesworth/ac3forge/tree/main/packaging/vcpkg-port/ac3forge),
  with the submission and per-release update flow documented in `docs/releasing.md`; what
  remains is the `microsoft/vcpkg` registry submission itself, plus Homebrew and winget. The
  root `vcpkg.json`'s `version` field is a deliberate placeholder (the build's version derives
  from git tags, and releases are tracked by the port's own `version-semver`), not something
  to fix.
- [ ] **F5 (M)** — API freeze → v1.0.0: the SemVer commitment, a deprecation policy, ABI
  notes.

## G. Validation depth

- [x] **G1 (M)** — A perceptual-quality leg: a perceptual metric (ViSQOL or PEAQ) column on
  the quality race and landscape pages alongside SNR.
- [x] **G2 (M)** — Backfill thin test coverage. Resurveyed rather than trusting the original
  estimate (already stale): the WAV reader and `meta/mixing` genuinely had no dedicated test file
  and got one each (`tests/test_wav.cpp`, `tests/test_mixing.cpp`); `meta/loudness` and
  `silent_frame` turned out to already be solidly covered (`tests/test_loudness.cpp`,
  `tests/test_frame.cpp`) and were left alone; the CLI's `silence`/`eac3-silence` commands had zero
  coverage of their own argv wiring and got some.
- [x] **G3 (M)** — Differential decoder fuzzing against FFmpeg: feed the same mutated frames
  to both decoders and diff the PCM.

## Deliberately not on the list

- **Forging Dolby's authenticity tag** — see `docs/concepts/object-signing.md`.
- **APT/DNF repositories and Docker images** — ruled out in `docs/releasing.md`.
- **Renderer and room-correction territory** — already covered by
  [Cavern](https://github.com/VoidXH/Cavern).
- **AC-3 VBR** — structurally impossible; the frame size indexes a fixed table.

---

Drafted 2026-08-15 at v0.5.0-beta.1 from a repository inventory and a survey of the
surrounding ecosystem.
