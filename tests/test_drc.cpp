#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "golden/drc_goldens.hpp"

namespace {

using ac3::meta::compr_gain;
using ac3::meta::dynrng_gain;

// Real audio, not silence, and with dynamics: a tone whose amplitude steps
// between two levels. Digital silence trips §7.2.2.1.1 and zeroes the whole bit
// allocation, and a steady tone never moves the compressor off its initial
// gain - both hide the class of bug these tests exist to catch.
std::vector<std::vector<float>> stepped_tone(int frames, int channels, double loud,
                                            double quiet, int frames_per_step) {
    std::vector<std::vector<float>> out(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(frames) * ac3::kSamplesPerFrame));
    for (int f = 0; f < frames; ++f) {
        const bool is_loud = (f / frames_per_step) % 2 == 0;
        const double amplitude = is_loud ? loud : quiet;
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const auto index =
                static_cast<std::size_t>(f) * ac3::kSamplesPerFrame + static_cast<std::size_t>(n);
            const double value =
                amplitude * std::sin(2.0 * std::numbers::pi * 440.0 *
                                     static_cast<double>(index) / 48000.0);
            for (int ch = 0; ch < channels; ++ch) {
                out[static_cast<std::size_t>(ch)][index] = static_cast<float>(value);
            }
        }
    }
    return out;
}

std::vector<std::span<const float>> frame_views(
    const std::vector<std::vector<float>>& audio, int frame) {
    std::vector<std::span<const float>> views;
    for (const auto& channel : audio) {
        views.emplace_back(std::span{channel}.subspan(
            static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame, ac3::kSamplesPerFrame));
    }
    return views;
}

double rms_db(std::span<const float> samples) {
    double sum = 0.0;
    for (const float value : samples) {
        sum += static_cast<double>(value) * static_cast<double>(value);
    }
    return 10.0 * std::log10(std::max(sum / static_cast<double>(samples.size()), 1e-30));
}

double peak_db(std::span<const float> samples) {
    double peak = 0.0;
    for (const float value : samples) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    return 20.0 * std::log10(std::max(peak, 1e-30));
}

}  // namespace

// --- the two wire formats --------------------------------------------------

TEST_CASE("dynrng and compr gains match an independent transcription", "[drc]") {
    // tools/drc_ref.py builds these from Table 7.29 / 7.30's arithmetic-shift
    // column; the encoder computes the exponent in closed form from the signed
    // field. Two readings of the same tables, so agreement is evidence about
    // the reading and not just about the arithmetic.
    for (int word = 0; word < 256; ++word) {
        const auto w = static_cast<std::uint8_t>(word);
        CHECK(dynrng_gain(w) == ac3::golden::kDynrngGain[static_cast<std::size_t>(word)]);
        CHECK(compr_gain(w) == ac3::golden::kComprGain[static_cast<std::size_t>(word)]);
    }
}

TEST_CASE("dynrng and compr endpoints are the ones the spec states", "[drc]") {
    // §7.7.1.2: "+23.95 dB, to -24.08 dB", and '0000 0000' is unity.
    CHECK(ac3::meta::to_db(dynrng_gain(0x7F)) == Catch::Approx(23.95).margin(0.01));
    CHECK(ac3::meta::to_db(dynrng_gain(0x80)) == Catch::Approx(-24.08).margin(0.01));
    CHECK(dynrng_gain(ac3::meta::kDynrngUnity) == 1.0);
    // §7.7.2.2: "+47.89 dB, to -48.16 dB" - twice the range, half the
    // resolution (§7.7.2.1).
    CHECK(ac3::meta::to_db(compr_gain(0x7F)) == Catch::Approx(47.89).margin(0.01));
    CHECK(ac3::meta::to_db(compr_gain(0x80)) == Catch::Approx(-48.16).margin(0.01));
    CHECK(compr_gain(ac3::meta::kComprUnity) == 1.0);
}

TEST_CASE("every word round-trips through the quantiser", "[drc]") {
    for (int word = 0; word < 256; ++word) {
        const auto w = static_cast<std::uint8_t>(word);
        CHECK(ac3::meta::encode_dynrng(ac3::meta::to_db(dynrng_gain(w))) == w);
        CHECK(ac3::meta::encode_compr(ac3::meta::to_db(compr_gain(w))) == w);
        // Rounding down must not move an exactly representable value.
        CHECK(ac3::meta::encode_compr_at_most(ac3::meta::to_db(compr_gain(w))) == w);
    }
}

TEST_CASE("the quantiser agrees with an exhaustive nearest-code search", "[drc]") {
    // The encoder splits the double with frexp; the reference tries all 256
    // codes. Cases where two codes tie are excluded at generation time, so any
    // disagreement here is real.
    for (const auto& c : ac3::golden::kDrcQuantCases) {
        CHECK(ac3::meta::encode_dynrng(c.gain_db) == c.dynrng);
        CHECK(ac3::meta::encode_compr(c.gain_db) == c.compr);
    }
}

TEST_CASE("encode_compr_at_most never rounds up past the request", "[drc]") {
    // §7.7.2 promises "an assured upper limit". Nearest-code rounding can
    // exceed a request by half a step (0.14 dB), which is small and still not a
    // ceiling. This is the guarantee, checked across the whole range.
    for (int millibel = -48000; millibel <= 47000; millibel += 7) {
        const double target_db = static_cast<double>(millibel) / 1000.0;
        const double target = std::pow(10.0, target_db / 20.0);
        const auto word = ac3::meta::encode_compr_at_most(target_db);
        REQUIRE(compr_gain(word) <= target * (1.0 + 1e-9));
        // And it is the LARGEST such code: no other word fits under the
        // target with less loss.
        double best = 0.0;
        for (int w = 0; w < 256; ++w) {
            const double gain = compr_gain(static_cast<std::uint8_t>(w));
            if (gain <= target * (1.0 + 1e-9)) {
                best = std::max(best, gain);
            }
        }
        REQUIRE(compr_gain(word) == best);
    }
}

// --- the compression characteristic ---------------------------------------

TEST_CASE("the profile curve is continuous, monotone and unity at dialogue",
          "[drc][profile]") {
    constexpr std::array<ac3::meta::ProfileId, 5> ids = {
        ac3::meta::ProfileId::kFilmStandard, ac3::meta::ProfileId::kFilmLight,
        ac3::meta::ProfileId::kMusicStandard, ac3::meta::ProfileId::kMusicLight,
        ac3::meta::ProfileId::kSpeech,
    };
    for (const auto id : ids) {
        const auto p = ac3::meta::profile(id);
        // A signal at dialogue level keeps its gain (§7.7.1.1).
        CHECK(ac3::meta::static_gain_db(p, p.null_low_db) == 0.0);
        CHECK(ac3::meta::static_gain_db(p, p.null_high_db) == 0.0);
        CHECK(ac3::meta::static_gain_db(p, 0.5 * (p.null_low_db + p.null_high_db)) == 0.0);

        double previous = ac3::meta::static_gain_db(p, -140.0);
        // The boost ceiling, reached well below the boost region.
        CHECK(previous == Catch::Approx(p.max_boost_db));
        for (int tenth = -1400; tenth <= 0; ++tenth) {
            const double level = static_cast<double>(tenth) / 10.0;
            const double gain = ac3::meta::static_gain_db(p, level);
            // Louder never means more gain.
            CHECK(gain <= previous + 1e-12);
            // No step: a jump in a gain curve is audible as a click.
            CHECK(std::abs(gain - previous) < 0.06);
            previous = gain;
        }
        // Above the null band the cut is real, and by full scale it is large.
        CHECK(ac3::meta::static_gain_db(p, 0.0) < -4.0);
    }
}

TEST_CASE("the named profiles land on their published boost edges", "[drc][profile]") {
    // The boost region is stored as a ratio plus a ceiling; its lower edge is
    // therefore derived, and these are the figures the published tables give.
    struct Case {
        ac3::meta::ProfileId id;
        double edge_db;
    };
    for (const auto& c : std::array<Case, 4>{{
             {ac3::meta::ProfileId::kFilmStandard, -43.0},
             {ac3::meta::ProfileId::kFilmLight, -53.0},
             {ac3::meta::ProfileId::kMusicStandard, -55.0},
             {ac3::meta::ProfileId::kMusicLight, -65.0},
         }}) {
        const auto p = ac3::meta::profile(c.id);
        const double edge = p.null_low_db - p.max_boost_db * p.boost_ratio;
        CHECK(edge == Catch::Approx(c.edge_db));
        // At the edge the boost is exactly the ceiling, and just above it the
        // curve has already come off the clamp.
        CHECK(ac3::meta::static_gain_db(p, edge) == Catch::Approx(p.max_boost_db));
        CHECK(ac3::meta::static_gain_db(p, edge + 2.0) < p.max_boost_db);
    }
}

TEST_CASE("the profile ratios are the ones the regions claim", "[drc][profile]") {
    const auto p = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard);
    const auto slope = [&](double level) {
        return (ac3::meta::static_gain_db(p, level + 0.5) -
                ac3::meta::static_gain_db(p, level - 0.5));
    };
    // A 2:1 boost gives -1/2 dB of gain per dB of level; 2:1 cut the same; the
    // 20:1 cut region gives -1/20.
    CHECK(slope(p.null_low_db - 4.0) == Catch::Approx(-0.5));
    CHECK(slope(0.5 * (p.null_high_db + p.early_cut_end_db)) == Catch::Approx(-0.5));
    CHECK(slope(p.early_cut_end_db + 4.0) == Catch::Approx(-0.05));
    // Speech expands more gently on the way up: 5:1 rather than 2:1.
    const auto speech = ac3::meta::profile(ac3::meta::ProfileId::kSpeech);
    const auto speech_slope = [&](double level) {
        return (ac3::meta::static_gain_db(speech, level + 0.5) -
                ac3::meta::static_gain_db(speech, level - 0.5));
    };
    CHECK(speech_slope(speech.null_low_db - 4.0) == Catch::Approx(-0.2));
}

TEST_CASE("the range controller tracks the curve and respects dialnorm", "[drc]") {
    const auto p = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard);
    // A steady level, held long enough for a 1 s release to settle.
    const auto settle = [&](double level, int dialnorm) {
        ac3::meta::RangeController controller{p, ac3::SampleRate::k48000};
        for (int block = 0; block < 2000; ++block) {
            (void)controller.next(level, dialnorm);
        }
        return controller.gain_db();
    };

    // Dialogue level in, unity out: with dialnorm 31 the anchor is -31 dBFS.
    CHECK(settle(-29.0, 31) == Catch::Approx(0.0).margin(0.01));
    // The same audio, but the stream now says dialogue is 7 dB louder than it
    // is - so the encoder must treat the audio as 7 dB quieter and boost it.
    // Getting this shift backwards is invisible on a single dialnorm.
    CHECK(settle(-29.0, 24) > 2.0);
    CHECK(settle(-29.0, 24) == Catch::Approx(ac3::meta::static_gain_db(p, -36.0)).margin(0.01));
    // Loud in, cut out.
    CHECK(settle(-6.0, 31) == Catch::Approx(ac3::meta::static_gain_db(p, -6.0)).margin(0.01));

    // Attack (gain going down) must be faster than release (gain going up), or
    // a transient escapes and every gap pumps.
    ac3::meta::RangeController attack{p, ac3::SampleRate::k48000};
    (void)attack.next(-29.0, 31);  // settle at unity
    (void)attack.next(-3.0, 31);   // one block of loud
    const double after_attack = attack.gain_db();
    ac3::meta::RangeController release{p, ac3::SampleRate::k48000};
    for (int block = 0; block < 500; ++block) {
        (void)release.next(-3.0, 31);  // settle at the cut
    }
    const double from = release.gain_db();
    (void)release.next(-29.0, 31);  // one block of dialogue
    const double moved_up = release.gain_db() - from;
    CHECK(after_attack < -0.15);        // attack moved a long way in one block
    CHECK(moved_up < -after_attack);    // release moved less
}

TEST_CASE("the heavy compressor keeps its ceiling", "[drc]") {
    const ac3::meta::HeavyConfig config{.peak_ceiling_dbfs = -2.0};
    ac3::meta::HeavyCompressor compressor{config, ac3::SampleRate::k48000};
    // A peak that jumps about frame to frame, including straight from very
    // quiet to nearly full scale - the case an attack-smoothed limiter misses.
    constexpr std::array<double, 10> peaks = {-40.0, -38.0, -0.2,  -0.1, -30.0,
                                              -0.5,  -20.0, -10.0, -0.05, -45.0};
    for (const double peak : peaks) {
        const auto word = compressor.next(peak, 24);
        const double applied = ac3::meta::to_db(compr_gain(word));
        CHECK(peak + applied <= config.peak_ceiling_dbfs + 1e-9);
    }
}

TEST_CASE("the heavy compressor releases slowly and attacks at once", "[drc]") {
    const ac3::meta::HeavyConfig config{.peak_ceiling_dbfs = -2.0,
                                       .release_db_per_second = 10.0};
    ac3::meta::HeavyCompressor compressor{config, ac3::SampleRate::k48000};
    (void)compressor.next(-40.0, 24);
    const double quiet_gain = compressor.gain_db();
    CHECK(quiet_gain == Catch::Approx(4.0));  // dialnorm 24 with a -20 dBFS target
    (void)compressor.next(-1.0, 24);
    CHECK(compressor.gain_db() == Catch::Approx(-1.0));  // instantaneous
    // A frame is 32 ms, so 10 dB/s is 0.32 dB of release per frame.
    (void)compressor.next(-40.0, 24);
    CHECK(compressor.gain_db() == Catch::Approx(-1.0 + 0.32).margin(0.001));
}

// --- loudness -------------------------------------------------------------

TEST_CASE("the loudness meter hits the BS.1770 calibration point", "[loudness]") {
    // BS.1770 / EBU Tech 3341: a 1 kHz sine at -20 dBFS in left and right reads
    // -20.0 LKFS. That single number pins the K-weighting gain, the -0.691
    // offset and the channel weights all at once.
    ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    std::vector<float> tone(48000 * 10);
    for (std::size_t n = 0; n < tone.size(); ++n) {
        tone[n] = static_cast<float>(
            0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);
    const auto lkfs = meter.integrated_lkfs();
    REQUIRE(lkfs.has_value());
    CHECK(*lkfs == Catch::Approx(-20.0).margin(0.1));
    CHECK(ac3::meta::dialnorm_from_lkfs(*lkfs) == 20);
}

TEST_CASE("the loudness meter weights the surrounds and drops the LFE",
          "[loudness]") {
    std::vector<float> tone(48000 * 6);
    std::vector<float> silence(48000 * 6, 0.0f);
    for (std::size_t n = 0; n < tone.size(); ++n) {
        tone[n] = static_cast<float>(
            0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    const auto measure = [&](std::span<const std::span<const float>> channels) {
        ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k3_2, true};
        meter.push(channels);
        return meter.integrated_lkfs();
    };
    // 3/2 + LFE coded order is L, C, R, Ls, Rs, LFE.
    const std::array<std::span<const float>, 6> fronts = {tone, silence, silence,
                                                          silence, silence, silence};
    const std::array<std::span<const float>, 6> surround = {silence, silence, silence,
                                                            tone, silence, silence};
    const std::array<std::span<const float>, 6> lfe_only = {silence, silence, silence,
                                                            silence, silence, tone};
    const auto front = measure(fronts);
    const auto rear = measure(surround);
    REQUIRE(front.has_value());
    REQUIRE(rear.has_value());
    // BS.1770 Table 3: a surround channel carries a weight of 1.41, i.e.
    // +1.5 dB relative to a front channel.
    CHECK(*rear - *front == Catch::Approx(10.0 * std::log10(1.41)).margin(0.02));
    // The LFE is excluded outright, so an LFE-only programme has no measurable
    // loudness at all rather than a quiet one.
    CHECK_FALSE(measure(lfe_only).has_value());
}

TEST_CASE("silence has no loudness, and dialnorm clamps to its legal range",
          "[loudness]") {
    ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const std::vector<float> silence(48000 * 2, 0.0f);
    const std::array<std::span<const float>, 2> channels = {silence, silence};
    meter.push(channels);
    // Below the -70 LKFS absolute gate: reporting a number here would put an
    // invented dialnorm on the stream.
    CHECK_FALSE(meter.integrated_lkfs().has_value());

    // §5.4.2.8: valid values are 1..31, and 0 is reserved.
    CHECK(ac3::meta::dialnorm_from_lkfs(-23.0) == 23);
    CHECK(ac3::meta::dialnorm_from_lkfs(-23.4) == 23);
    CHECK(ac3::meta::dialnorm_from_lkfs(-23.6) == 24);
    CHECK(ac3::meta::dialnorm_from_lkfs(-0.2) == 1);   // never 0
    CHECK(ac3::meta::dialnorm_from_lkfs(-45.0) == 31);
}

// --- downmix --------------------------------------------------------------

TEST_CASE("the mix-level tables are the spec's quarter-powers of two", "[mixing]") {
    // Tables 5.9, 5.10, D2.3-D2.6 print rounded values; these are the exact
    // ones, so a chain of them does not drift.
    using ac3::meta::coefficient;
    CHECK(coefficient(ac3::meta::CentreMixLevel::kMinus3dB) ==
          Catch::Approx(0.707).margin(0.0005));
    CHECK(coefficient(ac3::meta::CentreMixLevel::kMinus4_5dB) ==
          Catch::Approx(0.595).margin(0.0005));
    CHECK(coefficient(ac3::meta::CentreMixLevel::kMinus6dB) == 0.5);
    CHECK(coefficient(ac3::meta::SurroundMixLevel::kSilent) == 0.0);
    CHECK(coefficient(ac3::meta::MixLevel::kPlus3dB) == Catch::Approx(1.414).margin(0.0005));
    CHECK(coefficient(ac3::meta::MixLevel::kPlus1_5dB) ==
          Catch::Approx(1.189).margin(0.0005));
    CHECK(coefficient(ac3::meta::MixLevel::kUnity) == 1.0);
    CHECK(coefficient(ac3::meta::MixLevel::kMinus1_5dB) ==
          Catch::Approx(0.841).margin(0.0005));
    // §E2.3.1.11: LFE mix level (dB) = 10 - code, so 0..31 spans +10 to -21.
    CHECK(ac3::meta::lfe_mix_level_db(0) == 10.0);
    CHECK(ac3::meta::lfe_mix_level_db(31) == -21.0);
    // Tables D2.4 / D2.6 reserve the three loudest surround codes.
    CHECK_FALSE(ac3::meta::valid_surround_mix_level(ac3::meta::MixLevel::kPlus3dB));
    CHECK_FALSE(ac3::meta::valid_surround_mix_level(ac3::meta::MixLevel::kUnity));
    CHECK(ac3::meta::valid_surround_mix_level(ac3::meta::MixLevel::kMinus1_5dB));
    CHECK(ac3::meta::valid_surround_mix_level(ac3::meta::MixLevel::kSilent));
}

// Test NAMES stay ASCII: catch_discover_tests passes them back on the command
// line, where a section sign does not survive the round trip.
TEST_CASE("downmix coefficients are normalised and route correctly", "[mixing]") {
    const double clev = ac3::meta::coefficient(ac3::meta::CentreMixLevel::kMinus4_5dB);
    const double slev = ac3::meta::coefficient(ac3::meta::SurroundMixLevel::kMinus6dB);

    for (const auto acmod : {ac3::Acmod::k1_0, ac3::Acmod::k2_0, ac3::Acmod::k3_0,
                             ac3::Acmod::k2_1, ac3::Acmod::k3_1, ac3::Acmod::k2_2,
                             ac3::Acmod::k3_2}) {
        const auto stereo = ac3::meta::stereo_downmix(acmod, clev, slev);
        const auto mono = ac3::meta::mono_downmix(acmod, clev, slev);
        // §7.8.1: no output's coefficients may sum above 1, or a full-scale
        // input overloads the fold-down.
        const auto total = [](std::span<const double> c) {
            double sum = 0.0;
            for (const double value : c) {
                sum += value;
            }
            return sum;
        };
        CHECK(total(stereo.left) <= 1.0 + 1e-12);
        CHECK(total(stereo.right) <= 1.0 + 1e-12);
        CHECK(total(mono) <= 1.0 + 1e-12);
        // Channels the layout does not code contribute nothing.
        const auto nfchans = static_cast<std::size_t>(fullbw_channel_count(acmod));
        for (std::size_t ch = nfchans; ch < 5; ++ch) {
            CHECK(stereo.left[ch] == 0.0);
            CHECK(stereo.right[ch] == 0.0);
            CHECK(mono[ch] == 0.0);
        }
    }

    // 3/2: left takes L, the centre at clev and the LEFT surround only.
    const auto full = ac3::meta::stereo_downmix(ac3::Acmod::k3_2, clev, slev);
    const double norm = 1.0 + clev + slev;
    CHECK(full.left[0] == Catch::Approx(1.0 / norm));
    CHECK(full.left[1] == Catch::Approx(clev / norm));
    CHECK(full.left[2] == 0.0);
    CHECK(full.left[3] == Catch::Approx(slev / norm));
    CHECK(full.left[4] == 0.0);
    CHECK(full.right[4] == Catch::Approx(slev / norm));

    // A mono source has no left or right to route, so §7.8 mixes its centre
    // into both outputs at -3 dB - and §7.8.1's normalisation does NOT then
    // scale that back up to unity, because it only ever attenuates to satisfy
    // a bound. -3 dB into each of two speakers is also the answer that
    // conserves acoustic power.
    const auto mono_source = ac3::meta::stereo_downmix(ac3::Acmod::k1_0, clev, slev);
    CHECK(mono_source.left[0] == Catch::Approx(ac3::meta::level::kMinus3dB));
    CHECK(mono_source.right[0] == Catch::Approx(ac3::meta::level::kMinus3dB));

    // Silencing the surrounds removes them from the fold-down entirely, and
    // renormalisation gives their share back to the remaining channels.
    const auto dropped = ac3::meta::stereo_downmix(
        ac3::Acmod::k3_2, clev, ac3::meta::coefficient(ac3::meta::SurroundMixLevel::kSilent));
    CHECK(dropped.left[3] == 0.0);
    CHECK(dropped.left[0] > full.left[0]);
}

TEST_CASE("the mono downmix peak accounts for the MDCT overlap", "[mixing]") {
    // A frame's block 0 windows in the previous frame's last 256 samples, so
    // those samples are coded in this frame and this frame's compr governs
    // them. A peak detector that ignores the overlap under-reads exactly at a
    // loud-to-quiet transition, which is where the ceiling then leaks.
    std::vector<float> quiet(ac3::kSamplesPerFrame, 0.01f);
    const std::array<std::span<const float>, 2> channels = {quiet, quiet};
    std::array<std::array<double, 256>, 2> loud_tail{};
    for (auto& channel : loud_tail) {
        channel.fill(0.9);
    }
    const double without = ac3::meta::mono_downmix_peak_dbfs(
        channels, ac3::Acmod::k2_0, 0.595, 0.5);
    const double with = ac3::meta::mono_downmix_peak_dbfs(
        loud_tail, channels, ac3::Acmod::k2_0, 0.595, 0.5);
    CHECK(without == Catch::Approx(20.0 * std::log10(0.01)).margin(0.01));
    CHECK(with == Catch::Approx(20.0 * std::log10(0.9)).margin(0.01));
}

// --- AC-3 bitstream ------------------------------------------------------

TEST_CASE("an AC-3 stream with no DRC is byte-identical to one from before",
          "[drc][encoder]") {
    // The whole metadata layer is opt-in, and this is the guard on that claim:
    // dynrnge stays clear in every block and compre stays clear in bsi, so a
    // caller who asked for nothing gets exactly the bits they used to.
    const auto audio = stepped_tone(4, 2, 0.5, 0.02, 1);
    ac3::FrameEncoder plain{{.bitrate_kbps = 192, .dialnorm = 27}};
    ac3::FrameEncoder same{{.bitrate_kbps = 192, .dialnorm = 27}};
    for (int frame = 0; frame < 4; ++frame) {
        const auto views = frame_views(audio, frame);
        const auto a = plain.encode_frame(views);
        const auto b = same.encode_frame(views);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(*a == *b);
    }
}

TEST_CASE("AC-3 dynrng survives the round trip and moves the decoded level",
          "[drc][encoder][decoder]") {
    // Eight frames: four loud, then four quiet. Frame 0 is never asserted on -
    // its MDCT window is half history that does not exist, so it is a fade-in
    // rather than steady state - and the assertions land on the last frame of
    // each run, by which point the gain has settled.
    //
    // Film standard's release is a second, which is 31 frames: right for a
    // programme and far too slow to observe in a unit test. The curve is what
    // is under test here, not the time constant (RangeController covers that
    // directly and cheaply), so the profile is used with a short release.
    constexpr int kLoudFrames = 4;
    const auto audio = stepped_tone(8, 2, 0.5, 0.004, kLoudFrames);
    auto profile = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard);
    profile.attack_ms = 5.0;
    profile.release_ms = 40.0;
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .dialnorm = 24, .drc = profile}};

    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < 8; ++frame) {
        auto encoded = encoder.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        frames.push_back(std::move(*encoded));
    }

    // The words the encoder chose have to come back out of the bitstream, with
    // the §7.7.1.2 persistence rule resolved the same way on both sides.
    ac3::FrameDecoder reader;
    std::vector<std::array<std::uint8_t, ac3::kBlocksPerFrame>> words;
    for (const auto& frame : frames) {
        const auto decoded = reader.decode_frame(frame);
        REQUIRE(decoded.has_value());
        words.push_back(decoded->dynrng);
    }
    // Loud frames must be cut and quiet frames lifted, or the curve is not
    // being consulted at all.
    CHECK(dynrng_gain(words[3][5]) < 1.0);  // end of the loud run
    CHECK(dynrng_gain(words[7][5]) > 1.0);  // end of the quiet run

    // And the words must actually change the audio when applied. Decoding the
    // same bytes twice with different drc_scale is the discriminating test: a
    // stream carrying dead metadata gives the same answer both times.
    ac3::FrameDecoder off{{.drc_scale = 0.0}};
    ac3::FrameDecoder on{{.drc_scale = 1.0}};
    std::vector<double> off_db;
    std::vector<double> on_db;
    for (const auto& frame : frames) {
        const auto a = off.decode_frame(frame);
        const auto b = on.decode_frame(frame);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        off_db.push_back(rms_db(a->channels[0]));
        on_db.push_back(rms_db(b->channels[0]));
    }
    // Frame 3 is settled loud, frame 7 settled quiet.
    CHECK(on_db[3] < off_db[3] - 2.0);
    CHECK(on_db[7] > off_db[7] + 2.0);
    const double range_off = off_db[3] - off_db[7];
    const double range_on = on_db[3] - on_db[7];
    CHECK(range_off - range_on > 5.0);

    // Half the compression is half the dB, which is what §7.7.1's partial
    // compression means.
    ac3::FrameDecoder half{{.drc_scale = 0.5}};
    double half_db = 0.0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = half.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        if (i == 3) {
            half_db = rms_db(decoded->channels[0]);
        }
    }
    CHECK(half_db == Catch::Approx(0.5 * (off_db[3] + on_db[3])).margin(0.15));
}

TEST_CASE("AC-3 compr holds its ceiling through the decoder", "[drc][encoder][decoder]") {
    // Nearly full scale, alternating, so the ceiling binds AND the loud-to-quiet
    // transitions are hard - the transition is where the overlap makes a naive
    // peak detector under-read.
    const auto audio = stepped_tone(8, 2, 0.95, 0.004, 2);
    constexpr double ceiling = -1.0;
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448,
                               .dialnorm = 24,
                               .heavy = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = ceiling}}};
    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < 8; ++frame) {
        auto encoded = encoder.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        frames.push_back(std::move(*encoded));
    }

    ac3::FrameDecoder plain;
    ac3::FrameDecoder heavy{{.heavy_compression = true}};
    // The ceiling is measured on the encoder's INPUT downmix, so a decoder's
    // reconstruction can sit a hair above it: the difference is the coding
    // error, not a metadata fault. See HeavyConfig::peak_ceiling_dbfs.
    constexpr double kCodingSlack = 0.1;
    bool saw_word = false;
    bool would_have_breached = false;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto a = plain.decode_frame(frames[i]);
        const auto b = heavy.decode_frame(frames[i]);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(a->compr.has_value());
        saw_word = true;
        if (peak_db(a->channels[0]) > ceiling + kCodingSlack) {
            would_have_breached = true;
        }
        if (i >= 1) {  // skip the fade-in frame
            // The promise, per channel and per frame - including the frames
            // straddling a loud-to-quiet step, where the previous frame's tail
            // is windowed into a frame that has already gone quiet.
            CHECK(peak_db(b->channels[0]) <= ceiling + kCodingSlack);
            CHECK(peak_db(b->channels[1]) <= ceiling + kCodingSlack);
        }
    }
    CHECK(saw_word);
    // Without the metadata this material WOULD have gone over, so the ceiling
    // above is being kept by compr and not by the audio happening to be quiet.
    CHECK(would_have_breached);
}

TEST_CASE("AC-3 dual mono: Ch2's own DRC profile is not Ch1's, and is not assumed",
          "[drc][encoder][decoder][dual-mono]") {
    // Same audio on both channels (stepped_tone fills every channel
    // identically), so any difference between the decoded dynrng and
    // dynrng2 words is attributable only to the profile, never to the input
    // differing between programmes.
    constexpr int kLoudFrames = 4;
    const auto audio = stepped_tone(8, 2, 0.5, 0.0004, kLoudFrames);
    auto film = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard);
    film.attack_ms = 5.0;
    film.release_ms = 40.0;
    auto speech = ac3::meta::profile(ac3::meta::ProfileId::kSpeech);
    speech.attack_ms = 5.0;
    speech.release_ms = 40.0;

    ac3::FrameEncoder both{{.bitrate_kbps = 448,
                            .dialnorm = 24,
                            .dialnorm2 = 24,
                            .acmod = ac3::Acmod::kDualMono,
                            .drc = film,
                            .drc2 = speech}};
    ac3::FrameDecoder reader;
    std::array<std::uint8_t, ac3::kBlocksPerFrame> last_dynrng{};
    std::array<std::uint8_t, ac3::kBlocksPerFrame> last_dynrng2{};
    for (int frame = 0; frame < 8; ++frame) {
        auto encoded = both.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        const auto decoded = reader.decode_frame(*encoded);
        REQUIRE(decoded.has_value());
        last_dynrng = decoded->dynrng;
        last_dynrng2 = decoded->dynrng2;
    }
    // Settled on the last (quiet) frame: at this level film-standard's 6 dB
    // ceiling has already clamped while speech's gentler 5:1 ratio has not
    // yet reached its own 15 dB ceiling, so the two curves must read
    // differently even though Ch1 and Ch2 heard the identical signal.
    const double film_gain_db = 20.0 * std::log10(dynrng_gain(last_dynrng[5]));
    const double speech_gain_db = 20.0 * std::log10(dynrng_gain(last_dynrng2[5]));
    CHECK(speech_gain_db > film_gain_db + 2.0);

    // And the regression this bundle actually fixes: drc alone (no drc2)
    // must NOT silently compress Ch2 with Ch1's curve - it must leave Ch2
    // uncompressed. dialnorm2 already establishes that a dual-mono field
    // with no explicit value is an error, never an inherited one; drc2
    // follows the same rule, just without dialnorm2's hard failure since
    // "no DRC on Ch2" is a legal, meaningful answer where "no dialnorm" is
    // not.
    ac3::FrameEncoder drc_only{{.bitrate_kbps = 448,
                                .dialnorm = 24,
                                .dialnorm2 = 24,
                                .acmod = ac3::Acmod::kDualMono,
                                .drc = film}};
    ac3::FrameDecoder reader2;
    for (int frame = 0; frame < 8; ++frame) {
        auto encoded = drc_only.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        const auto decoded = reader2.decode_frame(*encoded);
        REQUIRE(decoded.has_value());
        for (const auto word : decoded->dynrng2) {
            CHECK(word == ac3::meta::kDynrngUnity);
        }
    }
}

TEST_CASE("AC-3 dual mono: Ch2's own heavy compression is not Ch1's, and is not assumed",
          "[drc][encoder][decoder][dual-mono]") {
    // Loud on both channels, identical signal, so both ceilings actually
    // bind - only then does a difference in the DECODED audio prove each
    // programme's own ceiling is being enforced, rather than merely present.
    const auto audio = stepped_tone(6, 2, 0.95, 0.95, 6);
    constexpr double kLooseCeiling = -1.0;
    constexpr double kTightCeiling = -6.0;
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448,
         .dialnorm = 24,
         .dialnorm2 = 24,
         .acmod = ac3::Acmod::kDualMono,
         .heavy = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = kLooseCeiling},
         .heavy2 = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = kTightCeiling}}};
    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < 6; ++frame) {
        auto encoded = encoder.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        frames.push_back(std::move(*encoded));
    }

    ac3::FrameDecoder heavy{{.heavy_compression = true}};
    // Looser than the sibling "AC-3 compr holds its ceiling" test's 0.1 dB:
    // that test alternates loud/quiet, where this one holds a steady
    // near-full-scale tone across every checked frame, so std::max below is
    // more likely to catch a genuine inter-sample reconstruction peak a
    // touch above the nominal target - still tight enough that a channel
    // landing near the OTHER programme's ceiling (a ~5 dB gap) would fail
    // it outright.
    constexpr double kCodingSlack = 0.3;
    double ch1_peak = -200.0;
    double ch2_peak = -200.0;
    for (std::size_t i = 1; i < frames.size(); ++i) {  // skip the fade-in frame
        const auto decoded = heavy.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->compr.has_value());
        REQUIRE(decoded->compr2.has_value());
        ch1_peak = std::max(ch1_peak, peak_db(decoded->channels[0]));
        ch2_peak = std::max(ch2_peak, peak_db(decoded->channels[1]));
    }
    // Each programme kept ITS OWN ceiling - if Ch2's controller had been
    // built from `heavy` instead of `heavy2` (the bug this bundle fixes),
    // both channels would land near the loose -1 dBFS ceiling instead.
    CHECK(ch1_peak <= kLooseCeiling + kCodingSlack);
    CHECK(ch2_peak <= kTightCeiling + kCodingSlack);
    CHECK(ch1_peak > ch2_peak + 3.0);

    // And the literal regression: heavy alone (no heavy2) must clear
    // compr2e, not silently carry Ch1's compr as Ch2's too - this is the
    // exact bsi bit that used to read `config_.heavy ? 1 : 0` instead of
    // `config_.heavy2 ? 1 : 0`.
    ac3::FrameEncoder heavy_only{
        {.bitrate_kbps = 448,
         .dialnorm = 24,
         .dialnorm2 = 24,
         .acmod = ac3::Acmod::kDualMono,
         .heavy = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = kLooseCeiling}}};
    ac3::FrameDecoder plain;
    for (int frame = 0; frame < 6; ++frame) {
        auto encoded = heavy_only.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        const auto decoded = plain.decode_frame(*encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->compr.has_value());
        CHECK_FALSE(decoded->compr2.has_value());
    }
}

TEST_CASE("AC-3 carries the configured downmix levels in bsi", "[mixing][encoder]") {
    // 3/2 is the only layout with both fields, and they sit at fixed offsets:
    // syncinfo is 40 bits, then bsid(5) bsmod(3) acmod(3) puts cmixlev at 51.
    const auto audio = stepped_tone(2, 6, 0.3, 0.05, 1);
    const auto encode_with = [&](ac3::meta::CentreMixLevel cmix,
                                 ac3::meta::SurroundMixLevel smix) {
        ac3::FrameEncoder encoder{{.bitrate_kbps = 448,
                                   .acmod = ac3::Acmod::k3_2,
                                   .lfe = true,
                                   .cmixlev = cmix,
                                   .surmixlev = smix}};
        (void)encoder.encode_frame(frame_views(audio, 0));
        auto frame = encoder.encode_frame(frame_views(audio, 1));
        REQUIRE(frame.has_value());
        ac3::BitReader reader{*frame};
        reader.skip(40 + 5 + 3 + 3);
        const auto cmixlev = reader.read(2);
        const auto surmixlev = reader.read(2);
        return std::pair{cmixlev, surmixlev};
    };
    const auto minus3 = encode_with(ac3::meta::CentreMixLevel::kMinus3dB,
                                    ac3::meta::SurroundMixLevel::kMinus3dB);
    CHECK(minus3.first == 0);
    CHECK(minus3.second == 0);
    const auto quiet = encode_with(ac3::meta::CentreMixLevel::kMinus6dB,
                                   ac3::meta::SurroundMixLevel::kSilent);
    CHECK(quiet.first == 2);
    CHECK(quiet.second == 2);
    // The decoder reads them without complaint either way.
    CHECK(minus3 != quiet);
}

// --- E-AC-3 bitstream ----------------------------------------------------

namespace {

// Walk an E-AC-3 substream's bsi and audfrm to the dynrnge bit of block 0.
// Everything up to there is determinate for this encoder's profile, so a
// misplaced field shows up as a wrong dynrnge rather than as silence.
struct Eac3Probe {
    int dialnorm = 0;
    std::optional<std::uint8_t> compr;
    bool mixmdate = false;
    std::optional<std::uint8_t> dynrng;  // block 0's transmitted word
};

Eac3Probe probe_eac3(std::span<const std::byte> frame) {
    ac3::BitReader r{frame};
    Eac3Probe out;
    REQUIRE(r.read(16) == ac3::kSyncWord);
    const auto strmtyp = r.read(2);
    r.skip(3);  // substreamid
    r.skip(11); // frmsiz
    r.skip(2);  // fscod (never 0x3 here, so no fscod2)
    const auto numblkscod = r.read(2);
    const auto acmod = r.read(3);
    const bool lfeon = r.read(1) != 0;
    REQUIRE(r.read(5) == ac3::eac3::kBsid);
    out.dialnorm = static_cast<int>(r.read(5));
    if (r.read(1) != 0) {  // compre
        out.compr = static_cast<std::uint8_t>(r.read(8));
    }
    if (strmtyp == 1) {
        if (r.read(1) != 0) {  // chanmape
            r.skip(16);
        }
    }
    out.mixmdate = r.read(1) != 0;
    if (out.mixmdate) {
        if (acmod > 0x2) {
            r.skip(2);  // dmixmod
        }
        if ((acmod & 0x1) != 0 && acmod > 0x2) {
            r.skip(6);  // ltrtcmixlev, lorocmixlev
        }
        if ((acmod & 0x4) != 0) {
            r.skip(6);  // ltrtsurmixlev, lorosurmixlev
        }
        if (lfeon && r.read(1) != 0) {
            r.skip(5);  // lfemixlevcod
        }
        if (strmtyp == 0) {
            if (r.read(1) != 0) r.skip(6);  // pgmscl
            if (r.read(1) != 0) r.skip(6);  // extpgmscl
            REQUIRE(r.read(2) == 0);        // mixdef: no mixing-parameter data
            if (acmod < 0x2 && r.read(1) != 0) {
                r.skip(14);  // panmean + paninfo
            }
            REQUIRE(r.read(1) == 0);  // frmmixcfginfoe
        }
    }
    REQUIRE(r.read(1) == 0);  // infomdate
    REQUIRE(r.read(1) == 0);  // addbsie

    // audfrm (Table E1.3), the fixed profile this encoder emits.
    REQUIRE(numblkscod == 3);
    r.skip(1 + 1);  // expstre, ahte
    r.skip(2);      // snroffststr
    r.skip(1);                        // transproce
    const bool blkswe = r.read(1) != 0;  // blkswe
    // dithflage is the ONLY set flag in this run, which makes it the canary for
    // everything upstream of it: drop or add a bit anywhere in bsi and this
    // reads a neighbouring zero instead. Without it a bsi field of the wrong
    // width shifts the whole frame and the probe still finds plausible values.
    REQUIRE(r.read(1) == 1);  // dithflage
    r.skip(1 + 1);  // bamode, frmfgaincode
    r.skip(1 + 1);  // dbaflde, skipflde
    r.skip(1);      // spxattene
    const int nfchans = ac3::fullbw_channel_count(static_cast<ac3::Acmod>(acmod));
    if (acmod > 0x1) {
        r.skip(1);                      // cplinu[0]
        r.skip(ac3::kBlocksPerFrame - 1);  // cplstre[1..5]
    }
    r.skip(static_cast<std::size_t>(nfchans) * 5);  // frmchexpstr
    if (lfeon) {
        r.skip(ac3::kBlocksPerFrame);  // lfeexpstr
    }
    if (strmtyp == 0) {
        r.skip(static_cast<std::size_t>(nfchans) * 5);  // convexpstr
    }
    r.skip(6 + 4);  // frmcsnroffst, frmfsnroffst
    r.skip(1);      // blkstrtinfoe

    // audblk 0 (Table E1.4): blksw per channel (only when blkswe), then
    // dithflag per channel, then dynrnge.
    if (blkswe) {
        r.skip(static_cast<std::size_t>(nfchans));  // blksw[ch]
    }
    r.skip(static_cast<std::size_t>(nfchans));  // dithflag[ch]
    if (r.read(1) != 0) {
        out.dynrng = static_cast<std::uint8_t>(r.read(8));
    }
    REQUIRE_FALSE(r.overflowed());
    return out;
}

}  // namespace

TEST_CASE("E-AC-3 carries the mixmdate group and stays the same size",
          "[mixing][eac3]") {
    const auto audio = stepped_tone(2, 6, 0.3, 0.05, 1);
    const ac3::meta::MixMetadata mixing{
        .dmixmod = ac3::meta::DownmixMode::kLoRo,
        .ltrtcmixlev = ac3::meta::MixLevel::kMinus3dB,
        .lorocmixlev = ac3::meta::MixLevel::kMinus4_5dB,
        .ltrtsurmixlev = ac3::meta::MixLevel::kMinus3dB,
        .lorosurmixlev = ac3::meta::MixLevel::kMinus6dB,
        .lfemixlevcod = ac3::meta::kLfeMixLevelIdeal};
    // DRC is switched on so the probe has to walk all the way past the mixmdate
    // group to a field whose value is known: a group of the wrong length shifts
    // the frame, and the probe would then read the wrong bit for dynrnge.
    ac3::eac3::FrameConfig base{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .dialnorm = 24};
    base.drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard);
    auto with = base;
    with.mixing = mixing;

    ac3::eac3::FrameEncoder plain_encoder{base};
    ac3::eac3::FrameEncoder mixed_encoder{with};
    (void)plain_encoder.encode_frame(frame_views(audio, 0));
    (void)mixed_encoder.encode_frame(frame_views(audio, 0));
    const auto plain = plain_encoder.encode_frame(frame_views(audio, 1));
    const auto mixed = mixed_encoder.encode_frame(frame_views(audio, 1));
    REQUIRE(plain.has_value());
    REQUIRE(mixed.has_value());
    // CBR: the group costs bits, so the SNR search has to give some back. The
    // frame size cannot move.
    CHECK(plain->size() == mixed->size());
    CHECK(*plain != *mixed);

    // And the group has to be where Table E1.2 puts it, which the probe checks
    // by walking all the way to block 0's dynrnge: any misplaced field lands
    // the probe somewhere else and the requires inside it fail.
    const auto probed = probe_eac3(*mixed);
    const auto unmixed = probe_eac3(*plain);
    CHECK(probed.mixmdate);
    CHECK(probed.dialnorm == 24);
    CHECK_FALSE(unmixed.mixmdate);
    // Block 0 always transmits a word when a profile is configured (§7.7.1.2),
    // and carrying the mixing group cannot change which word that is.
    REQUIRE(probed.dynrng.has_value());
    REQUIRE(unmixed.dynrng.has_value());
    CHECK(*probed.dynrng == *unmixed.dynrng);
}

TEST_CASE("E-AC-3 rejects metadata that cannot legally be carried",
          "[mixing][eac3]") {
    const ac3::eac3::FrameConfig base{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true};

    // Tables D2.4 / D2.6 reserve the three loudest surround levels; a decoder
    // receiving one substitutes 0.841, so writing it means the level applied is
    // not the level asked for.
    auto reserved = base;
    reserved.mixing = ac3::meta::MixMetadata{.lorosurmixlev = ac3::meta::MixLevel::kUnity};
    auto frame = ac3::eac3::build_silent_frame(reserved);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == ac3::FrameError::kInvalidMixLevel);

    // §E2.3.1.11 gives lfemixlevcod five bits.
    auto out_of_range = base;
    out_of_range.mixing = ac3::meta::MixMetadata{.lfemixlevcod = 32};
    frame = ac3::eac3::build_silent_frame(out_of_range);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == ac3::FrameError::kInvalidMixLevel);

    // §E3.8.5 gives a dependent substream's compre to the end-of-programme
    // marker, so heavy compression cannot ride there.
    auto dependent = base;
    dependent.strmtyp = ac3::eac3::StreamType::kDependent;
    dependent.chanmap = ac3::eac3::chanmap::kLeftSurroundBit | ac3::eac3::chanmap::kRightSurroundBit |
                        ac3::eac3::chanmap::kLrsRrsBit;
    dependent.acmod = ac3::Acmod::k2_2;
    dependent.lfe = false;
    dependent.heavy = ac3::meta::HeavyConfig{};
    frame = ac3::eac3::build_silent_frame(dependent);
    REQUIRE_FALSE(frame.has_value());
    CHECK(frame.error() == ac3::FrameError::kInvalidSubstream);
}

TEST_CASE("every substream of an E-AC-3 access unit carries the same dynrng",
          "[drc][eac3]") {
    // A decoder applies each substream's word to that substream's own channels.
    // Words derived per substream would therefore tilt the mix: the bed would
    // duck while the height channels did not. So the access unit measures the
    // independent substream once and hands the result down - including to a
    // dependent whose own channels are nowhere near that level.
    ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 448,
                          .acmod = ac3::Acmod::k3_2,
                          .lfe = true,
                          .dialnorm = 24,
                          .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard)};
    config.dependents.push_back({.bitrate_kbps = 224,
                                 .acmod = ac3::Acmod::k2_0,
                                 .chanmap = ac3::eac3::chanmap::k512Height});
    ac3::eac3::AccessUnitEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 8);

    // A loud 5.1 bed with near-silent height channels: measured separately the
    // two substreams would reach wildly different gains.
    const auto bed = stepped_tone(3, 6, 0.5, 0.5, 1);
    const auto height = stepped_tone(3, 2, 0.0005, 0.0005, 1);
    std::vector<std::array<std::uint8_t, 2>> words;
    for (int frame = 0; frame < 3; ++frame) {
        auto views = frame_views(bed, frame);
        for (auto& view : frame_views(height, frame)) {
            views.push_back(view);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        REQUIRE(unit->substream_count() == 2);
        const auto independent = probe_eac3(unit->substream(0));
        const auto dependent = probe_eac3(unit->substream(1));
        REQUIRE(independent.dynrng.has_value());
        REQUIRE(dependent.dynrng.has_value());
        words.push_back({*independent.dynrng, *dependent.dynrng});
        // §E3.8.5: the dependent's compre is the marker, and the word it drags
        // in is unity - never a gain.
        REQUIRE(dependent.compr.has_value());
        CHECK(*dependent.compr == ac3::meta::kComprUnity);
        CHECK_FALSE(independent.compr.has_value());
    }
    for (const auto& pair : words) {
        CHECK(pair[0] == pair[1]);
    }
    // And the gain is a real one, derived from the loud bed rather than left at
    // unity - otherwise "they agree" would be trivially true.
    CHECK(words.back()[0] != ac3::meta::kDynrngUnity);
    CHECK(dynrng_gain(words.back()[0]) < 1.0);
}
