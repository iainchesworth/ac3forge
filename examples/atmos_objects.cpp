// Encode objects as Dolby Atmos in Dolby Digital Plus (ETSI TS 103 420).
//
// The output is one ordinary 5.1 E-AC-3 stream. Objects are panned into the
// bed, which a legacy decoder plays unchanged; the OAMD and JOC payloads ride
// beside it in an EMDF container saying where each object is and how to pull
// it back out. See docs/LIBRARY.md for what a decoder will and will not do
// with them.

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

int main() {
    constexpr int kObjects = 3;
    // Object metadata competes with the mantissas for the same frame, so an
    // object stream wants more headroom than a plain 5.1 one.
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};

    std::vector<std::vector<float>> sources(
        kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (const auto& source : sources) {
        views.emplace_back(source);
    }

    constexpr std::array<double, kObjects> tones{440.0, 880.0, 1320.0};
    std::vector<std::byte> stream;

    for (int frame = 0; frame < 62; ++frame) {  // two seconds
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[obj][static_cast<std::size_t>(n)] = static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * tones[obj] * t));
            }
        }

        // Positions are room-anchored per §4.2.1: x 0 at the left wall to 1 at
        // the right, y 0 front to 1 back, z 0 floor to 1 ceiling. Each object
        // circles at its own rate and height.
        const double seconds = frame * ac3::kSamplesPerFrame / 48000.0;
        std::array<ac3::oba::ObjectPlacement, kObjects> placement{};
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            const double angle = 2.0 * std::numbers::pi * seconds / (2.0 + obj);
            placement[obj] = {
                .position = {.x = 0.5 + 0.45 * std::cos(angle),
                             .y = 0.5 + 0.45 * std::sin(angle),
                             .z = 0.25 * static_cast<double>(obj)},
                .gain = 1.0,
            };
        }

        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("atmos encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    std::printf("%zu bytes of DD+ with %d objects over a 5.1 bed\n", stream.size(),
                encoder.dynamic_object_count());
    return 0;
}
