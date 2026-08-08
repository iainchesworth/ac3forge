// Place a mono source in the room and move it, rendering to a 5.1 AC-3 bed.
//
// spatial::BedRenderer is the plain-AC-3 object path: objects are panned onto
// the ITU-R BS.775 ring and the result is an ordinary 5.1 stream. Nothing
// survives about where the object was — for that, see atmos_objects.cpp.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/spatial/spatial.hpp"

int main() {
    ac3::FrameEncoder encoder{{
        .bitrate_kbps = 448,
        .acmod = ac3::Acmod::k3_2,
        .lfe = true,
    }};

    ac3::spatial::BedRenderer renderer;
    // add_object allocates, so call it before rendering starts.
    const std::size_t object = renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7});

    // The renderer works a block at a time (256 samples) because that is the
    // rate automation is clocked at; gains ramp linearly within a block, so
    // moving an object does not click.
    std::array<float, ac3::spatial::kBlockSamples> source{};
    std::vector<std::vector<float>> bed(6, std::vector<float>(ac3::kSamplesPerFrame));

    std::vector<std::span<const float>> bed_views;
    for (const auto& channel : bed) {
        bed_views.emplace_back(channel);
    }

    std::vector<std::byte> stream;
    long sample_index = 0;
    for (int frame = 0; frame < 62; ++frame) {  // two seconds
        for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
            // One full turn every two seconds.
            const double seconds = static_cast<double>(sample_index) / 48000.0;
            renderer.set_target(object, {.azimuth_deg = 180.0 * seconds, .gain = 0.7});

            for (auto& sample : source) {
                sample = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * 440.0 *
                             static_cast<double>(sample_index) / 48000.0));
                ++sample_index;
            }

            // Six writable 256-sample spans into this block of the frame:
            // L, C, R, SL, SR, LFE. render_block overwrites them.
            std::array<std::span<float>, 6> block_out{};
            for (std::size_t ch = 0; ch < 6; ++ch) {
                block_out[ch] = std::span<float>{bed[ch]}.subspan(
                    static_cast<std::size_t>(block * ac3::spatial::kBlockSamples),
                    ac3::spatial::kBlockSamples);
            }
            const std::array<std::span<const float>, 1> audio{std::span<const float>{source}};
            renderer.render_block(audio, block_out);
        }

        const auto encoded = encoder.encode_frame(bed_views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }

    std::printf("%zu bytes of 5.1 AC-3 with a moving source\n", stream.size());
    return 0;
}
