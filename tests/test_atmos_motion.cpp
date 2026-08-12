#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/motion.hpp"

namespace {

constexpr int kFrame = ac3::kSamplesPerFrame;

// Same helpers as test_atmos.cpp, duplicated here per this project's
// per-file convention (no shared test-utility header exists). Objects that
// are actually distinguishable: different frequencies, different phases,
// none of them silent - silence would make every reconstruction trivially
// "correct" at zero, the exact false pass this project keeps rediscovering.
std::vector<float> tone(double hz, double amplitude, double phase, std::uint64_t start) {
    std::vector<float> out(kFrame);
    for (int n = 0; n < kFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * t + phase));
    }
    return out;
}

std::complex<double> project(std::span<const float> x, double hz) {
    std::complex<double> sum{};
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double angle = -2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0;
        sum += static_cast<double>(x[n]) * std::polar(1.0, angle);
    }
    return sum * (2.0 / static_cast<double>(x.size()));
}

int band_of(double hz, int num_bands_idx) {
    const auto subband = static_cast<std::size_t>(hz / (24000.0 / 64.0));
    return ac3::joc::kSubbandToBand[static_cast<std::size_t>(num_bands_idx)][subband];
}

std::complex<double> reconstruct_at(const ac3::oba::AtmosEncoder& encoder, int object, double hz,
                                    int num_bands_idx) {
    constexpr std::array<int, 5> kAc3FromJoc = {0, 2, 1, 3, 4};
    const int band = band_of(hz, num_bands_idx);
    std::complex<double> sum{};
    for (int channel = 0; channel < 5; ++channel) {
        const double m = encoder.parameters().at(object, channel, band);
        sum += m * project(encoder.bed()[static_cast<std::size_t>(
                               kAc3FromJoc[static_cast<std::size_t>(channel)])],
                           hz);
    }
    return sum;
}

double error_db(std::complex<double> got, std::complex<double> want) {
    return 20.0 * std::log10(std::max(std::abs(got - want), 1e-30) /
                             std::max(std::abs(want), 1e-30));
}

// Builds a KeyframePath that holds each waypoint for hold_frames consecutive
// frame-ends, starting at frame 1 (1-indexed) - so evaluating at frame f's
// end (0-indexed) lands exactly on an authored keyframe for every frame,
// never mid-interpolation, which is what makes the LAST frame of each hold a
// clean, fully settled check point (see the flagship test below).
ac3::oba::KeyframePath make_holds(std::span<const ac3::oba::Position> waypoints, int hold_frames) {
    std::vector<ac3::oba::Keyframe> keyframes;
    int frame_index = 1;
    for (const auto& p : waypoints) {
        for (int h = 0; h < hold_frames; ++h) {
            keyframes.push_back({.time_s = static_cast<double>(frame_index) *
                                           static_cast<double>(kFrame) / 48000.0,
                                 .position = p,
                                 .gain = 1.0,
                                 .lfe_send = 0.0});
            ++frame_index;
        }
    }
    auto created = ac3::oba::KeyframePath::create(std::move(keyframes));
    REQUIRE(created.has_value());
    return std::move(*created);
}

}  // namespace

TEST_CASE("keyframe paths interpolate linearly and hold past the ends", "[atmos][motion]") {
    const auto path = ac3::oba::KeyframePath::create({
        {.time_s = 0.0, .position = {.x = 0.0, .y = 0.0, .z = -1.0}, .gain = 0.2, .lfe_send = 0.0},
        {.time_s = 2.0, .position = {.x = 1.0, .y = 1.0, .z = 1.0}, .gain = 1.0, .lfe_send = 0.5},
    });
    REQUIRE(path.has_value());

    const auto before = path->evaluate(-5.0);
    CHECK(before.position.x == 0.0);
    CHECK(before.gain == 0.2);

    const auto mid = path->evaluate(1.0);
    CHECK_THAT(mid.position.x, Catch::Matchers::WithinAbs(0.5, 1e-12));
    CHECK_THAT(mid.position.y, Catch::Matchers::WithinAbs(0.5, 1e-12));
    CHECK_THAT(mid.position.z, Catch::Matchers::WithinAbs(0.0, 1e-12));
    CHECK_THAT(mid.gain, Catch::Matchers::WithinAbs(0.6, 1e-12));
    CHECK_THAT(mid.lfe_send, Catch::Matchers::WithinAbs(0.25, 1e-12));

    const auto after = path->evaluate(50.0);
    CHECK(after.position.x == 1.0);
    CHECK(after.lfe_send == 0.5);
}

TEST_CASE("a single keyframe holds its placement everywhere", "[atmos][motion]") {
    const auto path = ac3::oba::KeyframePath::create({
        {.time_s = 3.0, .position = {.x = 0.2, .y = 0.8, .z = 0.5}, .gain = 0.7, .lfe_send = 0.1},
    });
    REQUIRE(path.has_value());
    for (const double t : {-10.0, 0.0, 3.0, 999.0}) {
        const auto p = path->evaluate(t);
        CAPTURE(t);
        CHECK(p.position.x == 0.2);
        CHECK(p.position.y == 0.8);
        CHECK(p.gain == 0.7);
    }
}

TEST_CASE("KeyframePath::create rejects no keyframes or a duplicate timestamp",
         "[atmos][motion]") {
    const auto empty = ac3::oba::KeyframePath::create({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == ac3::oba::PathError::kNoKeyframes);

    const auto duplicate = ac3::oba::KeyframePath::create({
        {.time_s = 1.0, .position = {}, .gain = 1.0, .lfe_send = 0.0},
        {.time_s = 1.0, .position = {.x = 0.1}, .gain = 0.5, .lfe_send = 0.0},
    });
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == ac3::oba::PathError::kDuplicateTimestamp);
}

TEST_CASE("make_orbit_path reproduces the closed-form circle", "[atmos][motion]") {
    constexpr double kRateHz = 0.25;
    constexpr double kPhaseRad = 0.3;
    constexpr double kHeight = -0.5;
    const auto path = ac3::oba::make_orbit_path(kRateHz, kPhaseRad, kHeight, 0.6, 0.1);
    for (const double t : {0.0, 0.7, 3.1, 12.5}) {
        CAPTURE(t);
        const double angle = 2.0 * std::numbers::pi * kRateHz * t + kPhaseRad;
        const double want_x = 0.5 + 0.5 * std::sin(angle);
        const double want_y = 0.5 - 0.5 * std::cos(angle);
        const auto got = path.evaluate(t);
        CHECK_THAT(got.position.x, Catch::Matchers::WithinAbs(want_x, 1e-12));
        CHECK_THAT(got.position.y, Catch::Matchers::WithinAbs(want_y, 1e-12));
        CHECK(got.position.z == kHeight);
        CHECK(got.gain == 0.6);
        CHECK(got.lfe_send == 0.1);
    }
}

TEST_CASE("evaluate_placements evaluates every path at one instant, in order",
         "[atmos][motion]") {
    auto keyframe = ac3::oba::KeyframePath::create(
        {{.time_s = 0.0, .position = {.x = 0.1, .y = 0.2, .z = 0.3}, .gain = 0.9}});
    REQUIRE(keyframe.has_value());
    const auto orbit = ac3::oba::make_orbit_path(0.5, 0.0, 0.0, 0.8, 0.0);

    std::vector<ac3::oba::ObjectPath> paths;
    paths.emplace_back(std::move(*keyframe));
    paths.push_back(orbit);

    const auto placement = ac3::oba::evaluate_placements(paths, 1.0);
    REQUIRE(placement.size() == 2);
    CHECK(placement[0].position.x == 0.1);
    CHECK(placement[0].gain == 0.9);
    const auto want_orbit = orbit.evaluate(1.0);
    CHECK(placement[1].position.x == want_orbit.position.x);
    CHECK(placement[1].position.y == want_orbit.position.y);
}

// The flagship regression test. One object, three waypoints, each held for
// three consecutive frames (this project's own convention for "settled,
// not frame 0's fade-in" - see test_atmos.cpp) - and, unlike test_atmos.cpp's
// other tests, this one decodes the REAL bitstream through the in-repo
// Eac3Decoder rather than reading the encoder's pre-bitstream state, because
// the whole point is to prove authored motion survives encoding and comes
// back out, the same way test_spatial.cpp's orbit test proves motion on the
// plain-channel path. The waypoints sit exactly on the 5.1 ring's L, SR and R
// azimuths (see ac3::spatial::pan_room / kRing in spatial.cpp: L +30 degrees,
// SR -110 degrees, R -30 degrees), so each one should pan almost entirely
// into a single bed channel - and L -> SR is a huge jump, deliberately
// stressing the frame-to-frame placement change harder than a smooth orbit
// ever would.
TEST_CASE("a moving object's decoded bed tracks its authored path frame by frame",
         "[atmos][motion]") {
    constexpr ac3::oba::Position kL{.x = 0.25, .y = 0.066987, .z = 0.0};
    constexpr ac3::oba::Position kSR{.x = 0.969846, .y = 0.671010, .z = 0.0};
    constexpr ac3::oba::Position kR{.x = 0.75, .y = 0.066987, .z = 0.0};
    constexpr int kHoldFrames = 3;
    const std::array<ac3::oba::Position, 3> waypoints{kL, kSR, kR};

    std::vector<ac3::oba::ObjectPath> paths;
    paths.emplace_back(make_holds(waypoints, kHoldFrames));

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, 1};
    ac3::Eac3Decoder decoder;
    std::vector<std::span<const float>> views(1);

    const int total_frames = kHoldFrames * static_cast<int>(waypoints.size());
    std::vector<int> argmax_per_frame;
    for (int f = 0; f < total_frames; ++f) {
        const auto start = static_cast<std::uint64_t>(f) * static_cast<std::uint64_t>(kFrame);
        const auto essence = tone(440.0, 0.4, 0.0, start);
        views[0] = essence;

        const double t =
            static_cast<double>(start + static_cast<std::uint64_t>(kFrame)) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, t);
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());

        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());

        double best = -1.0;
        int best_ch = -1;
        for (int ch = 0; ch < 5; ++ch) {  // full-bandwidth channels only, LFE excluded
            double energy = 0.0;
            for (const auto v : (*decoded)->channels[static_cast<std::size_t>(ch)]) {
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

    // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.
    constexpr int kLCh = 0;
    constexpr int kRCh = 2;
    constexpr int kSRCh = 4;
    REQUIRE(argmax_per_frame.size() == 9);
    CHECK(argmax_per_frame[2] == kLCh);
    CHECK(argmax_per_frame[5] == kSRCh);
    CHECK(argmax_per_frame[8] == kRCh);
}

// Two objects, each independently changing position over time - proving
// per-object placement is tracked independently rather than coupled or
// averaged. Reuses this file's reconstruct_at()-style internal-state check
// (test_atmos.cpp's own convention for AtmosEncoder: the encoder's own
// pre-quantization matrix applied to its own bed) rather than a second full
// decode pipeline, since what is being guarded here is separation surviving
// motion, not motion surviving the bitstream (the flagship test above
// already covers that).
TEST_CASE("two independently moving objects keep reconstructing cleanly", "[atmos][motion]") {
    constexpr ac3::oba::Position kFrontLeft{.x = 0.0, .y = 0.0, .z = 0.0};
    constexpr ac3::oba::Position kFrontRight{.x = 1.0, .y = 0.0, .z = 0.0};
    constexpr ac3::oba::Position kBackLeftUp{.x = 0.0, .y = 1.0, .z = 1.0};
    constexpr ac3::oba::Position kBackRightUp{.x = 1.0, .y = 1.0, .z = 1.0};
    constexpr int kHoldFrames = 3;
    constexpr double kHzA = 311.0;
    constexpr double kHzB = 997.0;
    REQUIRE(band_of(kHzA, 4) != band_of(kHzB, 4));

    // Object A: front-left, then swaps to the diagonally opposite corner.
    // Object B: front-right, then swaps to ITS diagonally opposite corner -
    // never sharing a direction with A at either interval.
    const std::array<ac3::oba::Position, 2> waypoints_a{kFrontLeft, kBackRightUp};
    const std::array<ac3::oba::Position, 2> waypoints_b{kFrontRight, kBackLeftUp};

    std::vector<ac3::oba::ObjectPath> paths;
    paths.emplace_back(make_holds(waypoints_a, kHoldFrames));
    paths.emplace_back(make_holds(waypoints_b, kHoldFrames));

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 640}, 2};
    std::vector<std::span<const float>> views(2);

    const int total_frames = kHoldFrames * static_cast<int>(waypoints_a.size());
    std::vector<std::vector<float>> essence_at_check;  // [checked interval][object]
    const std::array<int, 2> kCheckFrames{2, 5};

    for (int f = 0; f < total_frames; ++f) {
        const auto start = static_cast<std::uint64_t>(f) * static_cast<std::uint64_t>(kFrame);
        auto essence_a = tone(kHzA, 0.30, 0.0, start);
        auto essence_b = tone(kHzB, 0.25, 0.9, start);
        views[0] = essence_a;
        views[1] = essence_b;

        const double t =
            static_cast<double>(start + static_cast<std::uint64_t>(kFrame)) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, t);
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());

        if (f == kCheckFrames[0] || f == kCheckFrames[1]) {
            CAPTURE(f);
            const auto want_a = project(essence_a, kHzA);
            const auto want_b = project(essence_b, kHzB);
            const auto got_a = reconstruct_at(encoder, 0, kHzA, 4);
            const auto got_b = reconstruct_at(encoder, 1, kHzB, 4);
            CHECK(error_db(got_a, want_a) < -20.0);
            CHECK(error_db(got_b, want_b) < -20.0);

            // Cross-leak: each object's tone should barely register in the
            // OTHER object's reconstruction.
            const auto leak_a_in_b = reconstruct_at(encoder, 1, kHzA, 4);
            const auto leak_b_in_a = reconstruct_at(encoder, 0, kHzB, 4);
            CHECK(20.0 * std::log10(std::max(std::abs(leak_a_in_b), 1e-30) /
                                    std::abs(want_a)) < -20.0);
            CHECK(20.0 * std::log10(std::max(std::abs(leak_b_in_a), 1e-30) /
                                    std::abs(want_b)) < -20.0);
        }
    }
}
