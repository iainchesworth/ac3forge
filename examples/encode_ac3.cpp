// Encode PCM to an AC-3 elementary stream.
//
// Builds a 5.1 layout (3/2 + LFE) carrying a different tone in each channel,
// encodes one second of it, and reports the size. Every example in this
// directory backs the excerpts in the docs/library/ pages; they live here so
// the build checks them.

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
#include "ac3/encoder/encoder.hpp"

namespace {

constexpr std::array<double, 6> kTones{1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};

// Real audio from the first frame onward matters: an all-zero frame takes the
// §7.2.2.1.1 all-zero bit-allocation path and exercises almost none of the
// encoder. See CONTRIBUTING.md on why silence is a bad test signal.
void fill_with_audio(std::vector<std::vector<float>>& pcm, int frame, double rate) {
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const double t = (frame * ac3::kSamplesPerFrame + n) / rate;
            pcm[ch][static_cast<std::size_t>(n)] = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
        }
    }
}

void write(std::vector<std::byte>& stream, std::span<const std::byte> frame) {
    stream.insert(stream.end(), frame.begin(), frame.end());
}

}  // namespace

int main() {
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
        .bitrate_kbps = 448,
        .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
        .lfe = true,
    });

    // Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    // encode_frame takes a span of spans, so the views must outlive the call.
    // Build them once and refill the buffers underneath each frame.
    const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

    std::vector<std::byte> stream;
    for (int frame = 0; frame < 31; ++frame) {  // 48000 / 1536, near enough
        fill_with_audio(pcm, frame, 48000.0);

        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        write(stream, *encoded);  // one complete syncframe
    }

    std::printf("%zu bytes of AC-3\n", stream.size());
    return 0;
}
