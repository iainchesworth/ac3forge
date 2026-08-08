#include "ac3/spatial/spatial.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace ac3::spatial {

namespace {

// The 5.1 ring in counterclockwise order starting at front-center, with the
// bed-channel index each ring position maps to (AC-3 3/2 order L,C,R,SL,SR).
struct RingSpeaker {
    double azimuth_deg;
    int bed_index;
};
// Ascending azimuth is what the pair search below needs, so the two rear
// speakers appear wrapped into [0, 360) rather than negative.
constexpr std::array<RingSpeaker, 5> kRing = {{
    {kSpeakerAzimuthDeg[1], 1},          // C     0°
    {kSpeakerAzimuthDeg[0], 0},          // L   +30°
    {kSpeakerAzimuthDeg[3], 3},          // SL +110°
    {kSpeakerAzimuthDeg[4] + 360.0, 4},  // SR -110°
    {kSpeakerAzimuthDeg[2] + 360.0, 2},  // R   -30°
}};

constexpr double kDegToRad = std::numbers::pi / 180.0;

}  // namespace

PanGains pan_azimuth(double azimuth_deg) {
    double azimuth = std::fmod(azimuth_deg, 360.0);
    if (azimuth < 0.0) {
        azimuth += 360.0;
    }

    // Find the adjacent ring pair enclosing the azimuth (wrapping 330° -> 0°).
    std::size_t first = kRing.size() - 1;
    for (std::size_t i = 0; i + 1 < kRing.size(); ++i) {
        if (azimuth >= kRing[i].azimuth_deg && azimuth < kRing[i + 1].azimuth_deg) {
            first = i;
            break;
        }
    }
    const RingSpeaker& a = kRing[first];
    const RingSpeaker& b = kRing[(first + 1) % kRing.size()];

    // 2D VBAP: solve [pa pb] g = u for the unit vectors, clamp, normalize.
    const double ax = std::cos(a.azimuth_deg * kDegToRad);
    const double ay = std::sin(a.azimuth_deg * kDegToRad);
    const double bx = std::cos(b.azimuth_deg * kDegToRad);
    const double by = std::sin(b.azimuth_deg * kDegToRad);
    const double ux = std::cos(azimuth * kDegToRad);
    const double uy = std::sin(azimuth * kDegToRad);
    const double det = ax * by - ay * bx;
    assert(std::abs(det) > 1e-9);
    double ga = (ux * by - uy * bx) / det;
    double gb = (ax * uy - ay * ux) / det;
    ga = std::max(ga, 0.0);
    gb = std::max(gb, 0.0);
    const double norm = std::sqrt(ga * ga + gb * gb);

    PanGains gains{};
    if (norm > 0.0) {
        gains[static_cast<std::size_t>(a.bed_index)] = ga / norm;  // Σg² == 1
        gains[static_cast<std::size_t>(b.bed_index)] = gb / norm;
    }
    return gains;
}

PanGains pan_room(double x, double y) {
    // Forward is -y and left is -x, so the ring's azimuth (CCW from front,
    // left positive) is atan2(left, forward).
    const double left = 0.5 - x;
    const double forward = 0.5 - y;
    if (left == 0.0 && forward == 0.0) {
        return pan_azimuth(0.0);
    }
    return pan_azimuth(std::atan2(left, forward) / kDegToRad);
}

std::size_t BedRenderer::add_object(const ObjectState& initial) {
    Slot slot;
    slot.target = initial;
    slots_.push_back(slot);
    return slots_.size() - 1;
}

void BedRenderer::set_target(std::size_t object, const ObjectState& target) {
    assert(object < slots_.size());
    slots_[object].target = target;
}

void BedRenderer::render_block(std::span<const std::span<const float>> audio,
                               std::span<const std::span<float>> bed) {
    assert(audio.size() == slots_.size());
    assert(bed.size() == kBedChannels + 1);  // + LFE
    for (const auto& channel : bed) {
        assert(channel.size() == kBlockSamples);
        std::ranges::fill(channel, 0.0f);
    }

    for (std::size_t object = 0; object < slots_.size(); ++object) {
        auto& slot = slots_[object];
        const auto& source = audio[object];
        assert(source.size() == kBlockSamples);

        PanGains target_gains = pan_azimuth(slot.target.azimuth_deg);
        for (auto& g : target_gains) {
            g *= slot.target.gain;
        }
        const double target_lfe = slot.target.lfe_send * slot.target.gain;
        if (!slot.primed) {
            slot.current_gains = target_gains;
            slot.current_lfe = target_lfe;
            slot.primed = true;
        }

        for (int ch = 0; ch < kBedChannels; ++ch) {
            const double from = slot.current_gains[static_cast<std::size_t>(ch)];
            const double to = target_gains[static_cast<std::size_t>(ch)];
            if (from == 0.0 && to == 0.0) {
                continue;
            }
            auto& out = bed[static_cast<std::size_t>(ch)];
            for (int n = 0; n < kBlockSamples; ++n) {
                const double g = from + (to - from) * (n + 1) / kBlockSamples;
                out[static_cast<std::size_t>(n)] += static_cast<float>(
                    g * static_cast<double>(source[static_cast<std::size_t>(n)]));
            }
        }
        if (slot.current_lfe != 0.0 || target_lfe != 0.0) {
            auto& lfe = bed[kBedChannels];
            for (int n = 0; n < kBlockSamples; ++n) {
                const double g =
                    slot.current_lfe + (target_lfe - slot.current_lfe) * (n + 1) / kBlockSamples;
                lfe[static_cast<std::size_t>(n)] += static_cast<float>(
                    g * static_cast<double>(source[static_cast<std::size_t>(n)]));
            }
        }
        slot.current_gains = target_gains;
        slot.current_lfe = target_lfe;
    }
}

}  // namespace ac3::spatial
