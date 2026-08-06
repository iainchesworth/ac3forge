# AC-3 Encoder App — Research & Design Brief

**Project:** a C++23 Windows application that converts stereo/multichannel PCM audio into a Dolby Digital (AC-3) stream.
**Status of this document:** synthesis of six research tracks (codec, legal, encoder libraries, audio input, audio output, build toolchain), with load-bearing claims adversarially verified. Claims that verification could not confirm are flagged inline.

## The short version

AC-3 — the codec marketed as "Dolby Digital" — is a lossy, transform-based audio compression format from the early 1990s, standardized publicly as ATSC A/52 and ETSI TS 102 366. Every patent on it expired by March 2017, so we can implement, encode, and ship it royalty-free. The name "Dolby Digital" is a live trademark, however, so the product must be branded around "AC-3", not "Dolby".

We do not write the encoder ourselves. FFmpeg's libavcodec contains a mature, native, LGPL-licensed `ac3` encoder that handles all the hard signal-processing internally. It is available through the vcpkg `ffmpeg` port (currently 8.1.2) with no extra feature flags, alongside `libswresample` (format/rate conversion) and `libavformat` (file containers and the IEC 61937 "S/PDIF" packer we will want later).

The pipeline is: **audio source → conditioner (convert to planar float, 48 kHz, a supported channel layout) → AC-3 encoder (fed exactly 1536 samples per channel per frame) → sink (raw `.ac3` file first; later live passthrough to an AV receiver)**. Encoding costs about 0.1 ms of CPU per 32 ms frame — throughput is a non-issue; latency and buffer management are the real engineering work.

The MVP is a command-line converter: WAV in, `.ac3` file out, verified with `ffprobe` and VLC. The headline later milestone is a software re-creation of "Dolby Digital Live": capture what Windows is playing, encode it to 5.1 AC-3 in real time, and bitstream it to an AV receiver over S/PDIF or HDMI so the receiver's "Dolby Digital" light comes on.

## 1. What Dolby Digital / AC-3 actually is

**PCM** (pulse-code modulation) is uncompressed digital audio: a stream of numbers, each one the amplitude of the sound wave at an instant, sampled tens of thousands of times per second per channel. A WAV file is essentially PCM with a small header.

**AC-3** ("Audio Codec 3") is a *lossy perceptual codec*: it shrinks PCM roughly 10:1 by discarding detail the human ear cannot hear, using a model of auditory masking (loud sounds hide quiet ones near them in frequency). It was introduced in cinemas in 1991–92, then became the mandatory audio format for DVD-Video and US digital TV (ATSC), is mandatory on Blu-ray, and is accepted by effectively every AV receiver made in the last 25 years. That universality is why it is still worth targeting in 2026: it is the lowest common denominator of surround sound.

Hard format constraints, verified directly against the ATSC A/52:2018 standard (a free PDF — the spec is public):

- **Frame structure.** The bitstream is a sequence of self-contained "syncframes". Each holds 6 blocks × 256 samples = **1536 PCM samples per channel**, begins with the sync word `0x0B77`, and carries its own CRCs. At 48 kHz a frame is exactly **32 ms**. A `.ac3` file is nothing but these frames back-to-back — no global header, no index.
- **Sample rates:** exactly 48, 44.1, or 32 kHz. Anything else must be resampled first. In practice the whole ecosystem runs at 48 kHz.
- **Bit rates:** 19 fixed values from 32 to 640 kbit/s (constant bit rate; frame byte size is fixed per rate — 640 kbit/s at 48 kHz is exactly 2560 bytes/frame). DVD and ATSC broadcast cap at 448 kbit/s; 640 is fine for Blu-ray and receiver passthrough.
- **Channels:** mono up to **5.1** — at most 5 full-bandwidth channels plus one optional **LFE** (low-frequency effects, the subwoofer channel). There is no 7.1 AC-3; that is E-AC-3 territory.
- **Channel order pitfall:** AC-3's internal order (L, C, R, SL, SR, LFE) differs from WAV/Windows order (FL, FR, FC, LFE, BL, BR). Verified good news: FFmpeg's encoder accepts WAV-style order and remaps internally, so we never hand-reorder channels.

**E-AC-3** (Dolby Digital Plus) is the successor — higher bitrates, more channels, used by streaming services. It is *not* playable by plain AC-3 decoders and has a different (fresher) patent situation; see §3. It is a possible future extension, not the target.

## 2. The complexity map — what the encoder does vs. what we decide

Everything in this first list lives inside libavcodec, and we never touch it. It is listed only so the internals are demystified:

- **Transform:** each block of samples is converted to 256 frequency coefficients via an MDCT (modified discrete cosine transform — a Fourier-like transform designed for overlapping audio blocks). A transient detector switches to shorter transforms on percussive attacks to avoid "pre-echo" smearing.
- **Exponent/mantissa coding:** each coefficient is split into a binary exponent (coarse magnitude, forming the "spectral envelope") and a mantissa (fine detail), with three envelope-resolution strategies.
- **Bit allocation:** a psychoacoustic model computes a masking curve from the envelope and hands out mantissa bits where the ear will notice. Cleverly, the *decoder* runs the same model, so almost no allocation side-data is transmitted.
- **Coupling and rematrixing:** two bitrate-stretching tricks — sharing high-frequency content between channels, and mid/side coding for stereo. FFmpeg's defaults for both are correct; leave them alone.

What **we** must decide, because the encoder cannot infer it from the audio — these are *metadata* fields written into the bitstream that consumer decoders act on:

- **dialnorm** (dialogue normalization): a 5-bit field, −1…−31, declaring how far average dialogue level sits below digital full scale. Decoders attenuate playback by (31 + dialnorm) dB to level-match programs. FFmpeg's default of **−31 means "no attenuation"**, which plays ~4 dB hotter than typical commercial content (often −27). The right approach is to measure loudness per ITU-R BS.1770 (libebur128 is in vcpkg) and set dialnorm from the measurement; a fixed −27 is a reasonable interim default. Getting this wrong is the classic "my encodes are louder than my DVDs" bug.
- **Channel layout / downmix behavior:** which acmod (channel configuration) to encode, plus the mix-level fields a 2-channel decoder uses when downmixing our 5.1 (`center_mixlev`, `surround_mixlev`, Lt/Rt-vs-Lo/Ro preference). FFmpeg's defaults are sane; expose under "advanced".
- **DRC (dynamic range compression):** AC-3 can carry per-block gain words that let decoders tame loud passages ("Film Standard", "Music Light", etc.). Our research found no option in FFmpeg's native encoder to generate these (a real gap versus professional Dolby encoders); this specific point was not independently re-verified, so confirm against `ffmpeg -h encoder=ac3` during Milestone 1. For home use, "no DRC" is acceptable — but document it.
- **Bit rate and sample rate defaults:** 48 kHz always; 192–224 kbit/s for stereo, 448 kbit/s for 5.1 (DVD-grade), 640 kbit/s as "max quality". Only ever offer the 19 legal rates, filtered by channel count.

## 3. Legal landscape (as of August 2026)

**Patents — AC-3 is free.** The last US patents covering core AC-3 (US 6,449,368 and US 5,890,106) expired in March 2017; Google Patents shows both "Expired — Lifetime", and Dolby's own SEC filings said its Dolby Digital patents expire "between 2008 and 2017". Encoding and shipping AC-3 is patent-royalty-free. One nuance our verification flagged: the claim that *no non-US patent outlived the US ones* rests on community analysis rather than an authoritative worldwide patent search — but nine years of unchallenged AC-3 shipping by patent-conservative distributors (Fedora ships AC-3 in its fully-free FFmpeg build today) makes the practical risk negligible. (A researcher's claim that Fedora's free FFmpeg build shipped AC-3 "since 2017" was refuted: `ffmpeg-free` only entered Fedora in 2022; 2017 was AC-3 via other packages.)

**E-AC-3 — probably free since January 2026, not yet settled.** The last Dolby Digital Plus patent (US 7,516,064) shows an adjusted expiration of 2026-01-30 on Google Patents, and Phoronix reported the family as likely expired. This is months old, community-verified rather than lawyer-confirmed, and Fedora was still awaiting legal confirmation before enabling E-AC-3. **Treat E-AC-3 as "likely clear, re-verify before shipping".** TrueHD, Atmos, and AC-4 remain firmly patented — do not touch.

**Trademarks — never expire.** "Dolby", "Dolby Digital", and the double-D logo are active registered trademarks. Branding a product with them requires a signed agreement with Dolby. What *is* allowed is nominative fair use: truthful, non-misleading references such as "produces AC-3 streams playable on Dolby® Digital compatible receivers", with no logos. Concretely: the working folder name `dolbydigitalconverter` is fine privately, but the released app needs a different name built around "AC-3", plus a footnote that Dolby Digital is a trademark of Dolby Laboratories and the software is unaffiliated. The same applies to any future live-encoding feature — describe the function; do not call it "Dolby Digital Live".

**FFmpeg LGPL obligations.** FFmpeg's default build is LGPL-2.1+ (the "Lesser GPL" — a license that lets proprietary/differently-licensed apps link the library under conditions). The native AC-3 encoder is plain LGPL libavcodec code; no GPL component is involved as long as we never enable vcpkg's `gpl`/`nonfree` features. When distributing the app:

- Link FFmpeg **dynamically** (ship its DLLs beside the EXE — the vcpkg default does exactly this). FFmpeg's official compliance checklist flatly says to use dynamic linking; note that the widely repeated "static linking is fine if you ship your object files for relinking" rule comes from the LGPL 2.1 §6(a) text itself, *not* from FFmpeg's guidance (our verification refuted the attribution). Practically: just link dynamically.
- Include the LGPL-2.1 text and FFmpeg copyright notices, credit FFmpeg in the About/README, and publish the exact FFmpeg source you built from (attach the source tarball, or a link, to each release).
- Your own application code stays under whatever license you choose.

## 4. Encoding library choice

**Recommendation: FFmpeg libavcodec's native `ac3` (floating-point) encoder, installed via vcpkg.** Verified facts: it is part of core avcodec (no feature flag, no external codec library), validates every format constraint for you, fixes `frame_size` at 1536, accepts only planar float input, handles the AC-3 channel reorder internally, and encodes ~300× faster than real time single-threaded (~0.1 ms CPU per 32 ms frame, measured locally on FFmpeg 8.0.1).

Alternatives considered and rejected:

- **Aften** — a standalone AC-3 encoder forked *from* FFmpeg's; last release 2007, not in vcpkg, long since surpassed by its parent. Dead end.
- **`ac3_fixed`** — FFmpeg's fixed-point sibling, meant for FPU-less embedded chips; lower quality at a given bitrate on a desktop. Ignore.
- **Dolby-certified encoders** (MainConcept's Dolby-approved FFmpeg plugins, Dolby Media Encoder, DP600 hardware) — approval-gated, enterprise-priced, impractical for this project. MainConcept claims (with obvious vendor bias) that FFmpeg's quality is much lower than Dolby's libraries; at our generous target bitrates the native encoder is widely used in production (it is what Plex/Kodi emit) and receivers accept it universally.
- **Writing our own** — the spec is free and readable, but a bit-allocation model plus MDCT plus framing is months of work to reach parity with a library we get for free. Educational option only.

## 5. Getting audio in

**MVP — file input via libavformat/libavcodec.** Since FFmpeg is already our encoder, using it for input adds zero dependencies and decodes essentially anything: WAV, FLAC, MP3, AAC/M4A, even audio tracks inside video files. `avformat_open_input()` also accepts URLs (RTSP/HTTP/HLS/SRT), so network input later is free. Rejected for MVP: **libsndfile** (fine library, but a subset of FFmpeg's formats and another LGPL dependency to track) and **dr_wav** (nice public-domain single-header WAV reader — only worth it for a pre-FFmpeg smoke test). Also add a synthetic tone-generator source behind the same interface so the encoder can be exercised with no file at all.

**Later — live capture via WASAPI**, Windows' modern audio API. Verified landscape:

- **Shared mode** is the default; the OS mixes everything and hands you 32-bit float at the device's mixer rate. **Loopback capture** (`AUDCLNT_STREAMFLAGS_LOOPBACK` on a render endpoint) records "whatever the PC is playing" — shared-mode-only, event-driven since Windows 10 1703. One confirmed gotcha: loopback delivers **no packets while nothing is playing** (driver-dependent, but the common case), so a real-time encoder must synthesize silence against a wall-clock sample counter to keep its 48 kHz output continuous.
- **Wrapper choice: miniaudio** (vcpkg port `miniaudio`, 0.11.25, actively maintained, public-domain/MIT-0). It is the only wrapper whose vcpkg-installed version does both device capture and WASAPI loopback out of the box. PortAudio's vcpkg port pins a 2021 build that predates its loopback support (verified against the pinned commit); RtAudio, libsoundio, and SDL3 have no loopback at all.
- **Per-application capture** (record just one program) exists via `ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (Windows 10 20348+ / all Windows 11). No wrapper exposes it yet, so it is the one place hand-written WASAPI code is genuinely warranted — a small, isolated, later module. Note `GetMixFormat` returns E_NOTIMPL on that path; you specify the format yourself.

## 6. Getting AC-3 out

Three deliverables, in increasing difficulty:

1. **Raw `.ac3` file (MVP).** Encoder packets *are* complete syncframes; `fwrite` them back-to-back and you have a valid stream playable in VLC, ffplay, and foobar2000. Verified locally: 10 s of 5.1 @ 640 kbit/s = exactly 313 × 2560 bytes, first bytes `0B 77`.
2. **IEC 61937 ("S/PDIF") passthrough (the flagship milestone).** AV receivers accept compressed audio disguised as 16-bit stereo PCM at 48 kHz: each AC-3 frame is wrapped in a "burst" — preamble words Pa `0xF872`, Pb `0x4E1F`, Pc (data type 1 = AC-3), Pd (payload length in bits) — then zero-padded to 6144 bytes, giving a constant 1.536 Mbit/s carrier regardless of AC-3 bitrate. FFmpeg's `spdif` muxer implements this and was verified byte-for-byte locally. Delivery on Windows requires **WASAPI exclusive mode** with a `WAVEFORMATEXTENSIBLE_IEC61937` format (SubFormat GUID `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL`, `00000092-0000-0010-8000-00aa00389b71`), because exclusive mode bypasses the Windows mixer — any volume scaling or resampling corrupts the bursts into static. Kodi's `AESinkWASAPI.cpp` is the best open reference; no portable audio library offers this, so plan on ~300 lines of direct WASAPI. Code defensively: users can disable exclusive access per device, and `IsFormatSupported` can succeed while `Initialize` fails on some drivers. A great intermediate validation trick: wrap the IEC 61937 stream in a plain WAV header and play it bit-exactly through the S/PDIF/HDMI endpoint — if the receiver lights up "Dolby Digital", the packer is correct before any WASAPI sink code exists.
3. **Containers.** libavformat muxes AC-3 into MKV, MP4, MPEG-TS, and WAV via stream copy (all four verified locally). ~30 lines each once the pipeline exists.

## 7. Toolchain plan

**C++23 on MSVC.** As of August 2026 there is still **no `/std:c++23` switch**; C++23 is enabled via `/std:c++23preview` (VS 2022 17.13+) or `/std:c++latest`. Microsoft has announced `/std:c++23` for MSVC Build Tools 14.52 previews (announced plan — not yet verified as shipped). Important verified subtlety: `set(CMAKE_CXX_STANDARD 23)` makes CMake emit `/std:c++latest` on MSVC — which also opts you into experimental C++26 material. Accept that (most projects do) or inject `/std:c++23preview` via `target_compile_options`. Useful C++23 library goodies already solid in MSVC: `std::expected` (ideal for pipeline error handling), `std::print`, `ranges::to`. Do **not** build around `import std` — still unsupported in C++23 mode.

**vcpkg.** Manifest mode with a pinned `builtin-baseline` (add it via `vcpkg x-update-baseline --add-initial-baseline`). Verified: the `ffmpeg` port is **8.1.2#3**, default features include avcodec/avformat/swresample, and the portfile passes `--enable-gpl` only if you opt into the `gpl` feature — the default build is pure LGPL with the ac3 encoder compiled in. Recommended dependency line: `ffmpeg` with `default-features: false, features: [avcodec, avformat, swresample]` (drops avfilter/avdevice/swscale to cut build time). Triplet: **`x64-windows`** (dynamic) — the CI-tested default and the LGPL-friendly choice; `VCPKG_APPLOCAL_DEPS` copies the DLLs next to the EXE automatically. Expect the first FFmpeg build to take ~20–40 minutes (vcpkg has no public binary cache); afterwards the local cache makes it minutes. In CI, configure `VCPKG_BINARY_SOURCES` with the GitHub Actions cache.

**CMake.** Presets-based: toolchain file `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` in `cacheVariables`; Ninja presets for the daily loop (run from a Developer PowerShell, or let VS/VS Code auto-load vcvars), optional Visual Studio generator preset for `.sln` debugging (the "Visual Studio 18 2026" generator is in upstream CMake 4.2; VS 2026 bundles a backported one). Consume FFmpeg with `find_package(FFMPEG REQUIRED)` and the `FFMPEG_INCLUDE_DIRS`/`FFMPEG_LIBRARY_DIRS`/`FFMPEG_LIBRARIES` *variables* (this port provides no imported targets), with `extern "C"` around the FFmpeg headers.

**API version note.** Code must use the `AVChannelLayout` (`ch_layout`) API exclusively. One researcher attributed the old bitmask API's removal to FFmpeg 8; verification refuted this — it was removed in **FFmpeg 7.0** (April 2024). The consequence for us is identical: against vcpkg's FFmpeg 8.x, only the new API exists.

## 8. Proposed architecture

```
[Source] ──► [Conditioner] ──► [FIFO] ──► [AC-3 Encoder] ──► [Sink]
 file/URL     libswresample     1536-      libavcodec "ac3"    .ac3 file
 tone gen     → fltp, 48 kHz,   sample                         container mux
 capture      → target layout   frames                         IEC61937+WASAPI
```

- **`IAudioSource`** implementations: FileSource (libavformat), ToneSource, DeviceSource / LoopbackSource (miniaudio), later NetSource and ProcessLoopbackSource. Each yields float32 frames plus a declared rate/layout.
- **Conditioner:** one `SwrContext` (libswresample) is the single canonical converter for every path — interleaved anything in, planar float (`AV_SAMPLE_FMT_FLTP`) at 48 kHz in the target layout out. Planar means one array per channel — writing interleaved data into plane 0 produces garbled audio with no error, a classic first bug.
- **FIFO:** an `AVAudioFifo` re-chunks arbitrary-sized conditioner output into exactly-1536-sample frames, which the encoder requires for every frame except the last (`AV_CODEC_CAP_SMALL_LAST_FRAME`). Set `frame->pts` in sample counts (time base 1/48000). Flush with a NULL frame at EOF (`AV_CODEC_CAP_DELAY`) or the stream's tail is silently lost.
- **Sinks** behind one interface: FileSink (fwrite), MuxSink (libavformat), SpdifSink (spdif muxer → WASAPI exclusive, MMCSS-boosted feeder thread).
- **Threading and the 32 ms frame:** for file conversion, a single thread is fine. For live capture: capture callback thread pushes into a lock-free ring buffer; an encode thread pops 1536-sample chunks; the WASAPI event-driven render thread consumes 6144-byte bursts. All buffer sizes in multiples of 1536 samples. Latency floor is ~one frame plus 256 samples of encoder lookahead (~37 ms at 48 kHz); budget **~70–100 ms end-to-end** including capture and receiver decode — fine for movies, marginal for games. Encoding CPU is negligible.
- Wrap every FFmpeg object (`AVCodecContext`, `AVFrame`, `AVPacket`, `SwrContext`) in RAII holders; surface errors as `std::expected`.

## 9. Suggested roadmap

- **M0 — Scaffold.** Repo, `vcpkg.json` (pinned baseline), `CMakePresets.json`, CI stub, hello-world that prints `avcodec_version()`. Proves the 20–40 min FFmpeg build once.
- **M1 — WAV → .ac3 CLI.** Full pipeline on files. Acceptance: `ffprobe` reports `ac3, 48000 Hz, 5.1(side), 448 kb/s`; `ffmpeg -v error -i out.ac3 -f null -` runs clean; plays in VLC; file size = frames × expected frame size exactly.
- **M2 — Any input, correct metadata.** Arbitrary input formats via libavformat + swresample; loudness measurement (libebur128) driving dialnorm; advanced options (bitrate, mix levels).
- **M3 — IEC 61937 packer + "spdif-WAV" validation.** Wrap frames in bursts (reuse the `spdif` muxer), emit as a playable WAV, confirm a real receiver lights "Dolby Digital" via a bit-exact player. No WASAPI code yet.
- **M4 — Live passthrough sink.** WASAPI exclusive-mode IEC 61937 sink modeled on Kodi's. Fallback paths for exclusive-mode-disabled and flaky drivers.
- **M5 — Live capture.** miniaudio loopback → encoder → passthrough: the software Dolby-Digital-Live moment. Handle the silence-gap gotcha.
- **Fun extensions:** container muxing UI, per-application capture (raw WASAPI process loopback), network input, E-AC-3 output (after re-verifying patent clearance), a real GUI.

## 10. Open questions for the user

1. **Product name.** `dolbydigitalconverter` cannot ship. Pick an AC-3-centric name (e.g., "AC3Forge").
2. **dialnorm policy.** Measure loudness per encode (slower, correct), fixed −27 (matches commercial content), or FFmpeg's −31 (no attenuation)?
3. **CLI-first or GUI-first?** This brief assumes CLI-first.
4. **Primary live use case** — movies/desktop audio (latency-tolerant) or games (latency-sensitive)? Affects buffer sizing.
5. **Is E-AC-3 wanted** enough to track its patent confirmation?
6. **Distribution plans** (GitHub releases? a store?) — determines how the LGPL source-offer is published.

## Glossary

- **AC-3 / Dolby Digital:** the lossy 5.1 audio codec defined by ATSC A/52; "Dolby Digital" is its trademarked marketing name.
- **PCM:** raw uncompressed sample-by-sample digital audio.
- **Syncframe:** AC-3's self-contained unit — 1536 samples/channel, 32 ms at 48 kHz.
- **MDCT:** the overlapping time-to-frequency transform at the codec's heart.
- **LFE:** low-frequency effects channel (the ".1").
- **acmod:** the bitstream field selecting the channel configuration.
- **dialnorm:** metadata declaring dialogue loudness so decoders can level-match programs.
- **DRC:** decoder-side dynamic range compression driven by encoder-written gain words.
- **Planar vs interleaved:** planar stores each channel in its own array (`AV_SAMPLE_FMT_FLTP`); interleaved alternates samples channel by channel.
- **S/PDIF:** the consumer digital audio link (optical/coax); HDMI carries the same audio framing.
- **IEC 61937:** the standard for disguising compressed frames as PCM over S/PDIF/HDMI ("bitstreaming"/"passthrough").
- **WASAPI:** Windows' modern audio API; *exclusive mode* bypasses the OS mixer for bit-exact output; *loopback* records what the system is playing.
- **LGPL:** the Lesser GPL; lets non-GPL apps link a library if it stays replaceable (dynamic linking) and its source is offered.
- **vcpkg triplet:** vcpkg's target descriptor (`x64-windows` = 64-bit, dynamic linking).
- **E-AC-3 (Dolby Digital Plus):** AC-3's non-backward-compatible successor.

## Sources

- [ATSC A/52:2018 — AC-3/E-AC-3 standard (free PDF)](https://www.atsc.org/wp-content/uploads/2021/04/A52-2018.pdf)
- [ETSI TS 102 366 V1.4.1 (free PDF twin of A/52)](https://www.etsi.org/deliver/etsi_ts/102300_102399/102366/01.04.01_60/ts_102366v010401p.pdf)
- [ATSC A/53 Part 5:2014 — broadcast constraints](https://www.atsc.org/wp-content/uploads/2015/03/A53-Part-5-2014.pdf)
- [RFC 4184 — RTP payload for AC-3](https://datatracker.ietf.org/doc/html/rfc4184)
- [Dolby Metadata Guide (dialnorm/DRC)](https://developer.dolby.com/globalassets/professional/documents/dolby-metadata-guide.pdf)
- [FFmpeg codecs documentation (ac3 encoder options)](https://ffmpeg.org/ffmpeg-codecs.html)
- [FFmpeg ac3enc.c (native encoder source)](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/ac3enc.c)
- [FFmpeg ac3tab.c (rate/bitrate/channel tables)](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/ac3tab.c)
- [FFmpeg spdifenc.c (IEC 61937 muxer)](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdifenc.c)
- [FFmpeg encode_audio.c example](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/doc/examples/encode_audio.c)
- [FFmpeg swresample.h](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libswresample/swresample.h)
- [FFmpeg legal/LGPL checklist](https://ffmpeg.org/legal.html)
- [LGPL 2.1 text](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- [Google Patents: US5890106](https://patents.google.com/patent/US5890106A/en) · [US6449368](https://patents.google.com/patent/US6449368B1/en) · [US7516064 (E-AC-3)](https://patents.google.com/patent/US7516064B2/en)
- [Phoronix: last E-AC-3 patents may be expired (2026)](https://www.phoronix.com/news/Dolby-Digital-Plus-E-AC3-2026)
- [Wikipedia: Dolby Digital](https://en.wikipedia.org/wiki/Dolby_Digital) · [Dolby Digital Live](https://en.wikipedia.org/wiki/Dolby_Digital_Live) · [Dialnorm](https://en.wikipedia.org/wiki/Dialnorm)
- [Dolby logo-use / licensing](https://professional.dolby.com/licensing/logo-use-agreement/) · [Dolby Terms of Use](https://www.dolby.com/about/legal/terms-of-use/)
- [Microsoft: Representing IEC 61937 formats](https://learn.microsoft.com/en-us/windows/win32/coreaudio/representing-formats-for-iec-61937-transmissions) · [Exclusive-mode streams](https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams) · [Loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) · [GetMixFormat](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat) · [Device formats](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats) · [Process loopback activation](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ne-audioclientactivationparams-audioclient_activation_type) · [ApplicationLoopbackAudio sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [Kodi AESinkWASAPI.cpp (passthrough reference)](https://github.com/xbmc/xbmc/blob/master/xbmc/cores/AudioEngine/Sinks/AESinkWASAPI.cpp)
- [vcpkg ffmpeg port manifest](https://github.com/microsoft/vcpkg/blob/master/ports/ffmpeg/vcpkg.json) · [portfile](https://raw.githubusercontent.com/microsoft/vcpkg/master/ports/ffmpeg/portfile.cmake) · [usage](https://raw.githubusercontent.com/microsoft/vcpkg/master/ports/ffmpeg/usage)
- [vcpkg CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration) · [triplets](https://learn.microsoft.com/en-us/vcpkg/concepts/triplets) · [x-update-baseline](https://learn.microsoft.com/en-us/vcpkg/commands/update-baseline) · [binary caching](https://learn.microsoft.com/en-us/vcpkg/users/binarycaching)
- [MSVC /std reference](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version?view=msvc-170) · [C++23 in MSVC 14.51](https://devblogs.microsoft.com/cppblog/c23-support-in-msvc-build-tools-14-51/) · [MSVC conformance table](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance?view=msvc-170) · [CMake presets in VS](https://learn.microsoft.com/en-us/cpp/build/cmake-presets-vs?view=msvc-170) · [CMake MSVC-CXX.cmake](https://github.com/Kitware/CMake/blob/master/Modules/Compiler/MSVC-CXX.cmake)
- [miniaudio manual](https://miniaud.io/docs/manual/index.html) · [miniaudio releases](https://github.com/mackron/miniaudio/releases) · [PortAudio loopback-silence issue #935](https://github.com/PortAudio/portaudio/issues/935)
- [Aften (historical)](https://aften.sourceforge.net/) · [MainConcept Dolby plugins for FFmpeg](https://www.mainconcept.com/ffmpeg-dolby)
- [Fedora ffmpeg-free package](https://packages.fedoraproject.org/pkgs/ffmpeg/ffmpeg-free/) · [Phoronix: Fedora codec enablement 2017](https://www.phoronix.com/news/Fedora-FDK-AAC)
- [AVS Forum: last AC-3 patent expiry thread](https://www.avsforum.com/threads/dolbys-last-patent-related-to-ac-3-expired-today-3-20-17.2789001/) · [Hacker News discussion](https://news.ycombinator.com/item?id=13910925) · [Gigazine report](https://gigazine.net/gsc_news/en/20170321-ac-3-patent-expired/) · [EFF via Free-To-Air America](https://freetoairamerica.wordpress.com/2017/03/20/electronic-frontier-foundation-the-patent-on-dolby-digital-ac-3-has-just-expired/)
