#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/spatial/spatial.hpp"

using Catch::Approx;

namespace {

// A whole number of cycles, so peak and RMS have exact closed forms: 1000 Hz
// at 48 kHz is 48 samples per cycle, and the peak lands on a sample.
std::vector<float> sine(double amplitude, std::size_t samples, double hz = 1000.0) {
    std::vector<float> out(samples);
    for (std::size_t n = 0; n < samples; ++n) {
        out[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return out;
}

std::vector<std::span<const float>> views(std::span<const std::vector<float>> channels) {
    std::vector<std::span<const float>> spans;
    spans.reserve(channels.size());
    for (const auto& channel : channels) {
        spans.emplace_back(channel);
    }
    return spans;
}

}  // namespace

TEST_CASE("dBFS conversion and meter scaling", "[analysis]") {
    CHECK(ac3::analysis::to_dbfs(1.0) == Approx(0.0));
    CHECK(ac3::analysis::to_dbfs(0.5) == Approx(-6.0205999));
    CHECK(ac3::analysis::to_dbfs(0.25) == Approx(-12.0411998));
    CHECK(ac3::analysis::to_dbfs(-0.5) == Approx(-6.0205999));  // magnitude only
    CHECK(ac3::analysis::to_dbfs(0.0) == ac3::analysis::kFloorDb);
    // Nothing may report below the floor, however quiet.
    CHECK(ac3::analysis::to_dbfs(1e-30) == ac3::analysis::kFloorDb);

    CHECK(ac3::analysis::meter_fraction(0.0) == Approx(1.0));
    CHECK(ac3::analysis::meter_fraction(-30.0) == Approx(0.5));
    CHECK(ac3::analysis::meter_fraction(-60.0) == Approx(0.0));
    CHECK(ac3::analysis::meter_fraction(-90.0) == Approx(0.0));   // clamped
    CHECK(ac3::analysis::meter_fraction(6.0) == Approx(1.0));     // clamped
    CHECK(ac3::analysis::meter_fraction(-20.0, -40.0) == Approx(0.5));
    static_assert(ac3::analysis::meter_fraction(-30.0) > 0.49);
}

TEST_CASE("channel names follow A/52 Table 5.8", "[analysis]") {
    using ac3::Acmod;
    using ac3::analysis::channel_name;

    CHECK(channel_name(Acmod::kDualMono, false, 0) == "Ch1");
    CHECK(channel_name(Acmod::kDualMono, false, 1) == "Ch2");
    CHECK(channel_name(Acmod::k1_0, false, 0) == "C");
    CHECK(channel_name(Acmod::k2_0, false, 0) == "L");
    CHECK(channel_name(Acmod::k2_0, false, 1) == "R");
    CHECK(channel_name(Acmod::k3_0, false, 1) == "C");
    CHECK(channel_name(Acmod::k2_1, false, 2) == "S");
    CHECK(channel_name(Acmod::k3_1, false, 3) == "S");
    CHECK(channel_name(Acmod::k2_2, false, 2) == "SL");
    CHECK(channel_name(Acmod::k2_2, false, 3) == "SR");

    // 3/2: L C R SL SR, with the LFE appended last.
    const std::array<std::string_view, 6> expected = {"L", "C", "R", "SL", "SR", "LFE"};
    for (int i = 0; i < 6; ++i) {
        CHECK(channel_name(Acmod::k3_2, true, i) == expected[static_cast<std::size_t>(i)]);
    }
    // Without the LFE the array stops one short.
    CHECK(channel_name(Acmod::k3_2, false, 5).empty());
    CHECK(channel_name(Acmod::k3_2, true, 6).empty());
    CHECK(channel_name(Acmod::k2_0, false, -1).empty());

    // The LFE always sits at nfchans, whatever the mode.
    CHECK(channel_name(Acmod::k1_0, true, 1) == "LFE");
    CHECK(channel_name(Acmod::k2_0, true, 2) == "LFE");

    CHECK(ac3::analysis::layout_name(Acmod::k3_2, true) == "3/2 + LFE");
    CHECK(ac3::analysis::layout_name(Acmod::k2_0, false) == "2/0 stereo");
    CHECK(ac3::analysis::layout_name(Acmod::k1_0, true) == "1/0 mono + LFE");
}

TEST_CASE("channel azimuths sit on the BS.775 ring", "[analysis]") {
    using ac3::Acmod;
    using ac3::analysis::channel_azimuth_deg;

    CHECK(channel_azimuth_deg(Acmod::k3_2, true, 0) == 30.0);    // L
    CHECK(channel_azimuth_deg(Acmod::k3_2, true, 1) == 0.0);     // C
    CHECK(channel_azimuth_deg(Acmod::k3_2, true, 2) == -30.0);   // R
    CHECK(channel_azimuth_deg(Acmod::k3_2, true, 3) == 110.0);   // SL
    CHECK(channel_azimuth_deg(Acmod::k3_2, true, 4) == -110.0);  // SR
    // The LFE is non-directional, and 1+1 is two programs rather than a scene.
    CHECK_FALSE(channel_azimuth_deg(Acmod::k3_2, true, 5).has_value());
    CHECK_FALSE(channel_azimuth_deg(Acmod::kDualMono, false, 0).has_value());
    // Mono surround is behind the listener.
    CHECK(channel_azimuth_deg(Acmod::k3_1, false, 3) == 180.0);
    CHECK(channel_azimuth_deg(Acmod::k2_1, false, 2) == 180.0);
    // 2/2 keeps the surrounds in their 3/2 places.
    CHECK(channel_azimuth_deg(Acmod::k2_2, false, 1) == -30.0);
    CHECK(channel_azimuth_deg(Acmod::k2_2, false, 2) == 110.0);

    // The ring the analysis quotes must be the ring the panner uses: a source
    // steered at a channel's azimuth has to land in that channel.
    for (int ch = 0; ch < 5; ++ch) {
        const auto azimuth = channel_azimuth_deg(Acmod::k3_2, false, ch);
        REQUIRE(azimuth.has_value());
        const auto gains = ac3::spatial::pan_azimuth(*azimuth);
        CHECK(gains[static_cast<std::size_t>(ch)] == Approx(1.0));
    }
}

TEST_CASE("summary reports exact peak and RMS", "[analysis]") {
    // 1 s of a half-scale tone: peak 0.5 (-6.02 dBFS), RMS 0.5/sqrt(2)
    // (-9.03 dBFS). The second channel is 6 dB quieter.
    const std::vector<std::vector<float>> channels = {sine(0.5, 48000), sine(0.25, 48000)};
    ac3::analysis::LevelMeter meter{ac3::Acmod::k2_0, false, 48000};
    meter.process(views(channels));

    const auto summary = meter.summary();
    REQUIRE(summary.size() == 2);
    CHECK(summary[0].peak == Approx(0.5).epsilon(1e-6));
    CHECK(summary[0].rms() == Approx(0.5 / std::numbers::sqrt2).epsilon(1e-5));
    CHECK(summary[0].peak_db() == Approx(-6.0206).margin(1e-3));
    CHECK(summary[0].rms_db() == Approx(-9.0309).margin(1e-3));
    CHECK(summary[1].peak_db() == Approx(-12.0412).margin(1e-3));
    CHECK(summary[1].rms_db() == Approx(-15.0515).margin(1e-3));
    CHECK(summary[0].samples == 48000);
    CHECK(summary[0].clipped_samples == 0);

    // Feeding the same audio again must not move an average.
    meter.process(views(channels));
    CHECK(meter.summary()[0].rms_db() == Approx(-9.0309).margin(1e-3));
    CHECK(meter.summary()[0].samples == 96000);

    meter.reset();
    CHECK(meter.summary()[0].samples == 0);
    CHECK(meter.levels()[0].peak_db == ac3::analysis::kFloorDb);
}

TEST_CASE("meter ballistics: instant attack, timed fallback, held peak", "[analysis]") {
    const std::vector<std::vector<float>> loud = {sine(1.0, 4800)};   // 100 ms, 0 dBFS
    const std::vector<std::vector<float>> quiet = {std::vector<float>(4800, 0.0f)};
    ac3::analysis::LevelMeter meter{ac3::Acmod::k1_0, false, 48000};

    meter.process(views(loud));
    CHECK(meter.levels()[0].peak_db == Approx(0.0).margin(1e-6));  // attack is instant
    CHECK(meter.levels()[0].hold_db == Approx(0.0).margin(1e-6));

    // 500 ms of silence at the default 20 dB/s puts the peak exactly 10 dB
    // down, while the 1200 ms hold has not yet expired.
    for (int block = 0; block < 5; ++block) {
        meter.process(views(quiet));
    }
    CHECK(meter.levels()[0].peak_db == Approx(-10.0).margin(1e-6));
    CHECK(meter.levels()[0].hold_db == Approx(0.0).margin(1e-6));

    // Past the 1200 ms hold the marker follows the peak down rather than
    // sticking, but stays above it - it is the recent maximum, not the level.
    for (int block = 0; block < 20; ++block) {
        meter.process(views(quiet));
    }
    CHECK(meter.levels()[0].peak_db == Approx(-50.0).margin(1e-6));  // 2.5 s at 20 dB/s
    CHECK(meter.levels()[0].hold_db < -10.0);
    CHECK(meter.levels()[0].hold_db >= meter.levels()[0].peak_db);

    // The fallback stops at the floor instead of running away.
    for (int block = 0; block < 200; ++block) {
        meter.process(views(quiet));
    }
    CHECK(meter.levels()[0].peak_db == ac3::analysis::kFloorDb);
}

TEST_CASE("integrated RMS converges on the signal's true level", "[analysis]") {
    const std::vector<std::vector<float>> tone = {sine(0.5, 480)};  // 10 ms blocks
    ac3::analysis::LevelMeter meter{ac3::Acmod::k1_0, false, 48000};

    // One 10 ms block into a 300 ms integration must not already read full
    // level - that is the whole point of the average.
    meter.process(views(tone));
    CHECK(meter.levels()[0].rms_db < -12.0);

    // After several time constants it settles on 0.5/sqrt(2).
    for (int block = 0; block < 200; ++block) {
        meter.process(views(tone));
    }
    CHECK(meter.levels()[0].rms_db == Approx(-9.0309).margin(0.05));
    // RMS of a sine sits 3 dB under its peak.
    CHECK(meter.levels()[0].peak_db == Approx(-6.0206).margin(1e-3));
}

TEST_CASE("clipping is detected at PCM16 full scale", "[analysis]") {
    ac3::analysis::LevelMeter meter{ac3::Acmod::k2_0, false, 48000};
    // 32767/32768 is as loud as a PCM16 source can be; a meter that insisted
    // on 1.0f would never flag a file mastered to 0 dBFS.
    std::vector<std::vector<float>> channels = {std::vector<float>(480, ac3::analysis::kFullScale),
                                                std::vector<float>(480, 0.9f)};
    meter.process(views(channels));
    CHECK(meter.levels()[0].clipped);
    CHECK(meter.summary()[0].clipped_samples == 480);
    CHECK_FALSE(meter.levels()[1].clipped);
    CHECK(meter.summary()[1].clipped_samples == 0);

    // The flag latches: a clip that has scrolled past still happened.
    channels[0].assign(480, 0.1f);
    meter.process(views(channels));
    CHECK(meter.levels()[0].clipped);
}

TEST_CASE("interleaved metering matches planar", "[analysis]") {
    const std::vector<std::vector<float>> planar = {sine(0.5, 4800), sine(0.25, 4800, 700.0)};
    std::vector<float> packed(planar[0].size() * 2);
    for (std::size_t n = 0; n < planar[0].size(); ++n) {
        packed[n * 2] = planar[0][n];
        packed[n * 2 + 1] = planar[1][n];
    }

    ac3::analysis::LevelMeter a{ac3::Acmod::k2_0, false, 48000};
    ac3::analysis::LevelMeter b{ac3::Acmod::k2_0, false, 48000};
    a.process(views(planar));
    b.process_interleaved(packed, 2);

    for (std::size_t ch = 0; ch < 2; ++ch) {
        CHECK(a.levels()[ch].peak_db == Approx(b.levels()[ch].peak_db));
        CHECK(a.levels()[ch].rms_db == Approx(b.levels()[ch].rms_db));
        CHECK(a.summary()[ch].peak == Approx(b.summary()[ch].peak));
        CHECK(a.summary()[ch].samples == b.summary()[ch].samples);
    }

    // Channels the caller does not supply are metered as silence, so they
    // fall away instead of freezing on their last value.
    ac3::analysis::LevelMeter wide{ac3::Acmod::k3_2, true, 48000};
    wide.process_interleaved(packed, 2);
    CHECK(wide.levels()[0].peak_db > -12.0);
    CHECK(wide.levels()[5].peak_db == ac3::analysis::kFloorDb);
    CHECK(wide.summary()[5].samples == 4800);
}

TEST_CASE("energy vector points where the energy is", "[analysis]") {
    using ac3::analysis::ChannelLevel;
    using ac3::analysis::energy_vector;
    const double floor_db = ac3::analysis::kFloorDb;

    std::vector<ChannelLevel> levels(6);
    for (auto& level : levels) {
        level.rms_db = floor_db;
    }

    // Everything in the centre: a hard image dead ahead.
    levels[1].rms_db = 0.0;
    auto vector = energy_vector(levels, ac3::Acmod::k3_2);
    CHECK(vector.azimuth_deg == Approx(0.0).margin(1e-9));
    CHECK(vector.magnitude == Approx(1.0));
    CHECK(vector.level_db == Approx(0.0).margin(1e-9));

    // Equal L and R: the phantom centre, but less focused than a real centre
    // speaker - the two vectors are 60 degrees apart, so the sum shortens by
    // cos(30).
    levels[1].rms_db = floor_db;
    levels[0].rms_db = -6.0;
    levels[2].rms_db = -6.0;
    vector = energy_vector(levels, ac3::Acmod::k3_2);
    CHECK(vector.azimuth_deg == Approx(0.0).margin(1e-9));
    CHECK(vector.magnitude == Approx(std::cos(30.0 * std::numbers::pi / 180.0)));
    CHECK(vector.level_db == Approx(-2.9897).margin(1e-3));  // two equal sources sum to +3 dB

    // Hard left surround.
    levels[0].rms_db = floor_db;
    levels[2].rms_db = floor_db;
    levels[3].rms_db = -20.0;
    vector = energy_vector(levels, ac3::Acmod::k3_2);
    CHECK(vector.azimuth_deg == Approx(110.0));
    CHECK(vector.magnitude == Approx(1.0));
    CHECK(vector.level_db == Approx(-20.0));

    // An even bed is diffuse: still a front bias, because the ring is not
    // symmetric front to back, but nothing like a point source.
    for (int ch = 0; ch < 5; ++ch) {
        levels[static_cast<std::size_t>(ch)].rms_db = 0.0;
    }
    vector = energy_vector(levels, ac3::Acmod::k3_2);
    CHECK(vector.magnitude < 0.5);
    CHECK(vector.azimuth_deg == Approx(0.0).margin(1e-9));

    // Silence has no image at all.
    for (auto& level : levels) {
        level.rms_db = floor_db;
    }
    vector = energy_vector(levels, ac3::Acmod::k3_2);
    CHECK(vector.magnitude == 0.0);
    CHECK(vector.level_db == floor_db);

    // The LFE never steers the image, however loud it gets.
    levels[5].rms_db = 0.0;
    vector = energy_vector(levels, ac3::Acmod::k3_2);
    CHECK(vector.magnitude == 0.0);
}

TEST_CASE("WAV and A/52 channel orders are inverse permutations", "[analysis]") {
    // 5.1 is where they differ: WAV is FL FR FC LFE BL BR, A/52 is
    // L C R SL SR LFE.
    const auto surround = ac3::io::ac3_layout_for(6);
    REQUIRE(surround.has_value());
    CHECK(surround->acmod == ac3::Acmod::k3_2);
    CHECK(surround->lfe);
    CHECK(surround->wav_index == std::vector<std::size_t>{0, 2, 1, 4, 5, 3});
    CHECK(ac3::io::wav_channel_order(ac3::Acmod::k3_2, true) ==
          std::vector<std::size_t>{0, 2, 1, 5, 3, 4});

    CHECK(ac3::io::ac3_layout_for(1)->acmod == ac3::Acmod::k1_0);
    CHECK(ac3::io::ac3_layout_for(2)->acmod == ac3::Acmod::k2_0);
    CHECK(ac3::io::ac3_layout_for(4)->acmod == ac3::Acmod::k2_2);
    CHECK(ac3::io::ac3_layout_for(5)->acmod == ac3::Acmod::k3_2);
    CHECK_FALSE(ac3::io::ac3_layout_for(5)->lfe);
    CHECK_FALSE(ac3::io::ac3_layout_for(0).has_value());
    CHECK_FALSE(ac3::io::ac3_layout_for(7).has_value());

    // Round trip: reading a WAV into A/52 order and writing it back out must
    // put every channel where it started.
    for (std::size_t count = 1; count <= 6; ++count) {
        CAPTURE(count);
        const auto layout = ac3::io::ac3_layout_for(count);
        REQUIRE(layout.has_value());
        const auto order = ac3::io::wav_channel_order(layout->acmod, layout->lfe);
        REQUIRE(order.size() == count);
        for (std::size_t ac3 = 0; ac3 < count; ++ac3) {
            CHECK(order[layout->wav_index[ac3]] == ac3);
        }
    }

    // Layouts with no WAV convention fall back to the codec's own order
    // rather than inventing one.
    CHECK(ac3::io::wav_channel_order(ac3::Acmod::kDualMono, false) ==
          std::vector<std::size_t>{0, 1});
    CHECK(ac3::io::wav_channel_order(ac3::Acmod::k3_1, false) ==
          std::vector<std::size_t>{0, 1, 2, 3});
}
