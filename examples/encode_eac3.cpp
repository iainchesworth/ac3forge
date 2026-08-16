// Encode PCM to E-AC-3 (Dolby Digital Plus), both shapes:
//
//   1. FrameEncoder      — one independent substream, up to 5.1.
//   2. AccessUnitEncoder — an independent 5.1 bed plus dependent substreams
//                          carrying the channels that do not fit in it. 7.1.4
//                          needs two dependents, which is the case no external
//                          decoder will read (see docs/library/encoding-eac3.md).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"

namespace {

// Fills each channel with its own tone and returns views onto the buffers.
void fill_tones(std::vector<std::vector<float>>& pcm, std::span<const double> tones,
                int frame, double rate) {
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const double t = (frame * ac3::kSamplesPerFrame + n) / rate;
            pcm[ch][static_cast<std::size_t>(n)] = static_cast<float>(
                0.4 * std::sin(2.0 * std::numbers::pi * tones[ch] * t));
        }
    }
}

std::vector<std::span<const float>> views_of(const std::vector<std::vector<float>>& pcm) {
    std::vector<std::span<const float>> views;
    views.reserve(pcm.size());
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    return views;
}

// --- 1. a single substream -------------------------------------------------
int encode_stereo() {
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::eac3::FrameEncoder>(ac3::eac3::FrameConfig{
        .bitrate_kbps = 192,
        .acmod = ac3::Acmod::k2_0,
    });

    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    const auto views = views_of(pcm);
    constexpr std::array<double, 2> tones{440.0, 660.0};

    std::vector<std::byte> stream;
    for (int frame = 0; frame < 31; ++frame) {
        fill_tones(pcm, tones, frame, 48000.0);
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("stereo encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }
    std::printf("stereo: %zu bytes\n", stream.size());
    return 0;
}

// --- 2. an access unit: 5.1 bed + two dependents = 7.1.4 -------------------
int encode_714() {
    // The bed is self-sufficient: a decoder that reads only the independent
    // substream gets a complete 5.1 programme.
    ac3::eac3::AccessUnitConfig config;
    config.independent = {
        .bitrate_kbps = 384,
        .acmod = ac3::Acmod::k3_2,
        .lfe = true,
    };
    // Each dependent gets its own slice of the rate — substreams share a frame
    // period, not a frame — and a Table E2.5 chanmap naming where its channels
    // belong. Per §E3.8.2 the locations that collide with the bed replace it
    // and the rest extend the layout.
    config.dependents.push_back({
        .bitrate_kbps = 192,
        .acmod = ac3::Acmod::k2_2,
        .chanmap = ac3::eac3::chanmap::k71Rear,  // Ls, Rs, Lrs, Rrs
    });
    config.dependents.push_back({
        .bitrate_kbps = 192,
        .acmod = ac3::Acmod::k2_2,
        .chanmap = ac3::eac3::chanmap::kTopQuad,  // Vhl, Vhr, Lts, Rts
    });

    ac3::eac3::AccessUnitEncoder encoder{config};

    // Channels are grouped by substream in transmission order: the
    // independent's first in Table 5.8 order with LFE last, then each
    // dependent's in the order its chanmap names them.
    const auto channel_count = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<float>> pcm(channel_count,
                                        std::vector<float>(ac3::kSamplesPerFrame));
    const auto views = views_of(pcm);
    const std::vector<double> tones{1000.0, 800.0,  1200.0, 600.0,  1400.0, 60.0,
                                    500.0,  1600.0, 400.0,  1800.0, 2000.0, 2400.0,
                                    2800.0, 3200.0};

    std::vector<std::byte> stream;
    for (int frame = 0; frame < 31; ++frame) {
        fill_tones(pcm, tones, frame, 48000.0);
        const auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::printf("7.1.4 encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        // unit->bytes is the wire order already; substream_bytes records the
        // per-substream boundaries, which crc2 is computed over.
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    std::printf("7.1.4: %zu channels, %zu bytes\n", channel_count, stream.size());
    return 0;
}

}  // namespace

int main() {
    if (encode_stereo() != 0) {
        return 1;
    }
    return encode_714();
}
