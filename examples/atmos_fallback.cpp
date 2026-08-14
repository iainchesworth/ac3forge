// AtmosConfig::emit_object_metadata: objects, or nothing - never both.
//
// A decoder that VALIDATES the EMDF container's protection field treats its
// sync word as a commitment to object decoding and refuses the whole stream
// if the tag does not check out (see ac3::signing and
// docs/concepts/object-signing.md for how a licensed decoder is satisfied).
// With the container left out entirely there is no sync word for such a
// decoder to find, so it falls back to the 5.1 bed underneath exactly as it
// would for a stream that was never object-based at all. This example builds
// the same programme both ways and shows what each costs and what each still
// decodes as.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

constexpr int kObjects = 2;
constexpr int kFrames = 31;  // one second

// Encodes the same two-object programme under the given config and returns
// the assembled stream plus the last frame's decoded 5.1 bed.
std::pair<std::vector<std::byte>, ac3::DecodedAccessUnit> run(const ac3::oba::AtmosConfig& config) {
    ac3::oba::AtmosEncoder encoder{config, kObjects};
    ac3::Eac3Decoder decoder;

    std::vector<std::vector<float>> sources(kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (const auto& source : sources) {
        views.emplace_back(source);
    }
    const std::array<ac3::oba::ObjectPlacement, kObjects> placement{{
        {.position = {.x = 0.25, .y = 0.6, .z = 0.0}, .gain = 0.8},
        {.position = {.x = 0.75, .y = 0.6, .z = 0.0}, .gain = 0.8},
    }};
    constexpr std::array<double, kObjects> tones{440.0, 880.0};

    std::vector<std::byte> stream;
    ac3::DecodedAccessUnit bed;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[obj][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * tones[obj] * t));
            }
        }
        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("atmos encode failed: %d\n", std::to_underlying(unit.error()));
            return {};
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());

        // Every frame here decodes immediately - this encoder never sets
        // transient pre-noise, the one thing that would hold a result back.
        const auto decoded = decoder.decode_access_unit(unit->bytes);
        if (decoded && decoded->has_value()) {
            bed = **decoded;
        }
    }
    return {std::move(stream), std::move(bed)};
}

}  // namespace

int main() {
    const auto [with_container, bed_with] = run({.bitrate_kbps = 448, .emit_object_metadata = true});
    const auto [without_container, bed_without] =
        run({.bitrate_kbps = 448, .emit_object_metadata = false});

    std::printf("with container:    %zu bytes, bed %d channels, dialnorm %d\n", with_container.size(),
                static_cast<int>(bed_with.channels.size()), bed_with.dialnorm);
    std::printf("without container: %zu bytes, bed %d channels, dialnorm %d\n",
                without_container.size(), static_cast<int>(bed_without.channels.size()),
                bed_without.dialnorm);

    // This is CBR: frmsiz follows bitrate_kbps either way, so the two streams
    // come out the SAME size - the container rides in bits the mantissas
    // would otherwise have had, rather than adding to the frame. The 5.1 mix
    // is not bit-identical between them for exactly that reason: leaving the
    // container out gives those bits back to the bed's own coding.
    double max_bed_diff = 0.0;
    if (bed_with.channels.size() == bed_without.channels.size()) {
        for (std::size_t ch = 0; ch < bed_with.channels.size(); ++ch) {
            for (std::size_t n = 0; n < bed_with.channels[ch].size(); ++n) {
                const double diff =
                    std::abs(static_cast<double>(bed_with.channels[ch][n]) -
                             static_cast<double>(bed_without.channels[ch][n]));
                max_bed_diff = std::max(max_bed_diff, diff);
            }
        }
    }
    std::printf("same %zu bytes either way (CBR); largest bed sample difference: %.5f\n",
                with_container.size(), max_bed_diff);

    // Both decode as an ordinary 5.1 bed through this project's own decoder,
    // which - like any decoder that ignores the container - never looks for
    // emdf_protection at all. Only a decoder that DOES validate it treats the
    // two streams differently, and that is the whole point of the toggle.
    return (bed_with.channels.size() == 6 && bed_without.channels.size() == 6) ? 0 : 1;
}
