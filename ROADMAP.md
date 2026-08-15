# Roadmap

Where ac3forge is headed after 0.5.0-beta.1 — a candidate list, not a commitment.

The first four releases were about depth: core correctness (0.2), Annex E completeness (0.3),
the GUI rebuild (0.4), and the fast-transform performance programme (0.5). The codec itself is
now broad and fast; almost everything valuable that remains is about **reach** — what the
library can ingest, emit, verify, and run on. That is what this list is organised around.

Every item carries a stable ID (`A1`–`G3`) so pull requests and discussions can reference it.
An item is checked off when the work is merged to `develop`; partial progress is noted inline
rather than half-checked. Sizes are honest guesses, not estimates to hold anyone to:
**S** (an afternoon), **M** (a day or two), **L** (a focused week), **XL** (a programme of
several PRs).

## The short list

Six bets lead the list, ranked by the size of the gap in the world times the evidence that
anyone actually wants it filled.

| # | Bet | Why it leads |
|---|-----|--------------|
| 1 | **ADM BWF ingest → DD+ JOC encode** (B1) | The master-to-consumer-bitstream step is exclusively proprietary today; no open path exists anywhere. Netflix and Apple both mandate ADM BWF masters. |
| 2 | **MP4 muxer with correct Atmos signaling** (A1–A2) | Years of open bug reports about lost or wrong `dec3` Atmos signaling across FFmpeg, Shaka Packager, and Jellyfin. Small scope, completes the "a phone will actually play it" story. |
| 3 | **Bitstream-aware loudness QC** (C1–C2) | Decode, measure, and verify dialnorm/DRC against R128/A85/platform gates. The decoder already exists; nothing open does this. |
| 4 | **WASM browser demo** (F3) | Decode E-AC-3 + JOC and visualise object motion in a browser tab. Nothing on the web can. |
| 5 | **Resume TrueHD/MLP** (D1) | Already in flight on a paused branch. An open TrueHD encoder with an Atmos story exists nowhere. |
| 6 | **C API → Python bindings** (F1–F2) | The encode-pipeline community lives in Python and currently wraps Dolby's proprietary encoder to get this done. |

## A. Delivery — plays everywhere

Matroska is currently the only container; `docs/project/history.md` already names MP4 and
MPEG-TS as the step the decoder was meant to lead toward.

- [ ] **A1 (L)** — MP4/ISOBMFF muxer with a correct `dec3` box. A standalone module in the
  `matroska::` mould: E-AC-3 (and AC-3) into MP4 with spec-correct `dec3` generation including
  the Atmos extension fields (`flag_ec3_extension_type_a`, `complexity_index`) derived from our
  own bitstream. *Evidence: FFmpeg remuxes silently drop Atmos signaling
  ([jellyfin-ffmpeg #584](https://github.com/jellyfin/jellyfin-ffmpeg/issues/584)); GPAC ships a
  dedicated repair option for it; HandBrake has a long-open request for E-AC-3 7.1 in MP4
  ([HandBrake #1085](https://github.com/HandBrake/HandBrake/issues/1085)).*
- [ ] **A2 (M)** — fMP4/CMAF segmenting plus HLS/DASH signaling helpers, so a stream from
  ac3forge drops straight into a packager — or arrives already packaged. Depends on A1.
  *Evidence: Shaka Packager has multiple long-standing E-AC-3 signaling bugs; Apple documents
  Atmos-over-HLS requirements precisely.*
- [ ] **A3 (M)** — MPEG-TS muxing with AC-3/E-AC-3 descriptors per ATSC/DVB. Lower priority
  than MP4 unless broadcast interop becomes a goal.
- [ ] **A4 (S)** — stdin/stdout streaming in `ac3cli`: the `-` filename convention for
  pipe-based workflows. Everything today is whole-file.
- [ ] **A5 (S)** — Live sessions mux straight to Matroska. Live captures currently write
  elementary streams only; `docs/gui/live-session.md` flags containerised output as pending.

## B. Production ingest — masters in

Today the object path starts from mono WAVs placed by hand or by the CLI/GUI. The industry's
actual deliverable is a Dolby Atmos master file.

- [ ] **B1 (XL)** — ADM BWF reader feeding the JOC encoder. Parse BS.2076 ADM metadata plus
  RF64/BWF audio, map beds and objects onto `AtmosEncoder`, and encode a real post-production
  master to a streamable DD+ Atmos bitstream. The first open implementation of a step every
  pipeline currently licenses from Dolby; Netflix mandates ADM BWF masters (128 channels,
  ≥ 7.1.4) and Apple's HLS authoring spec wants BWF ADM sources. Natural split: the ADM/RF64
  parser first, then object/bed mapping and trajectory conversion, then an end-to-end example.
- [ ] **B2 (M)** — DAMF reader: the `.atmos` / `.atmos.metadata` / `.atmos.audio` triple,
  sharing B1's mapping layer.
- [ ] **B3 (XL)** — IAMF / Eclipsa Audio interop. Long horizon: shared ADM ingest positions
  ac3forge as the bridge between the Dolby world and the royalty-free one, and could grow into
  E-AC-3+JOC ↔ IAMF conversion. *Context: Eclipsa shipped across Samsung's 2025 TV lineup,
  YouTube, and Android 16; FFmpeg 7.0 muxes IAMF;
  [iamf-tools](https://github.com/AOMediaCodec/iamf-tools) encodes from WAV with ADM conversion
  still experimental.*

## C. Loudness and QC

The library already measures BS.1770-4 gated loudness to set dialnorm. Nothing open decodes a
Dolby bitstream and verifies its loudness metadata against measurement — that is Dolby Media
Analyzer territory.

- [ ] **C1 (M)** — Full R128 metering: momentary and short-term loudness, loudness range (LRA),
  and oversampled true peak in `ac3::meta::loudness`. The prerequisite for real QC and for
  platform delivery specs.
- [ ] **C2 (L)** — `ac3cli qc`: decode a stream, measure, and compare against its embedded
  dialnorm/DRC with preset gates (EBU R128 s2, ATSC A/85, Netflix −27 LKFS). Pass/fail report.
  Depends on C1.
- [ ] **C3 (M)** — The same verification surfaced in the GUI workbench: meters against gate
  lines, dialnorm delta, per-preset verdicts. Depends on C2.
- [ ] **C4 (S)** — Finish `dialnorm=auto` everywhere. Two documented holes: unsupported with
  `src=`/`map=` multi-source, and for dual mono.

## D. Codec depth

- [ ] **D1 (XL)** — Resume TrueHD/MLP. `feature/truehd-atmos-support` already carries an
  `ac3::mlp` module — sync and restart-header framing, the lossless matrix cascade, Rice
  coding, CRC, tests, and a design doc (`docs/concepts/truehd-mlp.md`) — paused since
  2026-08-12. Next stop is a full substream encode. An open TrueHD encoder with an Atmos story
  exists nowhere; FFmpeg's is experimental, lossless-only, with no Atmos.
- [ ] **D2 (S)** — Decoder dither substitution. The only two literal TODOs in the tree:
  zero-mantissa bands currently emit silence instead of dither, in both decoders.
- [ ] **D3 (M)** — Delta bit allocation alongside coupling, and for LFE. Currently skipped
  whenever coupling is active that frame, and always for LFE — documented as "doesn't emit it
  yet". A real, if small, quality lever at low rates.
- [ ] **D4 (L)** — AC-4 bitstream parser/inspector: ETSI TS 103 190 parse-and-inspect as a
  first step, not an encoder. Newly legitimised in streaming (Peacock adopts AC-4 for the 2026
  World Cup), but every major streamer still ships DD+ JOC — a horizon item, entered cheaply.

## E. Platforms and hardware

- [ ] **E1 (L)** — macOS CoreAudio backend. Every live-audio command is a stub on macOS today —
  the build and gold-reference gates pass, but there is no capture, playback, or passthrough.
- [ ] **E2 (M)** — PipeWire backend, named in `docs/building.md` as the natural second Linux
  backend; PipeWire is the default on every current desktop distribution.
- [ ] **E3 (S)** — Confirm exclusive-mode passthrough against real bitstreaming hardware, the
  standing "Known gaps" bullet from 0.5.0. The bench exists — the receiver used for the Shield
  work is confirmed DD+/Atmos-capable — so this is an `ac3cli play` test session and a
  verification-doc update.
- [ ] **E4 (M)** — Linux aarch64 CI leg. The fast-transform work is now hardware-tuned; an
  ARM64 leg keeps it honest off x86 and hardens the Android path's foundations.

## F. API reach and distribution

- [ ] **F1 (L)** — C API over the encode/decode core: a stable, minimal C-callable surface.
  The unlock for every binding that follows, and for embedding from anything that isn't C++.
- [ ] **F2 (L)** — Python bindings on PyPI, with wheels for the three desktop platforms. The
  encode-pipeline community is Python-first — [deew](https://github.com/pcroland/deew) wraps
  Dolby's proprietary encoder and is widely used, which proves the demand. Depends on F1, or
  goes pybind11-direct.
- [ ] **F3 (L)** — WASM build plus a browser demo that decodes E-AC-3 + JOC and renders object
  motion in 3D. No other web page can decode an Atmos bitstream at all; this doubles as the
  documentation site's live demo.
- [ ] **F4 (M)** — Package-manager presence: a vcpkg port, a Homebrew formula, a winget
  manifest. The release artifacts already exist, signed and attested; nothing is discoverable
  through a package manager yet. Includes fixing the stale `version` field in `vcpkg.json`.
- [ ] **F5 (M)** — API freeze → v1.0.0: the SemVer commitment, a deprecation policy, ABI
  notes. The gate at the end of the road, once the surface stops moving.

## G. Validation depth

- [ ] **G1 (M)** — A perceptual-quality leg. SNR says the waveform is closer, not that it
  sounds better — `docs/verification.md` says so itself. Add a perceptual metric (ViSQOL or
  PEAQ) column to the quality race and landscape pages.
- [ ] **G2 (M)** — Backfill thin test coverage: the CLI has seven tests against roughly four
  thousand lines; the WAV reader, `meta/loudness`, `meta/mixing`, and `silent_frame` have no
  dedicated test files.
- [ ] **G3 (M)** — Differential decoder fuzzing against FFmpeg: feed the same mutated frames to
  both decoders and diff the PCM, catching wrong-but-stable decodes on the paths FFmpeg can
  check.

## One possible sequencing

A shape, not a schedule — each release keeps the established pattern of one headline plus a
supporting cast.

| Release | Headline | Items |
|---------|----------|-------|
| 0.6.0 | Plays everywhere | A1 A2 A4 A5 C4 D2 E3 |
| 0.7.0 | Masters in | B1 B2 C1 C2 C3 |
| 0.8.0 | In everyone's toolbox | F1 F2 F3 F4 G2 |
| 0.9.0 | New depths (pick by appetite) | D1 or D4 / B3 / E1 E2 E4 |
| 1.0.0 | The freeze | F5 G1 G3 |

## Deliberately not on the list

- **Forging Dolby's authenticity tag.** The licensing gate stays respected — see
  `docs/concepts/object-signing.md`.
- **APT/DNF repositories and Docker images.** Already ruled out in `docs/releasing.md`.
- **Renderer and room-correction territory.** That ground is well covered by
  [Cavern](https://github.com/VoidXH/Cavern); ac3forge differentiates on encoding, native
  embeddability, and open licensing.
- **AC-3 VBR.** Structurally impossible — the frame size indexes a fixed table. Documented,
  not a defect.

---

Drafted 2026-08-15 at v0.5.0-beta.1, from a full repository inventory plus a sweep of the
2025–26 ecosystem: the FFmpeg, HandBrake, Shaka Packager, and Jellyfin issue trackers, Dolby
and ATSC announcements, IAMF/Eclipsa tooling, and the Netflix and Apple delivery
specifications. When the world moves, this list should move with it — the changelog stays the
record of what actually happened.
