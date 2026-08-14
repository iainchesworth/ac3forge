// Drive Atmos objects from authored motion instead of hand-rolled trig.
//
// atmos_objects.cpp computes each object's position with its own per-frame
// sin/cos math. ac3::oba::motion is the shared layer that replaces that: an
// OrbitPath is a closed-form circle, a KeyframePath is sparse authored points
// linearly interpolated between them (and held at the ends), and
// evaluate_placements turns either kind into the ObjectPlacement span
// AtmosEncoder::encode_frame wants, one call per frame.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"

int main() {
    constexpr int kObjects = 2;

    // Object 0: a closed-form orbit, one revolution every two seconds, held
    // at half height.
    const auto orbit = ac3::oba::make_orbit_path(/*rate_hz=*/0.5, /*phase_rad=*/0.0,
                                                 /*height=*/0.5, /*gain=*/0.6, /*lfe_send=*/0.0);

    // Object 1: three authored cues - enters silent at the left wall, swells
    // to full gain crossing the front centre, fades out exiting right. Before
    // 0.0s and after 1.6s it holds at its nearest keyframe rather than
    // extrapolating.
    auto keyframed = ac3::oba::KeyframePath::create({
        {.time_s = 0.0, .position = {.x = 0.0, .y = 0.5, .z = 0.0}, .gain = 0.0},
        {.time_s = 0.8, .position = {.x = 0.5, .y = 0.9, .z = 0.0}, .gain = 0.8},
        {.time_s = 1.6, .position = {.x = 1.0, .y = 0.5, .z = 0.0}, .gain = 0.0},
    });
    if (!keyframed) {
        std::printf("KeyframePath::create failed\n");
        return 1;
    }

    const std::array<ac3::oba::ObjectPath, kObjects> paths{orbit, ac3::oba::ObjectPath{*keyframed}};

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};

    std::vector<std::vector<float>> sources(kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (const auto& source : sources) {
        views.emplace_back(source);
    }
    constexpr std::array<double, kObjects> tones{440.0, 880.0};

    std::vector<std::byte> stream;
    for (int frame = 0; frame < 93; ++frame) {  // three seconds
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[obj][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * tones[obj] * t));
            }
        }

        const double seconds = frame * ac3::kSamplesPerFrame / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, seconds);

        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("atmos encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    std::printf("%zu bytes of DD+ with %d scripted objects over a 5.1 bed\n", stream.size(),
                encoder.dynamic_object_count());
    return 0;
}
