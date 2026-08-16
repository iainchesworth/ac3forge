# Metadata

`ac3/meta/`. Everything here is optional; leaving it out produces a stream bit-identical to
one from before the metadata layer existed. An AV receiver reads exactly these bits to set
level, compress dynamics and fold down to fewer speakers than the stream carries.

`dialnorm` cannot be derived from the frame being encoded — BS.1770 gating is defined over the
whole programme — so measure first and configure second:

```cpp
// Weights follow BS.1770 Table 3: unity front, +1.5 dB surrounds, LFE
// excluded outright.
ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, kAcmod, kLfe};
for (int frame = 0; frame < kFrames; ++frame) {
    fill(pcm, frame);
    meter.push(views);
}

// nullopt until at least one 400 ms block has passed the absolute gate:
// silence has no meaningful loudness, and inventing one would put a wrong
// dialnorm on the stream.
const auto lkfs = meter.integrated_lkfs();
const int dialnorm = lkfs ? ac3::meta::dialnorm_from_lkfs(*lkfs) : 31;
```

The same pass also gives the rest of an R128 meter, off the same K-weighted, channel-summed
signal `integrated_lkfs()` already builds:

```cpp
// Momentary (400 ms) and short-term (3 s) loudness: the same un-gated block
// power integrated_lkfs() gates internally, read directly instead. nullopt
// until the respective window has elapsed.
const auto momentary = meter.momentary_lkfs();
const auto short_term = meter.short_term_lkfs();

// EBU Tech 3342 §3.1 Loudness Range: the 95th minus 10th percentile of
// short-term loudness values, gated at -70 LUFS absolute and -20 LU relative
// to their own gated mean - a different relative gate to integrated
// loudness's -10 LU, and over a different (short-term) population.
const auto lra = meter.loudness_range();

// ITU-R BS.1770-4 Annex 2: the highest sample found in a 4x-oversampled
// reconstruction of every pushed channel, LFE included - true peak is about
// physical overload headroom, not perceived loudness, so it is the one
// measure here that does not drop the LFE channel or apply the surround
// weighting.
const auto true_peak = meter.true_peak_dbtp();
```

```cpp
// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
// state (PREfast's C6262).
auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
    .bitrate_kbps = 448,
    .dialnorm = dialnorm,
    .acmod = kAcmod,
    .lfe = kLfe,
    // §7.7.1. A/52 fixes the wire format and the intent but never the
    // curve, so the profile is this project's reading of it.
    .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
    // §7.7.2, independent of drc: the two answer different questions, so a
    // stream may carry either, both or neither.
    .heavy = ac3::meta::HeavyConfig{.dialogue_target_dbfs = -20.0,
                                    .peak_ceiling_dbfs = -0.5},
    // Tables 5.9 / 5.10. These always define the §7.8 downmix, whatever
    // acmod is, so the heavy-compression peak detector consults them too.
    .cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB,
    .surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB,
});
```

Full program: [`examples/metadata.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/metadata.cpp).

Note `meta::Profile` is the curve and `meta::ProfileId` the name of one; `meta::profile(id)`
converts. The five ids are `kFilmStandard`, `kFilmLight`, `kMusicStandard`, `kMusicLight` and
`kSpeech`.

The K-weighting in `ac3/meta/loudness.hpp` is designed analytically rather than tabulated, so
44.1 and 32 kHz work too. Calibration: a 1 kHz tone at −20 dBFS reads −19.99 LKFS, matching
FFmpeg's `ebur128` to 0.01 LU.

`ac3/meta/mixing.hpp` holds the downmix levels — `CentreMixLevel` and `SurroundMixLevel` for
AC-3, `MixMetadata` for the E-AC-3 `mixmdate` group. In `MixMetadata`, an absent
`lfemixlevcod` means LFE mixing is *disabled*, which per §E2.3.1.10 is a decision in its own
right and not the same as sending code 31.

`ac3/meta/qc.hpp` is the QC-side counterpart: named loudness/true-peak delivery
gates a decoded stream's measurement can be checked against, mirroring `meta::Profile`/
`meta::ProfileId`'s own shape:

```cpp
const auto preset = ac3::meta::qc_preset(ac3::meta::QcPresetId::kEbuR128S2);
const auto verdict = ac3::meta::evaluate_qc_gate(preset, meter.integrated_lkfs(),
                                                 meter.true_peak_dbtp());
if (!verdict.pass()) { /* ... */ }
```

`QcPresetId` names three delivery specs — `kEbuR128S2`, `kAtscA85`, `kNetflix` — each carrying a
target integrated loudness, a symmetric tolerance (LU) and a true-peak ceiling (dBTP, a one-sided
limit) read from its own primary source (EBU R 128 s2 + EBU R 128, ATSC A/85:2013 §6, and
Netflix's Sound Mix Specifications & Best Practices, respectively — see `qc_preset()`'s own
comment for the exact clause cited per preset). `evaluate_qc_gate()` is the pure comparison:
`QcVerdict::loudness_delta_lu`/`true_peak_margin_dbtp` report signed distance from the target/
ceiling, and `pass()` is both halves together. `ac3cli qc` is this same API driven end to end over
a real, already-encoded file — see [`examples/qc_report.cpp`](https://github.com/iainchesworth/ac3forge/blob/main/examples/qc_report.cpp)
for the library-only version: encode with a dialnorm that does not match the real level, decode,
measure, and see the mismatch and the gate verdicts it produces.

---

See also: [Encoding AC-3](encoding-ac3.md) and [Encoding E-AC-3](encoding-eac3.md) — where
`dialnorm`, `drc`, `heavy` and the mix-level fields are actually consumed.
