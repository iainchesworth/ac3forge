#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/meta/loudness.hpp"

// Momentary/short-term loudness, Loudness Range and true peak - the R128
// metering roadmap item C1 adds on top of the pre-existing integrated_lkfs()
// (whose own calibration/surround-weighting/silence tests already live in
// test_drc.cpp; this file only covers the new surface). All of it needs real,
// multi-second, non-silent content: a 400 ms/3 s window, EBU Tech 3342's
// cascaded gate and an oversampled peak are all defined over real programme
// material, not frame 0 or digital silence.

using ac3::meta::LoudnessMeter;

namespace {

// Same generator shape as test_drc.cpp's calibration test: a single tone at
// a chosen peak amplitude, long enough to fill whichever window is under
// test many times over.
std::vector<float> make_tone(double seconds, double freq_hz, double amplitude,
                              double sample_rate = 48000.0) {
    const auto n = static_cast<std::size_t>(seconds * sample_rate);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * freq_hz * static_cast<double>(i) /
                                  sample_rate));
    }
    return out;
}

double dbfs(double amplitude) { return 20.0 * std::log10(amplitude); }

}  // namespace

// --- momentary / short-term -------------------------------------------------

TEST_CASE("momentary and short-term loudness are undefined before their window elapses",
          "[loudness]") {
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};

    // 200 ms: neither the 400 ms momentary window nor the 3 s short-term one
    // has elapsed yet.
    const auto tone_200ms = make_tone(0.2, 1000.0, 0.1);
    const std::array<std::span<const float>, 2> channels_200ms = {tone_200ms, tone_200ms};
    meter.push(channels_200ms);
    CHECK_FALSE(meter.momentary_lkfs().has_value());
    CHECK_FALSE(meter.short_term_lkfs().has_value());

    // Another 300 ms (500 ms total): momentary's 400 ms has now elapsed,
    // short-term's 3 s has not. If momentary read the short-term window's
    // step count (or vice versa), one of these two checks would fail.
    const auto tone_300ms = make_tone(0.3, 1000.0, 0.1);
    const std::array<std::span<const float>, 2> channels_300ms = {tone_300ms, tone_300ms};
    meter.push(channels_300ms);
    CHECK(meter.momentary_lkfs().has_value());
    CHECK_FALSE(meter.short_term_lkfs().has_value());
}

TEST_CASE("momentary and short-term loudness hit the same BS.1770 calibration point "
          "integrated loudness does, on steady content",
          "[loudness]") {
    // BS.1770/EBU Tech 3341: a 1 kHz sine at -20 dBFS reads -20.0 LKFS. On a
    // signal with no dynamics at all, the un-gated 400 ms and 3 s windows
    // should read the same thing the whole-programme gated measurement does
    // - there is nothing for gating or windowing to disagree about.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto tone = make_tone(10.0, 1000.0, 0.1);
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);

    const auto integrated = meter.integrated_lkfs();
    const auto momentary = meter.momentary_lkfs();
    const auto short_term = meter.short_term_lkfs();
    REQUIRE(integrated.has_value());
    REQUIRE(momentary.has_value());
    REQUIRE(short_term.has_value());
    CHECK(*integrated == Catch::Approx(-20.0).margin(0.1));
    CHECK(*momentary == Catch::Approx(-20.0).margin(0.1));
    CHECK(*short_term == Catch::Approx(-20.0).margin(0.1));
}

TEST_CASE("momentary loudness tracks a level step within one window, short-term lags",
          "[loudness]") {
    // Loud for 2 s, then quiet (-40 dBFS, well below the loud segment) for
    // another 2 s. Read right at the end: momentary's 400 ms window is
    // entirely inside the quiet tail, so it should read close to -40 LKFS.
    // Short-term's 3 s window still spans 1 s of the loud segment, so it
    // must sit measurably ABOVE momentary - if short_term_lkfs() were
    // accidentally wired to the same 4-step window as momentary, this
    // difference would vanish.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto loud = make_tone(2.0, 1000.0, 0.1);     // -20 dBFS
    const auto quiet = make_tone(2.0, 1000.0, 0.01);   // -40 dBFS
    const std::array<std::span<const float>, 2> loud_channels = {loud, loud};
    const std::array<std::span<const float>, 2> quiet_channels = {quiet, quiet};
    meter.push(loud_channels);
    meter.push(quiet_channels);

    const auto momentary = meter.momentary_lkfs();
    const auto short_term = meter.short_term_lkfs();
    REQUIRE(momentary.has_value());
    REQUIRE(short_term.has_value());
    CHECK(*momentary == Catch::Approx(-40.0).margin(0.5));
    CHECK(*short_term > *momentary + 3.0);
}

// --- Loudness Range ----------------------------------------------------------

TEST_CASE("Loudness Range matches EBU Tech 3342 Table 1's own minimum-requirements test #3",
          "[loudness]") {
    // Tech 3342 (2023) Table 1, test case 3: "As #1 [stereo sine wave,
    // 1000 Hz, in phase, 20 s per level], with the 2 tones at -40.0 dBFS and
    // -20.0 dBFS respectively" -> expected response LRA = 20 +/-1 LU. This is
    // the standard's own published compliance vector, not a value derived
    // from this implementation.
    //
    // Deliberately test #3 (20 dB gap) rather than #1 (10 dB gap): with only
    // a 10 dB gap, the quieter segment's short-term loudness sits close
    // enough to the absolute-gated mean (about -7 LU below it, worked out
    // from the two segments' relative power) that it clears Tech 3342's
    // real -20 LU relative gate AND a wrongly-implemented -10 LU one (i.e.
    // integrated_lkfs()'s own relative-gate threshold) equally - a #1-shaped
    // test was tried first and kept passing even with the relative gate
    // deliberately mistyped to -10 LU, which is exactly the class of false
    // pass this project's validation rule warns about. At a 20 dB gap the
    // quiet segment sits roughly -17 LU below the mean: inside the real
    // -20 LU gate (so it counts, giving the expected ~20 LU spread) but
    // outside a -10 LU one (so a mis-gated implementation drops it and
    // collapses LRA toward 0) - confirmed by reintroducing that exact typo
    // and watching this test fail before reverting it.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto quiet = make_tone(20.0, 1000.0, 0.01);          // -40.0 dBFS peak
    const auto loud = make_tone(20.0, 1000.0, 0.1);            // -20.0 dBFS peak
    const std::array<std::span<const float>, 2> quiet_channels = {quiet, quiet};
    const std::array<std::span<const float>, 2> loud_channels = {loud, loud};
    meter.push(quiet_channels);
    meter.push(loud_channels);

    const auto lra = meter.loudness_range();
    REQUIRE(lra.has_value());
    CHECK(*lra == Catch::Approx(20.0).margin(1.0));
}

TEST_CASE("Loudness Range is undefined for a programme with no short-term history yet",
          "[loudness]") {
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto tone = make_tone(1.0, 1000.0, 0.1);  // well under the 3 s short-term window
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);
    CHECK_FALSE(meter.loudness_range().has_value());
}

// --- true peak ----------------------------------------------------------------

TEST_CASE("true peak matches sample peak within a fraction of a dB for a slow, "
          "well-sampled tone",
          "[loudness][true-peak]") {
    // A 100 Hz tone at 48 kHz puts ~480 samples per cycle, so some sample
    // always lands within a small fraction of a degree of the true peak -
    // oversampling should find essentially nothing extra here. This is the
    // control for the inter-sample-peak test below: it proves the
    // oversampler does not just unconditionally read high.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto tone = make_tone(1.0, 100.0, 0.5);
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);

    const auto true_peak = meter.true_peak_dbtp();
    REQUIRE(true_peak.has_value());
    CHECK(*true_peak == Catch::Approx(dbfs(0.5)).margin(0.05));
}

TEST_CASE("true peak finds the inter-sample peak a sample-peak reading misses",
          "[loudness][true-peak]") {
    // The classic construction: a full-scale sine at exactly Fs/4 with a
    // 45-degree phase offset. At integer sample n, sin(pi*n/2 + pi/4) is
    // +-1/sqrt(2) for EVERY sample (period-4 pattern 0.7071, 0.7071,
    // -0.7071, -0.7071, ...), so the sample-peak reads a constant
    // -3.01 dBFS no matter how long the tone runs. The continuous
    // waveform's actual peaks (amplitude 1.0, i.e. 0 dBTP) fall exactly
    // halfway between samples (at n = 0.5 + 4k), landing precisely on the
    // 4x-oversampled grid's midpoint phase - the textbook case a
    // sample-peak meter cannot see at all and an oversampled true-peak
    // meter is built to catch.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq = kSampleRate / 4.0;
    constexpr std::size_t kSamples = 4000;  // 1000 full periods of the pattern
    std::vector<float> tone(kSamples);
    double max_sample = 0.0;
    for (std::size_t n = 0; n < kSamples; ++n) {
        const double v = std::sin(2.0 * std::numbers::pi * kFreq * static_cast<double>(n) /
                                       kSampleRate +
                                   std::numbers::pi / 4.0);
        tone[n] = static_cast<float>(v);
        max_sample = std::max(max_sample, std::abs(v));
    }
    // Confirms the construction: every sample sits at 1/sqrt(2), not 1.0.
    REQUIRE(max_sample == Catch::Approx(1.0 / std::numbers::sqrt2).margin(1e-9));

    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);

    const auto true_peak = meter.true_peak_dbtp();
    REQUIRE(true_peak.has_value());
    const double sample_peak_dbtp = dbfs(max_sample);  // ~ -3.01 dBFS

    // The oversampled reading must sit clearly above what sample-peak alone
    // would report (BS.1770-4 Annex 2's whole reason to exist), and land
    // close to the true analytic answer of 0 dBTP - Annex 2's own worst-case
    // under-read table puts a 4x-oversampled reading within ~0.7 dB of the
    // true value even at Nyquist itself; Fs/4 is comfortably inside that.
    CHECK(*true_peak > sample_peak_dbtp + 1.5);
    CHECK(*true_peak == Catch::Approx(0.0).margin(1.0));
}

TEST_CASE("true peak includes the LFE channel that integrated loudness excludes",
          "[loudness][true-peak]") {
    // header comment's own design point: true peak is about physical
    // overload, so unlike every weighted-loudness measure it must not drop
    // LFE. Silence everywhere except a full-scale LFE tone reproduces
    // test_drc.cpp's "lfe_only" pattern, but checks the opposite property.
    const auto tone = make_tone(1.0, 60.0, 0.9);
    const std::vector<float> silence(tone.size(), 0.0f);
    // 3/2 + LFE coded order is L, C, R, Ls, Rs, LFE.
    const std::array<std::span<const float>, 6> lfe_only = {silence, silence, silence,
                                                              silence, silence, tone};
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k3_2, true};
    meter.push(lfe_only);

    CHECK_FALSE(meter.integrated_lkfs().has_value());  // unchanged existing behaviour
    const auto true_peak = meter.true_peak_dbtp();
    REQUIRE(true_peak.has_value());
    CHECK(*true_peak == Catch::Approx(dbfs(0.9)).margin(0.1));
}

TEST_CASE("true peak is undefined before any sample is pushed", "[loudness][true-peak]") {
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    CHECK_FALSE(meter.true_peak_dbtp().has_value());
}
