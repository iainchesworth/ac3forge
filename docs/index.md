# ac3forge

A clean-room AC-3 and E-AC-3 encoder and decoder in C++23, implemented from the published
standards. It turns PCM — or mono sources placed and moved in 3D space — into AC-3, E-AC-3,
or E-AC-3 with Joint Object Coding elementary streams, and reads those streams back.

Nothing here links FFmpeg or any other codec library. The FFmpeg command-line tools are used
during development as an independent decoder to check output against; the build does not
depend on them.

!!! warning "Standards and trademarks"
    "Dolby", "Dolby Digital" and "Dolby Atmos" are trademarks of Dolby Laboratories. This
    project implements the openly published standards — ATSC A/52:2018 (of which E-AC-3 is
    normative Annex E), ETSI TS 102 366 and ETSI TS 103 420 — and is not affiliated with,
    endorsed by, or certified by Dolby Laboratories. Code and documentation use the technical
    names AC-3 and E-AC-3. Whether the patents reading on these formats matter for your use is
    your problem to assess, not something this project resolves.

!!! note "Status"
    The API is not stable — releases so far are 0.x betas; the
    [changelog](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) records what
    each contains. Green and required in CI on Windows (MSVC, clang-cl), Linux (GCC and Clang,
    x64 and arm64) and macOS (Homebrew LLVM) — CLI and GUI alike on every platform — plus an
    ASan+UBSan leg, clang-tidy static analysis, a line/branch
    coverage gate over the library, a per-platform gold-reference *quality* gate, dedicated
    Linux FFmpeg- and ADM-validation legs checking output *correctness*, and a required Android
    build leg. No leg remains experimental. See [building.md](building.md) for exact toolchain
    versions and what each CI leg covers.

## What it does

### Encoding

| | AC-3 (bsid 8) | E-AC-3 (bsid 16) |
|---|---|---|
| Coding modes | 1+1 dual mono, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2, each with or without LFE (1+1 never carries one) | the same, plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams |
| Sample rates | 48, 44.1, 32 kHz | 48, 44.1, 32 kHz, plus the `fscod2` half rates 24, 22.05, 16 kHz (Annex E only) |
| Bit rates | CBR only — the 19 nominal rates of Table 5.18, 32–640 kbps | CBR (the same 19, per substream) or VBR — a quality target with optional min/max kbps bounds, per substream |
| Transform | long (512-point) or short (2x256-point) blocks, KBD window, chosen per block per channel by a §8.2.2 transient detector | same |
| Exponents | D15 / D25 / D45, strategy chosen per block from the reuse span (§8.2.8) | frame-level, Table E2.10 code 0: D15 in block 0, reused for the other five |
| Coupling | yes (§7.4), begin and end frequencies auto or pinned | yes (§E3.3) |
| Delta bit allocation | automatic (§7.2.2.6), like rematrixing below — no toggle | automatic, same as AC-3 |
| Rematrixing | yes, 2/0 (§7.5.3 minimum-power rule) | yes, 2/0 — the same rule, over Table 7.25's bands clamped to wherever coupling or spectral extension takes over |
| Annex E tools | — | spectral extension (§E3.6), enhanced coupling (§E3.5), adaptive hybrid transform with GAQ (§E3.4), transient pre-noise processing (§3.7) |
| Objects | panned to a 5.1 bed (no metadata survives) | OAMD + JOC in an EMDF container (TS 103 420) |

At 44.1 kHz, CBR needs non-integral frame sizes; the AC-3 encoder alternates between the two
Table 5.18 lengths on a Bresenham accumulator so the long-run rate is exact. E-AC-3 signals
`frmsiz` directly and needs no such alternation.

**Block switching's scope**: a §8.2.2 transient detector (cascaded biquad 8 kHz high-pass, a
256/128/64-sample peak-ratio tree) runs per full-bandwidth channel per block; a channel that
switches anywhere in the frame is excluded from coupling and, on E-AC-3, from AHT for that whole
frame — this project's coupling and AHT decisions are frame-wide all-or-nothing, so there is no
per-channel toggle to hook a narrower exclusion into. The LFE never switches (§8.2.2 defines the
detector over full-bandwidth channels only).

**Delta bit allocation's scope**: the encoder compares the coarse exponent-only masking curve
§7.2.2.2-7.2.2.5 derive against one built from the real, pre-quantization coefficient magnitude,
and corrects bands where the two clearly diverge (at least a full 6 dB Table 5.17 step). It is
skipped for the LFE channel (no such field exists for it). On AC-3 the coupling channel is in
§7.2.2.6's scope like any full-bandwidth channel, and `cpldeltbae` is emitted whenever
correction segments exist. On E-AC-3 it is skipped, for now, for every channel whenever coupling
is in use that frame — the coupling channel is a synthesized average rather than a real recorded
signal, and even leaving only the coupled channels' own narrow below-`cplstrtmant` region
eligible measurably narrowed coupling's usual cost advantage and broke its tightest scenarios
(128 kbit/s 5.1). The decoder accepts delta bit allocation on any channel, either generation,
from any encoder.

### Metadata

| Field | Section | What it does here |
|---|---|---|
| `dynrng` | §7.7.1 | Per-block dynamic range control from an RMS-detected compressor on a piecewise-linear curve. Five profiles: `film-standard`, `film-light`, `music-standard`, `music-light`, `speech`. A/52 fixes the wire format and the intent but not the curve, so the profiles are this project's, not the standard's. |
| `compr` | §7.7.2 | Heavy compression as a limiter guaranteeing a peak ceiling in the §7.8 mono downmix. Rounds down, because nearest-code rounding can overshoot a ceiling by half a step. Its peak detector includes the previous frame's MDCT overlap. |
| `dialnorm` | §5.4.2.8 | Measured with ITU-R BS.1770-4 gated loudness and negated, or set directly. A/52 predates BS.1770 and leaves the measurement open. |
| Downmix levels | Tables 5.9/5.10, E1.2 | `cmixlev`/`surmixlev` in AC-3; the whole `mixmdate` group in E-AC-3. |

### Decoding

The in-repo decoder shares its tables, bit-allocation engine, exponent decoding and IMDCT with
the encoder. It reads AC-3 (bsid ≤ 8) and E-AC-3 (bsid 11–16), including dependent substreams,
`chanmap`, and the §E3.8.2 render that lays a dependent's channels over the bed. Every Annex E
coding tool decodes too — standard coupling (§E3.3), enhanced coupling (§E3.5, a full FFT-based
phase-restoring reconstruction over 22 sub-bands), spectral extension (§E3.6, including the
pseudo-random noise blend the standard requires but leaves the exact generator unspecified), the
adaptive hybrid transform with GAQ (§E3.4), and transient pre-noise processing (§3.7) —
individually or all stacked together, at every channel layout including 7.1.4.

Transient pre-noise processing holds a frame back per substream that uses it (see [What it does
not do](#what-it-does-not-do)), and that holding-back is not just a `decode_substream` detail:
`Eac3Decoder::decode_access_unit` assembles a whole access unit correctly even when only some of
its substreams set the flag, queuing whichever substreams release early rather than losing or
misaligning them against the one still catching up.

### Other

| Component | What it is |
|---|---|
| `ac3::io::scan` | Finds access-unit boundaries in a raw elementary stream and reports what it renders, without being told. |
| `matroska::matroska` | A standalone MKV muxer. Links nothing from `ac3::forge` and knows nothing about AC-3. |
| `mp4::mp4` | A standalone MP4/ISOBMFF muxer, same shape as `matroska::matroska`. `ac3::io::build_codec_config_box` builds a spec-correct `dac3`/`dec3` sample-entry box (ETSI TS 102 366 Annex F), Dolby Atmos extension included, straight off the bitstream. |
| `mpegts::mpegts` | A standalone MPEG-2 Transport Stream muxer (PAT + PMT + one PES-wrapped elementary stream), identifying AC-3/E-AC-3 per DVB's ETSI EN 300 468 Annex D descriptors. Links nothing from `ac3::forge` beyond the AC-3/E-AC-3 choice it is told. |
| `ac3adm::ac3adm` | A standalone BW64/RF64 + Audio Definition Model reader (container and metadata parsing; object/bed mapping onto `ac3::oba::AtmosEncoder` is not wired up yet). Parses the container (ITU-R BS.2088-1) and the ADM XML graph (ITU-R BS.2076-2) on top of the vendored libbw64/libadm (github.com/ebu); links nothing from `ac3::forge` and knows nothing about AC-3. Opt-in (`-DAC3FORGE_BUILD_ADM=ON`, needs Boost) — the one component in this project with a third-party dependency. |
| `mp4::fragment` + `mp4/hls.hpp` + `mp4/dash.hpp` | Fragmented MP4/CMAF segmenting (init segment + media segments, ISO/IEC 14496-12 §8.8 / ISO/IEC 23000-19) plus HLS media/master playlist and DASH `AdaptationSet` signaling helpers for the same segments — correct `CODECS`/`codecs` (RFC 6381) and, for Dolby Atmos, HLS's `CHANNELS="<N>/JOC"` (Apple's HLS Authoring Specification). `ac3cli fmp4` wraps the whole thing. |
| `ac3::iec61937` | S/PDIF burst packing: AC-3 byte-exact against FFmpeg's `spdif` muxer; E-AC-3 (`Eac3BurstPacker`) verified against FFmpeg's `spdif_header_eac3` and Microsoft's own IEC 61937 documentation (both independently fetched, not recalled — see the caveats below). |
| `ac3::audio` | Live input/loopback capture — WASAPI on Windows, ALSA on Linux, CoreAudio on macOS (input only, no loopback) — through a lock-free SPSC ring. |
| `ac3::audio::PassthroughSink` | Exclusive-mode/direct bitstream output, AC-3 or E-AC-3 — WASAPI on Windows, ALSA on Linux, CoreAudio on macOS, JNI-bridged `AudioTrack` on Android. See the caveats below (Windows and Android hardware-confirmed; the ALSA and CoreAudio backends are not). |
| `ac3::audio::MonitorSink` | Shared-mode PCM playback — WASAPI, ALSA or CoreAudio: a non-bitstreamed preview/monitor path that decodes what is being encoded and plays it back on an ordinary output. Confirmed against real Windows hardware. |
| `ac3::analysis` | Peak/RMS metering with console ballistics, and the Gerzon energy vector over the BS.775 ring. |
| `ac3::meta::qc` | Bitstream-aware loudness QC (`ac3cli qc`): decodes a stream, measures it with the real BS.1770-4/EBU Tech 3342 meter, and compares against the stream's own embedded `dialnorm`/`compr` and, optionally, a named delivery-spec gate — EBU R 128 s2, ATSC A/85, or Netflix's Sound Mix Specifications, each preset's target/tolerance/true-peak ceiling cited from its own primary source. |

## What it does not do

The full picture — verification gaps, quality numbers, and exactly what has and has not been
confirmed against real hardware — is [Validation](verification.md), not here. Two gaps are
load-bearing enough to flag up front:

!!! warning "Objects will not decode as objects in Dolby's decoder"
    DD+ JOC gates object decoding on a keyed, sequence-bound HMAC-SHA-256 tag in the EMDF
    `protection` field — which the standard itself leaves "implementation dependent and not
    defined" — keyed on a secret embedded in decoder binaries. Streams from here are
    spec-correct (FFmpeg validates them, the bed decodes bit-exactly, Dolby's own parser reports
    `atmos=true`) but this project ships no key, so by default they are unsigned and Dolby's
    decoder falls back to the 5.1 bed; an operator who has a key can sign with it — see
    [Object signing](library/signing.md). The gate is authenticity, not conformance. Forging
    the tag is deliberately not attempted. What is verified about reconstruction is the
    mathematics: §6.6.6 applied per band recovers each object to better than −20 dB.

!!! warning "No Linux audio output has reached a real receiver"
    The ALSA backend was verified headless (including against ALSA's software `null` device,
    under ASan+UBSan), and device enumeration has since run against real ALSA/HDMI devices on
    Raspberry Pi hardware (see [Raspberry Pi](platforms/raspberry-pi.md)). But nothing has been
    bitstreamed to an actual S/PDIF or HDMI output on Linux, and
    no AV receiver has been asked to lock onto it there. This is a genuine gap, not one this
    project's other real-hardware confirmations paper over — `PassthroughSink`'s Android backend
    (see [Shield Atmos Demo](platforms/android.md)) has been confirmed bitstreaming real E-AC-3/
    Atmos to a real AV receiver over HDMI, which validates the sink abstraction and burst-packing
    logic in general, but says nothing about ALSA's own implementation specifically.

Enhanced coupling and transient pre-noise processing have no external decode oracle at all —
not even the FFmpeg-can't-but-the-in-repo-decoder-can situation 7.1.4 is in, since FFmpeg's own
Annex E parser has never read either tool's syntax — so `tools/quality_race.py`'s CI gate scores
both through this project's own decoder instead (see
[Validation](verification.md#where-the-oracles-dont-reach)). Transient pre-noise processing's
one-frame decoder buffering is an API characteristic, not a gap; [Decoding](library/decoding.md)
covers it. Variable bit rate is E-AC-3 only — AC-3's frame size indexes Table 5.18 rather than
stating a word count directly, so it has no equivalent and stays CBR.

## Where to go next

- **Getting started** — [Quick start](quickstart.md): clone to first encode in under ten
  minutes.
- **Concepts** — [Overview](concepts/index.md): AC-3, E-AC-3 and the Atmos/JOC object layer
  explained.
- **Validation** — [how output is checked](verification.md): quality numbers, oracle coverage,
  and exactly where it runs out.
- **Library** — [Conventions](library/index.md): the public C++ API, with
  [compiled examples](library/examples.md).
- **CLI reference** — [Overview](cli/index.md): `ac3cli`'s commands and option grammars.
- **GUI guide** — [Window layout](gui/index.md): `ac3gui`, the Qt Quick front end.
