// Decode a stream that has one damaged frame in the middle of otherwise-good
// ones - the shape real capture/transport corruption takes, since a torn or
// bit-flipped frame does not usually take its neighbours down with it.
//
// ac3::split_frames delimits syncframes by sync word and declared size alone,
// so it still finds every frame boundary correctly even though frame 4's
// payload is corrupt; only that frame's own decode_frame call fails. A
// caller can then skip exactly the damaged frame and keep decoding, rather
// than aborting the whole stream on its account.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"

int main() {
    constexpr int kFrameCount = 8;
    constexpr int kCorruptFrame = 4;

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    constexpr std::array<double, 2> tones{440.0, 660.0};

    std::vector<std::byte> stream;
    std::vector<std::size_t> frame_offsets;
    for (int frame = 0; frame < kFrameCount; ++frame) {
        for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                pcm[ch][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * tones[ch] * t));
            }
        }
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        frame_offsets.push_back(stream.size());
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }
    frame_offsets.push_back(stream.size());  // end of the last frame

    // Flip a bit well inside the payload - past the sync word and frame-size
    // fields split_frames reads, so the corruption cannot move a boundary,
    // only break that one frame's own CRC/bit-allocation checks.
    const std::size_t corrupt_at =
        frame_offsets[static_cast<std::size_t>(kCorruptFrame)] +
        (frame_offsets[static_cast<std::size_t>(kCorruptFrame) + 1] -
         frame_offsets[static_cast<std::size_t>(kCorruptFrame)]) / 2;
    stream[corrupt_at] ^= std::byte{0xFF};

    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::printf("split_frames failed: %.*s\n",
                    static_cast<int>(ac3::describe(frames.error()).size()),
                    ac3::describe(frames.error()).data());
        return 1;
    }
    std::printf("%zu frame(s) delimited, corruption at byte %zu\n", frames->size(), corrupt_at);

    ac3::FrameDecoder decoder;
    int recovered = 0;
    int failed = 0;
    for (std::size_t i = 0; i < frames->size(); ++i) {
        const auto decoded = decoder.decode_frame((*frames)[i]);
        if (!decoded) {
            const auto message = ac3::describe(decoded.error());
            std::printf("frame %zu: decode failed (%.*s) - skipping\n", i,
                        static_cast<int>(message.size()), message.data());
            ++failed;
            continue;
        }
        ++recovered;
    }

    std::printf("%d of %zu frames recovered, %d skipped\n", recovered, frames->size(), failed);
    return (recovered == kFrameCount - 1 && failed == 1) ? 0 : 1;
}
