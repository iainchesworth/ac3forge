#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/oba/atmos.hpp"

// Real-time throughput regression guard.
//
// AtmosEncoder::encode_frame() once measured at ~266ms per 32ms-budget frame
// on real hardware (an NVIDIA Shield's ARM SoC, building the Android
// platform backend) - traced with Tracy to the forward MDCT recomputing
// std::cos() fresh inside an O(N^2) loop on every call, instead of using a
// precomputed table the way the inverse transform right next to it already
// did (see src/lib/src/core/mdct.cpp's ForwardCosTable). Every other test in
// this suite asserts correctness, not throughput, so nothing would have
// caught a regression like that - this file exists specifically to fail
// loudly if the encoder ever stops being faster than real time again, from
// two different entry points into the shared MDCT (the plain codec path and
// the Atmos/JOC one).
//
// Kept in a separate ac3perf binary/target, not folded into ac3tests: a
// throughput assertion is a different kind of check from the rest of the
// suite (environment-sensitive, meant to be read as a number as much as a
// pass/fail, and not something a correctness-only run should have to carry).
// See tests/performance/CMakeLists.txt.
//
// Not run at all under ASan/UBSan: instrumented code is not a throughput
// signal worth having an opinion about, at any slack factor - see this
// target's LABELS "Performance" and CMakePresets.json's
// test-linux-llvm-asan-ubsan preset, which excludes that label outright.
//
// The threshold is 2x real time, not 1x: real time is the actual functional
// requirement (a live/streaming caller - ac3cli's `live` command, or the
// Shield app's encode loop - cannot keep up otherwise), and 2x leaves
// headroom for a CI runner that is simply slower than a dev machine, without
// giving up on catching the class of regression this guards against - the
// bug it is named for was 8-28x over budget, not a marginal miss.

namespace {

constexpr int kFrames = 100;
constexpr double kSampleRate = 48000.0;
constexpr double kSlackFactor = 2.0;

double real_time_budget_seconds(int frames) {
    return static_cast<double>(frames) * ac3::kSamplesPerFrame / kSampleRate;
}

std::vector<float> tone_frame(std::uint64_t& n, double freq, double amplitude) {
    std::vector<float> samples(ac3::kSamplesPerFrame);
    for (auto& s : samples) {
        s = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freq *
                                                     static_cast<double>(n) / kSampleRate));
        ++n;
    }
    return samples;
}

}  // namespace

TEST_CASE("the plain 5.1 encoder stays faster than real time") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    std::uint64_t n = 0;
    const std::vector<float> samples = tone_frame(n, 440.0, 0.3);
    const std::vector<std::span<const float>> views(
        static_cast<std::size_t>(encoder.channel_count()), samples);

    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(views);
        REQUIRE(result.has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const double budget = real_time_budget_seconds(kFrames);

    INFO("encoded " << kFrames << " frames in " << elapsed.count() << "s ("
                    << (1000.0 * elapsed.count() / kFrames)
                    << "ms/frame); real-time budget is " << budget << "s");
    CHECK(elapsed.count() < kSlackFactor * budget);
}

TEST_CASE("the Atmos/JOC encoder stays faster than real time") {
    constexpr int kObjects = 4;
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};

    std::uint64_t n = 0;
    std::vector<std::vector<float>> sources;
    sources.reserve(kObjects);
    for (int obj = 0; obj < kObjects; ++obj) {
        sources.push_back(tone_frame(n, 220.0 * static_cast<double>(obj + 1), 0.2));
    }
    std::vector<std::span<const float>> views;
    views.reserve(kObjects);
    for (const auto& source : sources) {
        views.emplace_back(source);
    }
    std::vector<ac3::oba::ObjectPlacement> placement(static_cast<std::size_t>(kObjects));
    for (int obj = 0; obj < kObjects; ++obj) {
        placement[static_cast<std::size_t>(obj)] = {
            .position = {.x = 0.2 + 0.2 * obj, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }

    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(views, placement);
        REQUIRE(result.has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const double budget = real_time_budget_seconds(kFrames);

    INFO("encoded " << kFrames << " frames (" << kObjects << " objects) in " << elapsed.count()
                    << "s (" << (1000.0 * elapsed.count() / kFrames)
                    << "ms/frame); real-time budget is " << budget << "s");
    CHECK(elapsed.count() < kSlackFactor * budget);
}
