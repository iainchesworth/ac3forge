// Combine two separate sources onto one coded stream by explicit assignment.
//
// plan::route()'s other overload places ONE source by direction - the
// microphone-onto-5.1 case. A caller with several sources - here a stereo
// music bed and a separate mono voiceover - instead says exactly where each
// of THEIR channels goes: the voiceover onto centre, trimmed down so it sits
// under the music rather than fighting it. This is the API that backs the
// CLI's src=/map= options and the GUI's multi-source controller.

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
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"

int main() {
    const std::array<ac3::plan::SourceShape, 2> sources{{
        {.channels = 2, .label = "music.wav"},
        {.channels = 1, .label = "voiceover.wav"},
    }};

    using ac3::eac3::chanmap::Location;
    ac3::plan::Assignment assignment;
    assignment.set(0, 0, {.kind = ac3::plan::DestinationKind::kLocation, .location = Location::kLeft});
    assignment.set(0, 1, {.kind = ac3::plan::DestinationKind::kLocation, .location = Location::kRight});
    // -6 dB under the music, so the voiceover reads without burying it.
    assignment.set(1, 0, {.kind = ac3::plan::DestinationKind::kLocation,
                          .location = Location::kCentre,
                          .trim_db = -6.0});

    std::printf("assignment: %s\n", ac3::plan::format_assignment(sources, assignment).c_str());

    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const auto routing = ac3::plan::route(target, sources, assignment);
    if (!routing) {
        std::printf("route failed - two rows named the same location, or the target can't "
                    "express one\n");
        return 1;
    }

    // Ls, Rs and LFE were never assigned a source channel, so they stay silent
    // - a warning banner would read this the same way a GUI does.
    const auto unassigned = assignment.unassigned(sources);
    std::printf("%zu source channel(s) left unassigned\n", unassigned.size());

    constexpr int kSourceChannels = 3;  // music L, music R, voiceover
    constexpr int kFrames = 31;         // one second
    constexpr std::array<double, kSourceChannels> kTones{440.0, 660.0, 220.0};

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 384, .acmod = target.bed_acmod, .lfe = target.bed_lfe});

    std::vector<std::vector<float>> source_pcm(kSourceChannels, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> coded_pcm(static_cast<std::size_t>(routing->coded_channels),
                                              std::vector<float>(ac3::kSamplesPerFrame));

    std::vector<std::byte> stream;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (std::size_t ch = 0; ch < source_pcm.size(); ++ch) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                source_pcm[ch][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
            }
        }

        std::vector<std::span<const float>> source_views;
        for (const auto& channel : source_pcm) {
            source_views.emplace_back(channel);
        }
        std::vector<std::span<float>> coded_views;
        for (auto& channel : coded_pcm) {
            coded_views.emplace_back(channel);
        }
        ac3::plan::render(*routing, source_views, coded_views, ac3::kSamplesPerFrame);

        std::vector<std::span<const float>> encode_views;
        for (const auto& channel : coded_pcm) {
            encode_views.emplace_back(channel);
        }
        const auto encoded = encoder->encode_frame(encode_views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }
    std::printf("%zu bytes of 5.1 AC-3, music + trimmed centre voiceover\n", stream.size());
    return 0;
}
