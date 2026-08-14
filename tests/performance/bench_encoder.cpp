// Performance-trend data producer: the OTHER half of the performance suite,
// alongside test_performance.cpp's hard real-time gate (ac3perf). That gate
// answers "did the encoder stay faster than real time" (pass/fail); this
// answers "how much faster, and is that number drifting" - a trend signal,
// not a threshold, the same relationship docs/quality-trend.md's gold-
// reference gate has to scripts/append-quality-history.py.
//
// Not a Catch2 binary on purpose: nothing here asserts anything. It runs
// each configuration for a fixed frame count and writes one JSON record per
// configuration to --json-out, for scripts/append-performance-history.py to
// append to the performance-history data (docs/performance-trend.md) the
// same way compare_wav.py's --json-out feeds append-quality-history.py.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

constexpr int kFrames = 200;
constexpr double kSampleRate = 48000.0;

double real_time_budget_ms(int frames) {
    return 1000.0 * static_cast<double>(frames) * ac3::kSamplesPerFrame / kSampleRate;
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

struct Result {
    std::string name;
    int frames = 0;
    double total_ms = 0.0;
    double ms_per_frame = 0.0;
};

Result bench_plain_51(bool fast_mdct = false) {
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .fast_mdct = fast_mdct}};
    std::uint64_t n = 0;
    const std::vector<float> samples = tone_frame(n, 440.0, 0.3);
    const std::vector<std::span<const float>> views(
        static_cast<std::size_t>(encoder.channel_count()), samples);

    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(views);
        if (!result) {
            std::fprintf(stderr, "bench_plain_51: encode_frame failed\n");
            std::exit(1);
        }
    }
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start);
    return {.name = fast_mdct ? "plain_51_fast_mdct" : "plain_51", .frames = kFrames,
            .total_ms = elapsed_ms.count(), .ms_per_frame = elapsed_ms.count() / kFrames};
}

Result bench_atmos_4obj(bool fast_mdct = false) {
    constexpr int kObjects = 4;
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .fast_mdct = fast_mdct}, kObjects};

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
        if (!result) {
            std::fprintf(stderr, "bench_atmos_4obj: encode_frame failed\n");
            std::exit(1);
        }
    }
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start);
    return {.name = fast_mdct ? "atmos_4obj_fast_mdct" : "atmos_4obj", .frames = kFrames,
            .total_ms = elapsed_ms.count(), .ms_per_frame = elapsed_ms.count() / kFrames};
}

void write_json(const std::vector<Result>& results, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"real_time_budget_ms_per_frame\": " << real_time_budget_ms(1)
        << ",\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"frames\": " << r.frames
            << ", \"total_ms\": " << r.total_ms << ", \"ms_per_frame\": " << r.ms_per_frame
            << "}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        }
    }

    const std::vector<Result> results{bench_plain_51(), bench_plain_51(/*fast_mdct=*/true),
                                      bench_atmos_4obj(), bench_atmos_4obj(/*fast_mdct=*/true)};

    for (const auto& r : results) {
        std::printf("%-12s %5d frames  %8.3f ms total  %6.3f ms/frame  (budget %.3f ms/frame)\n",
                    r.name.c_str(), r.frames, r.total_ms, r.ms_per_frame,
                    real_time_budget_ms(1));
    }

    if (!json_out.empty()) {
        write_json(results, json_out);
        std::printf("wrote %s\n", json_out.c_str());
    }

    return 0;
}
