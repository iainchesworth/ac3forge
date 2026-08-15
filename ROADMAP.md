# Roadmap

Ideas under consideration — a candidate list, not a commitment.

Each item carries a stable ID (`A1`–`G3`) so pull requests and discussions can reference it.
An item is checked off when the work is merged to `develop`; partial progress is noted inline
rather than half-checked. Sizes are rough guesses: **S** (an afternoon), **M** (a day or two),
**L** (a focused week), **XL** (several PRs).

## A. Delivery

Matroska is currently the only container; `docs/project/history.md` names MP4 and MPEG-TS as
the step the decoder was meant to lead toward.

- [x] **A1 (L)** — MP4/ISOBMFF muxer with a correct `dec3` box. A standalone module in the
  `matroska::` mould: E-AC-3 (and AC-3) into MP4 with spec-correct `dec3` generation, including
  the Atmos extension fields (`flag_ec3_extension_type_a`, `complexity_index`) derived from the
  bitstream. FFmpeg remuxes are known to drop Atmos signaling
  ([jellyfin-ffmpeg #584](https://github.com/jellyfin/jellyfin-ffmpeg/issues/584)), GPAC ships
  a repair option for it, and HandBrake has a long-open request for E-AC-3 7.1 in MP4
  ([HandBrake #1085](https://github.com/HandBrake/HandBrake/issues/1085)).
- [ ] **A2 (M)** — fMP4/CMAF segmenting plus HLS/DASH signaling helpers. Depends on A1.
- [ ] **A3 (M)** — MPEG-TS muxing with AC-3/E-AC-3 descriptors per ATSC/DVB.
- [x] **A4 (S)** — stdin/stdout streaming in `ac3cli`: the `-` filename convention for
  pipe-based workflows. Everything today is whole-file.
- [x] **A5 (S)** — Live sessions mux straight to Matroska. Live captures currently write
  elementary streams only; `docs/gui/live-session.md` flags containerised output as pending.

## B. Production ingest

The object path today starts from mono WAVs placed by the CLI or GUI; post-production Atmos
mixes are delivered as master files.

- [ ] **B1 (XL)** — ADM BWF reader feeding the JOC encoder: parse BS.2076 ADM metadata plus
  RF64/BWF audio, map beds and objects onto `AtmosEncoder`, and encode a master file to a
  DD+ JOC bitstream. No open implementation of this step exists today; the Netflix and Apple
  delivery specifications both use ADM BWF masters. Natural split: the ADM/RF64 parser first,
  then object/bed mapping, then an end-to-end example.
- [ ] **B2 (M)** — DAMF reader: the `.atmos` / `.atmos.metadata` / `.atmos.audio` triple,
  sharing B1's mapping layer.
- [ ] **B3 (XL)** — IAMF / Eclipsa Audio interop: shared ADM ingest, possibly growing into
  E-AC-3+JOC ↔ IAMF conversion.

## C. Loudness and QC

The library measures BS.1770-4 gated loudness to set dialnorm; no open tool decodes an
AC-3/E-AC-3 bitstream and verifies its loudness metadata against measurement.

- [ ] **C1 (M)** — Full R128 metering: momentary and short-term loudness, loudness range
  (LRA), and oversampled true peak in `ac3::meta::loudness`.
- [ ] **C2 (L)** — `ac3cli qc`: decode a stream, measure, and compare against its embedded
  dialnorm/DRC, with preset gates (EBU R128 s2, ATSC A/85, common platform specifications).
  Depends on C1.
- [ ] **C3 (M)** — The same verification in the GUI: meters against gate lines, dialnorm
  delta, per-preset verdicts. Depends on C2.
- [ ] **C4 (S)** — Finish `dialnorm=auto`: currently unsupported with `src=`/`map=`
  multi-source, and for dual mono.

## D. Codec depth

- [ ] **D1 (XL)** — Resume TrueHD/MLP. `feature/truehd-atmos-support` carries a started
  `ac3::mlp` module — sync and restart-header framing, the lossless matrix cascade, Rice
  coding, CRC, tests, and a design doc (`docs/concepts/truehd-mlp.md`). Next step is a full
  substream encode.
- [ ] **D2 (S)** — Decoder dither substitution: zero-mantissa bands currently emit silence
  instead of dither, in both decoders (the only two TODOs in the tree).
- [ ] **D3 (M)** — Delta bit allocation alongside coupling, and for LFE: currently skipped
  whenever coupling is active that frame, and always for LFE.
- [ ] **D4 (L)** — AC-4 bitstream parser/inspector: ETSI TS 103 190 parse-and-inspect as a
  first step, not an encoder.

## E. Platforms and hardware

- [ ] **E1 (L)** — macOS CoreAudio backend. Every live-audio command is currently a stub on
  macOS; the build and gold-reference gates pass, but there is no capture, playback, or
  passthrough.
- [ ] **E2 (M)** — PipeWire backend, named in `docs/building.md` as the natural second Linux
  backend.
- [ ] **E3 (S)** — Confirm exclusive-mode passthrough against real bitstreaming hardware —
  the standing "Known gaps" bullet from 0.5.0 — and update `docs/verification.md`.
- [ ] **E4 (M)** — Linux aarch64 CI leg, keeping the hardware-tuned transforms honest off
  x86.

## F. API reach and distribution

- [ ] **F1 (L)** — C API over the encode/decode core: a stable, minimal C-callable surface
  for bindings and embedding.
- [ ] **F2 (L)** — Python bindings on PyPI, with wheels for the three desktop platforms.
  Depends on F1, or goes pybind11-direct.
- [ ] **F3 (L)** — WASM build plus a browser demo that decodes E-AC-3 + JOC and renders
  object motion; could double as the documentation site's live demo.
- [ ] **F4 (M)** — Package-manager presence: a vcpkg port, a Homebrew formula, a winget
  manifest. Includes fixing the stale `version` field in `vcpkg.json`.
- [ ] **F5 (M)** — API freeze → v1.0.0: the SemVer commitment, a deprecation policy, ABI
  notes.

## G. Validation depth

- [ ] **G1 (M)** — A perceptual-quality leg: a perceptual metric (ViSQOL or PEAQ) column on
  the quality race and landscape pages alongside SNR.
- [ ] **G2 (M)** — Backfill thin test coverage: the CLI has seven tests against roughly four
  thousand lines; the WAV reader, `meta/loudness`, `meta/mixing`, and `silent_frame` have no
  dedicated test files.
- [ ] **G3 (M)** — Differential decoder fuzzing against FFmpeg: feed the same mutated frames
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
