// Encode objects as Dolby Atmos in Dolby Digital Plus (ETSI TS 103 420), then
// decode the same stream straight back and report what came out.
//
// The output is one ordinary 5.1 E-AC-3 stream. Objects are panned into the
// bed, which a legacy decoder plays unchanged; the OAMD and JOC payloads ride
// beside it in an EMDF container saying where each object is and how to pull
// it back out. See docs/library/spatial-and-atmos.md for what a decoder will
// and will not do with them.
//
// This example is also the end-to-end proof that the decode side actually
// works: three objects circle continuously for two seconds (not a single
// static frame), and every frame is decoded back through ac3::Eac3Decoder as
// soon as it is encoded, so the reported positions and audio-tracking SNR
// below are measured against real, moving ground truth.

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

int main() {
    constexpr int kObjects = 3;
    // Object metadata competes with the mantissas for the same frame, so an
    // object stream wants more headroom than a plain 5.1 one.
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
    ac3::Eac3Decoder decoder;

    std::vector<std::vector<float>> sources(
        kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (const auto& source : sources) {
        views.emplace_back(source);
    }

    constexpr std::array<double, kObjects> tones{440.0, 880.0, 1320.0};
    std::vector<std::byte> stream;

    // Position error and audio-tracking SNR accumulate across every frame
    // after this one, so the transform pair's own warm-up (see
    // tests/oba/test_oba.cpp's "reconstruct is a 256-sample-delayed identity..."
    // and tests/oba/test_atmos.cpp's "joc::reconstruct recovers well-separated
    // objects...") doesn't flatter the numbers below.
    constexpr int kWarmupFrames = 3;
    constexpr int kTotalFrames = 62;  // two seconds
    constexpr std::size_t kDelay = 512;  // encode+decode (256) + reconstruct's own pass (256)

    double position_error_sum = 0.0;
    int position_samples = 0;
    std::array<std::vector<float>, kObjects> source_history;
    std::array<std::vector<float>, kObjects> recovered_history;

    for (int frame = 0; frame < kTotalFrames; ++frame) {
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[obj][static_cast<std::size_t>(n)] = static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * tones[obj] * t));
            }
        }

        // Positions are room-anchored per §4.2.1: x 0 at the left wall to 1 at
        // the right, y 0 front to 1 back, z -1 at the floor to +1 at the
        // ceiling (0 is listener height). Each object circles at its own rate
        // and height - real motion, not a single static placement.
        const double seconds = frame * ac3::kSamplesPerFrame / 48000.0;
        std::array<ac3::oba::ObjectPlacement, kObjects> placement{};
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            const double angle =
                2.0 * std::numbers::pi * seconds / (2.0 + static_cast<double>(obj));
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

        // --- decode this same frame straight back --------------------------
        if (unit->substream_count() != 1) {
            std::printf("unexpected substream count %d\n",
                       static_cast<int>(unit->substream_count()));
            return 1;
        }
        const auto decoded = decoder.decode_substream(unit->substream(0));
        if (!decoded) {
            std::printf("decode failed: %d\n", std::to_underlying(decoded.error()));
            return 1;
        }
        if (!decoded->has_value()) {
            continue;  // held back for transient pre-noise processing; AtmosEncoder never triggers this
        }
        const auto& sub = **decoded;

        if (frame >= kWarmupFrames) {
            if (!sub.object_metadata || sub.object_metadata->objects.size() != kObjects) {
                std::printf("frame %d: no object metadata decoded\n", frame);
                return 1;
            }
            for (std::size_t obj = 0; obj < kObjects; ++obj) {
                const auto& want = placement[obj].position;
                const auto& got = sub.object_metadata->objects[obj].position;
                const double dx = got.x - want.x;
                const double dy = got.y - want.y;
                const double dz = got.z - want.z;
                position_error_sum += std::sqrt(dx * dx + dy * dy + dz * dz);
                ++position_samples;
            }
            if (sub.object_audio.size() == kObjects) {
                for (std::size_t obj = 0; obj < kObjects; ++obj) {
                    source_history[obj].insert(source_history[obj].end(), sources[obj].begin(),
                                               sources[obj].end());
                    recovered_history[obj].insert(recovered_history[obj].end(),
                                                  sub.object_audio[obj].begin(),
                                                  sub.object_audio[obj].end());
                }
            }
        }

        if (frame % 20 == 0) {
            std::printf("frame %2d: object 0 encoded at (%.3f, %.3f, %.3f)", frame,
                       placement[0].position.x, placement[0].position.y, placement[0].position.z);
            if (sub.object_metadata && !sub.object_metadata->objects.empty()) {
                const auto& p = sub.object_metadata->objects[0].position;
                std::printf(", decoded at (%.3f, %.3f, %.3f)", p.x, p.y, p.z);
            }
            std::printf("\n");
        }
    }

    std::printf("%zu bytes of DD+ with %d objects over a 5.1 bed\n", stream.size(),
               encoder.dynamic_object_count());

    if (position_samples > 0) {
        std::printf("mean position error across %d frames of real motion: %.4f (room units)\n",
                   kTotalFrames - kWarmupFrames, position_error_sum / position_samples);
    }

    for (std::size_t obj = 0; obj < kObjects; ++obj) {
        const auto& src = source_history[obj];
        const auto& rec = recovered_history[obj];
        if (rec.size() <= kDelay) {
            continue;
        }
        double signal = 0.0;
        double error = 0.0;
        for (std::size_t n = kDelay; n < rec.size(); ++n) {
            const double want = static_cast<double>(src[n - kDelay]);
            const double got = static_cast<double>(rec[n]);
            signal += want * want;
            error += (got - want) * (got - want);
        }
        const double snr_db = 10.0 * std::log10(signal / std::max(error, 1e-30));
        std::printf("object %zu audio-tracking SNR: %.1f dB\n", obj, snr_db);
    }

    return 0;
}
