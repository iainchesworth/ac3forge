# Changelog

*For end users tracking what has shipped. How releases and version numbers are cut lives in
[docs/releasing.md](docs/releasing.md); the project overview is in [README.md](README.md).*

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [docs/releasing.md](docs/releasing.md) for how releases and version numbers are cut.

## [Unreleased]

### Added

- **A property/fuzz harness over the AC-3 encoder's own input space**
  (`tools/fuzz_encoder_space.py`). Every fuzzing target this project had mutates an
  already-encoded bitstream, which asks whether the *decoder* survives corrupt input; the codec
  matrix walks a hand-enumerated list of command lines against one bootstrap tone. Neither has
  any notion of option *combinations*, and neither varies the input material. This one draws
  random legal encoder configurations crossed with adversarial PCM whose character can change
  part-way through a frame — which is what drives exponent-run splits, block switching and the
  delta bit allocation — then holds every resulting stream against both this project's decoder
  and FFmpeg's strict decode. Motivated by the `deltbaie` defect below, which produced streams
  both decoders reject and escaped every existing gate; reverting that fix, the harness finds
  rejected streams within seconds. Runs bounded on every pull request (in the FFmpeg-oracle
  job) and deeper nightly, mirroring how `fuzz.yml` already splits short from nightly. It has
  already surfaced a further, still-unfixed defect of its own: with coupling on, roughly one
  stream in seven hundred is refused by FFmpeg's bit allocation while this project's decoder
  accepts it, because the coupling channel's delta bit allocation offsets are written relative
  to band 0 rather than to that channel's own start band. Both CI callers are non-blocking
  until that is fixed.

- **A new `auto` E-AC-3 tool set, which picks coupling/spectral extension/AHT from the
  per-channel bitrate** instead of taking the on/off flags as given. Every Annex E tool trades
  waveform fidelity for a band it can describe more cheaply than it can code, so each is a win
  below some rate and a loss above it — `auto` applies the measured crossovers (56 kbit/s per
  channel for spectral extension; `12 + 14n` for coupling, whose saving scales with how many
  channels share the band). It still honours an explicit `cpl:N`/`spx:N`/`aht:N` band-edge pin,
  so geometry stays steerable without taking over the decision.

### Changed

- **The AC-3 encoder now gives the LFE its own fine SNR offset instead of copying the one every
  other channel gets.** The bitstream carries a separate `lfefsnroffst`, but this encoder wrote
  the shared value into it, which left the LFE a price-taker in a search it cannot influence: the
  offset search picks the one value at which the frame's *total* mantissa cost fits, and that
  total is set by channels of about 250 bins each. The LFE's 7 bins are rounding error in that
  sum, so its precision was decided entirely by channels 36 times its size — and it lost
  precision at the same rate as them despite costing a fraction as much to serve. Raising only
  its own field by 4 fine steps moves about 12 bits per frame at 448 kbit/s and leaves the
  frame's total mantissa cost unchanged. Measured on two materials (the 5.1 fixture and the
  synthesized full-band decorrelated 5.1) at 192/256/320/384/448/640 kbit/s: LFE SNR up at every
  point, by as much as 5.7 dB, overall SNR never lower, ViSQOL MOS flat.
- **The AC-3 encoder now weighs delta bit allocation against what it costs at every layout, not
  only when coupling is active.** A delta segment is 12 bits of side information taken from the
  same budget that would otherwise buy a higher composite SNR offset, so the encoder already
  re-ran its offset search with delta cleared and kept whichever pass came out higher — but only
  when a coupling channel existed, because that is where a failing test first exposed it. Nothing
  in that reasoning is about coupling, and the layouts that never couple were the ones paying
  most: 5.1 at 448 kbit/s was emitting about ten segments per block, 724 bits per frame, 5% of
  the whole frame. On the 5.1 reference this is worth 0.7 dB.
- **The AC-3 encoder raises `dbpbcod` from the §8.2.12 recommendation of 2 to 3.** `dbpbcod` sets
  the knee below which §7.2.2.5 lifts a band's excitation, so raising it steers bits away from
  bands holding almost no energy and towards the ones that do. Measured on three materials
  (the 5.1 and stereo fixtures and the synthesized full-band decorrelated 5.1) at 192/256/320/
  384/448/640 kbit/s, it improves SNR in every case — by 5.9 dB at 192 kbit/s on the 5.1
  reference, where there are fewest bits to misplace — with ViSQOL MOS flat or better throughout.
  The other four parameters are unchanged: `floorcod` turns out never to bind, and `fgaincod`,
  though worth more still at high rates, regresses at 192 kbit/s.
- Together with the LFE exponent fix below, these move the AC-3 5.1 landscape leg at 448 kbit/s
  from 36.02 dB to 39.71 dB — from 2.98 dB behind FFmpeg 8.0.1 to 0.72 dB ahead of it — with MOS
  unchanged at 3.67. The three are independent and were each measured separately: the delta cost
  check and `dbpbcod` account for 39.13 dB between them, and the LFE fix adds the remaining
  0.58 dB on top.
- **Coupling is now dropped, rather than moved down in frequency, when spectral extension leaves
  it no room.** §E3.3.1 derives the coupling end frequency from `spxbegf`; when that landed below
  the requested `cplbegf` the encoder used to slide `cplbegf` down to meet it, which silently
  coupled from 8.0 kHz where the rate model had asked for 10.2 kHz and made every coefficient
  above 8.0 kHz parametric. On the stereo reference at 192 kbit/s this was worth 6.8 dB of SNR
  (21.6 → 28.5 dB with all tools forced on).
- **The landscape comparison now reports `auto` rather than a forced `all`.** The headline number
  is meant to be what a real user of this encoder gets, the same standard applied to FFmpeg's and
  DEE's own automatic choices; `all` was a configuration this encoder would never itself choose.
  Against FFmpeg 8.0.1 the E-AC-3 stereo leg moves from −11.19 dB to −0.83 dB, and the 5.1 leg is
  unchanged at +0.49 dB.
- **The landscape page shows SNR, LSD and MOS side by side, each with its own vs-FFmpeg/vs-DEE
  delta.** These tools trade waveform fidelity for banded envelope fidelity deliberately, so a
  single-metric headline reported a working tool as a straight loss.
- **The CI quality gate now includes an AC-3 5.1 leg.** It was stereo-only, which left the LFE and
  the full channel count with no absolute gate — two separate faults have now shipped through that
  hole. The floor is deliberately loose: the gate decodes with FFmpeg under `-xerror`, so a
  malformed frame fails it as a hard decode error, which is the failure mode both faults had.
- **A new `tools/check_ac3_allocation.py`** reports per-channel and per-band SNR against FFmpeg at
  a matched bitrate, to say *which* part of an allocation gap is worth chasing rather than only
  that one exists. It is what found the LFE fault below.

### Fixed

- **AC-3 encoder: a delta bit allocation that ended part-way through a frame produced an
  undecodable stream.** `deltbaie = 0` means "keep the previous block's delta bit allocation",
  not "no delta" (A/52 §5.4.3.47), so a channel whose exponent run stopped wanting a correction
  mid-frame was never told to drop it. The decoder kept applying the stale correction, its bit
  allocation diverged from the encoder's, and every field after that point was read at the wrong
  bit offset — a stream both this project's decoder and FFmpeg reject. Real material hit this at
  several bitrates, 64 and 96 kbit/s stereo among them. E-AC-3 was unaffected.
- **AC-3 encoder: the LFE sent one exponent set per frame however much its level moved.** A
  frame's exponents are the per-bin minimum across the blocks they cover, so a single set for six
  blocks is a set chosen by the loudest of them and every quieter block was then quantized
  against a scale meant for something louder. §5.4.3.15 makes `lfeexpstr` a single bit, and the
  encoder was reading that bit as though it could only ever say "reuse". On the 5.1 reference the
  LFE moves 10–16 dB inside one frame, which cost 12 dB of LFE channel SNR — 56% of the whole
  encode's noise power, on a channel carrying a third of its signal. Worth +0.3 to +3.8 dB
  overall across 192–640 kbit/s (+1.6 at 448), for 18 bits per refresh against a 14336-bit frame.
  Stereo is unaffected, having no LFE.

## [0.6.0-beta.1] - 2026-08-17

Fifth tagged release. The main change is Atmos object *decode*: earlier releases could only
encode object audio, and decoding an Atmos file just played its 5.1 bed. The E-AC-3 decoder now
reads OAMD object positions and reconstructs JOC object audio, surfaced through the CLI's
`decode`/`monitor` commands, a new GUI object inspector, and a browser-based WASM demo that
renders real decoded object motion and solos individual object audio. A companion
`verify-objects` mode checks a stream's own EMDF authenticity tag (not Dolby's proprietary
decoder gate — see Known gaps).

Also landing this release: a standalone BW64/RF64 + Audio Definition Model (ADM) reader that
drives a real professional ADM BWF master straight through to a DD+ JOC E-AC-3 stream; two new
container writers — MP4/ISOBMFF (with fragmented MP4/CMAF segmenting plus HLS/DASH signaling)
and MPEG-2 Transport Stream — alongside the existing Matroska writer; full ITU-R BS.1770/EBU
R128 loudness metering and a bitstream-aware delivery-QC command; Raspberry Pi (arm64 Linux) and
a real macOS CoreAudio backend; and the library is now installable through vcpkg.

### Atmos object decode

- **The E-AC-3 decoder reads OAMD object metadata and reconstructs JOC object audio**, closing
  the gap where only the encoder side supported objects. `ac3cli decode`/`monitor` surface the
  decoded object layer directly (including per-object WAV export via `objects_dir`).
- **A new GUI "Inspect objects…" dialog** plays back a decoded Atmos stream's object positions
  and lets you solo individual objects' audio.
- **A browser-based WASM demo** renders real decoded object motion and solo-plays real isolated
  object audio, entirely in-browser.
- **`ac3::signing` gained stream verification** (`verify_atmos_frame`/`verify_atmos_stream`, CLI
  `verify-objects`): checks a stream's own embedded EMDF authenticity tag. This is opt-in and
  separate from Dolby's own decoder gate — see Known gaps.
- **The E-AC-3 decoder now applies dynamic range control** (`drc=`/`heavy`), matching the legacy
  AC-3 decoder; previously accepted and silently ignored.

### ADM ingest

- **A standalone BW64/RF64 + Audio Definition Model parser** (`ac3adm::ac3adm`) reads a
  professional ADM BWF master's object graph into memory, and a bridging layer maps it onto the
  Atmos object encoder's input shape.
- **`ac3cli atmos-adm`** drives both together end to end: a real ADM BWF master straight to a
  DD+ JOC E-AC-3 stream. This module is opt-in (`-DAC3FORGE_BUILD_ADM=ON`, off by default) since
  it needs several Boost header libraries; see [docs/library/index.md](docs/library/index.md).

### Delivery containers

- **A standalone MP4/ISOBMFF container writer**, with a spec-correct `dec3`/`dac3` box, plus
  fragmented MP4/CMAF segmenting and HLS/DASH manifest signaling.
- **A standalone MPEG-2 Transport Stream container writer.**
- **Live capture sessions can mux straight to Matroska.** The GUI's Format tab and the CLI both
  gained the new container options.

### Loudness & delivery QC

- **Full ITU-R BS.1770-4/EBU R128 metering**: momentary and short-term loudness, loudness
  range, and true peak.
- **`dialnorm=auto` finished for multi-source assignments and dual-mono streams**, in both the
  CLI and GUI (dual-mono measures each channel independently).
- **A new CLI `qc` command and GUI QC dialog** audit an already-encoded stream's bitstream-level
  loudness against its embedded metadata and delivery gates.
- **A perceptual-quality (ViSQOL) column** sits alongside SNR in the quality-comparison tooling.

### Platform & packaging

- **Raspberry Pi (arm64 Linux)** is now a supported platform, Pi 4/5 tier (Pi 3 out of scope on
  real-time budget grounds).
- **A real macOS CoreAudio backend** for live capture/monitor playback.
- **The library is installable via vcpkg** (staged in-tree pending submission to the curated
  registry — see [docs/releasing.md](docs/releasing.md#vcpkg-port)): `ac3::forge` plus
  `matroska`/`mp4`/`mpegts` as opt-in container-writer features.

### Fixes

- **AC-3 decode's reported dynamic-range floor was wrong whenever the true minimum sample was
  exactly 0.0 dB** — an accumulator seeded at 0.0 instead of the first real sample silently
  widened the reported range.
- **A flushed E-AC-3 dependent substream (e.g. a height-only pair at end of stream) could crash
  the CLI decoder** instead of writing correct audio, when its channel layout didn't match the
  program's main substream.
- **`fast-mdct=off` is now honored consistently** across all `eac3-encode`/`eac3-encode-multi`
  commands.
- **Piping CLI output to `-` no longer risks corrupting stdout** when `dialnorm=auto` or a
  multi-source summary is printed.
- **The GUI's auto-monitor preference now actually takes effect** on the input rail's Add
  button.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware: DD+ JOC gates
  that on an authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this
  project ships no key for. `verify-objects` checks a stream against its *own* signature, not
  Dolby's gate. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

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
