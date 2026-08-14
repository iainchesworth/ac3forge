// Decode a stream and report what it measures: peak/RMS per channel, and
// where the soundfield's energy sits on the speaker ring.
//
// ac3::analysis is what ac3cli and ac3gui share so their meters never
// disagree about a signal - one LevelMeter instance serves both the moving
// display (levels(), ballistic) and the exact end-of-run report (summary()),
// fed by the same pass over the samples.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"

int main() {
    constexpr ac3::Acmod kAcmod = ac3::Acmod::k3_2;
    constexpr bool kLfe = true;
    constexpr int kFrames = 62;  // two seconds
    constexpr std::array<double, 6> kTones{1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    // A different level per channel, so the report has something to show.
    constexpr std::array<double, 6> kAmplitudes{0.8, 0.3, 0.8, 0.5, 0.5, 0.9};

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 448, .acmod = kAcmod, .lfe = kLfe});
    ac3::FrameDecoder decoder;
    ac3::analysis::LevelMeter meter{kAcmod, kLfe, 48000};

    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    for (int frame = 0; frame < kFrames; ++frame) {
        for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                pcm[ch][static_cast<std::size_t>(n)] = static_cast<float>(
                    kAmplitudes[ch] * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
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
        const auto decoded = decoder.decode_frame(*encoded);
        if (!decoded) {
            std::printf("decode failed: %.*s\n",
                        static_cast<int>(ac3::describe(decoded.error()).size()),
                        ac3::describe(decoded.error()).data());
            return 1;
        }

        std::vector<std::span<const float>> decoded_views;
        for (const auto& channel : decoded->channels) {
            decoded_views.emplace_back(channel);
        }
        meter.process(decoded_views);
    }

    for (int ch = 0; ch < meter.channel_count(); ++ch) {
        const auto name = ac3::analysis::channel_name(kAcmod, kLfe, ch);
        const auto& stats = meter.summary()[static_cast<std::size_t>(ch)];
        std::printf("%-3.*s peak %6.1f dBFS  rms %6.1f dBFS%s\n", static_cast<int>(name.size()),
                    name.data(), stats.peak_db(), stats.rms_db(), stats.clipped_samples > 0 ? "  CLIPPED" : "");
    }

    const auto energy = ac3::analysis::energy_vector(meter.levels(), kAcmod);
    std::printf("soundfield: %.1f degrees, magnitude %.2f, %.1f dBFS\n", energy.azimuth_deg,
                energy.magnitude, energy.level_db);
    return 0;
}
