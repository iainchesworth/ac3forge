#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/spatial/spatial.hpp"

namespace {

constexpr int kL = 0;
constexpr int kC = 1;
constexpr int kR = 2;
constexpr int kSL = 3;
constexpr int kSR = 4;

double sum_sq(const ac3::spatial::PanGains& g) {
    double total = 0.0;
    for (const auto v : g) {
        total += v * v;
    }
    return total;
}

}  // namespace

TEST_CASE("panning at speaker azimuths hits exactly that speaker", "[spatial]") {
    const std::array<std::pair<double, int>, 5> cases = {{
        {0.0, kC}, {30.0, kL}, {110.0, kSL}, {-110.0, kSR}, {-30.0, kR},
    }};
    for (const auto& [azimuth, channel] : cases) {
        CAPTURE(azimuth);
        const auto gains = ac3::spatial::pan_azimuth(azimuth);
        for (int ch = 0; ch < ac3::spatial::kBedChannels; ++ch) {
            CAPTURE(ch);
            if (ch == channel) {
                CHECK(std::abs(gains[static_cast<std::size_t>(ch)] - 1.0) < 1e-12);
            } else {
                CHECK(gains[static_cast<std::size_t>(ch)] < 1e-12);
            }
        }
    }
}

TEST_CASE("panning preserves energy and uses at most two speakers", "[spatial]") {
    for (int deg = 0; deg < 360; ++deg) {
        CAPTURE(deg);
        const auto gains = ac3::spatial::pan_azimuth(static_cast<double>(deg));
        CHECK(std::abs(sum_sq(gains) - 1.0) < 1e-12);
        int active = 0;
        for (const auto g : gains) {
            active += g > 1e-12 ? 1 : 0;
        }
        CHECK(active <= 2);
    }
    // The L/C bisector splits equally.
    const auto mid = ac3::spatial::pan_azimuth(15.0);
    CHECK(std::abs(mid[kL] - mid[kC]) < 1e-9);
}

TEST_CASE("renderer: static object, LFE send, and ramping", "[spatial]") {
    ac3::spatial::BedRenderer renderer;
    const auto object = renderer.add_object({.azimuth_deg = 0.0, .gain = 0.8, .lfe_send = 0.25});

    std::vector<float> mono(ac3::spatial::kBlockSamples);
    for (std::size_t n = 0; n < mono.size(); ++n) {
        mono[n] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / 48000.0));
    }
    std::array<std::vector<float>, 6> bed;
    for (auto& channel : bed) {
        channel.assign(ac3::spatial::kBlockSamples, 0.0f);
    }
    const std::array<std::span<const float>, 1> audio = {mono};
    const std::array<std::span<float>, 6> bed_views = {bed[0], bed[1], bed[2],
                                                       bed[3], bed[4], bed[5]};
    renderer.render_block(audio, bed_views);

    for (std::size_t n = 0; n < mono.size(); ++n) {
        CHECK(std::abs(bed[kC][n] - 0.8f * mono[n]) < 1e-6f);   // center only
        CHECK(std::abs(bed[5][n] - 0.25f * 0.8f * mono[n]) < 1e-6f);  // LFE send
        CHECK(bed[kL][n] == 0.0f);
        CHECK(bed[kR][n] == 0.0f);
    }

    // Move the object hard from C to R between blocks: the ramp must keep
    // sample-to-sample steps small (no zipper click).
    renderer.set_target(object, {.azimuth_deg = -30.0, .gain = 0.8, .lfe_send = 0.25});
    renderer.render_block(audio, bed_views);
    float worst_step = 0.0f;
    for (std::size_t n = 1; n < mono.size(); ++n) {
        worst_step = std::max(worst_step, std::abs(bed[kC][n] - bed[kC][n - 1]));
        worst_step = std::max(worst_step, std::abs(bed[kR][n] - bed[kR][n - 1]));
    }
    // Tone's own slope ~0.029/sample; the ramp adds ~0.8/256 ~ 0.003.
    CHECK(worst_step < 0.05f);
    // By block end the object is fully on R.
    CHECK(std::abs(bed[kC].back()) < 1e-3f);
}

TEST_CASE("orbiting object lands in the right channels end to end", "[spatial]") {
    // The object PARKS at each speaker's azimuth for one full frame (ring
    // order C, L, SL, SR, R), ramping between frames; render -> encode
    // 3/2+LFE -> in-repo decode -> each frame's dominant channel must be
    // the parked speaker. (A free-running orbit makes the expectation
    // ambiguous near pair boundaries and the 256-sample decode delay.)
    ac3::spatial::BedRenderer renderer;
    const auto object = renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7});
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    ac3::FrameDecoder decoder;

    constexpr std::array<double, 5> kParkAzimuth = {0.0, 30.0, 110.0, 250.0, 330.0};
    constexpr int kFrames = 5;
    std::vector<float> mono(ac3::spatial::kBlockSamples);
    std::array<std::vector<float>, 6> frame_channels;
    std::array<std::vector<float>, 6> bed_block;
    for (auto& channel : bed_block) {
        channel.assign(ac3::spatial::kBlockSamples, 0.0f);
    }
    std::vector<int> argmax_per_frame;
    std::uint64_t n0 = 0;
    for (int f = 0; f < kFrames; ++f) {
        for (auto& channel : frame_channels) {
            channel.clear();
        }
        renderer.set_target(
            object, {.azimuth_deg = kParkAzimuth[static_cast<std::size_t>(f)], .gain = 0.7});
        for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
            for (std::size_t n = 0; n < mono.size(); ++n) {
                mono[n] = static_cast<float>(
                    0.6 * std::sin(2.0 * std::numbers::pi * 440.0 *
                                   static_cast<double>(n0 + n) / 48000.0));
            }
            n0 += mono.size();
            const std::array<std::span<const float>, 1> audio = {mono};
            const std::array<std::span<float>, 6> bed_views = {bed_block[0], bed_block[1],
                                                               bed_block[2], bed_block[3],
                                                               bed_block[4], bed_block[5]};
            renderer.render_block(audio, bed_views);
            for (std::size_t ch = 0; ch < 6; ++ch) {
                frame_channels[ch].insert(frame_channels[ch].end(), bed_block[ch].begin(),
                                          bed_block[ch].end());
            }
        }
        std::vector<std::span<const float>> views;
        views.reserve(6);
        for (const auto& channel : frame_channels) {
            views.emplace_back(channel);
        }
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        const auto decoded = decoder.decode_frame(*frame);
        REQUIRE(decoded.has_value());
        double best = -1.0;
        int best_ch = -1;
        for (int ch = 0; ch < 5; ++ch) {  // fullbw channels only
            double energy = 0.0;
            for (const auto v : decoded->channels[static_cast<std::size_t>(ch)]) {
                const double sd = static_cast<double>(v);
                energy += sd * sd;
            }
            if (energy > best) {
                best = energy;
                best_ch = ch;
            }
        }
        argmax_per_frame.push_back(best_ch);
    }
    CHECK(argmax_per_frame == std::vector<int>{kC, kL, kSL, kSR, kR});
}
