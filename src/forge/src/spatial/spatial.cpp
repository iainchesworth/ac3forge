#include "ac3/spatial/spatial.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

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

namespace {

// Azimuth folded into [0, 360).
[[nodiscard]] double wrap360(double degrees) {
    double value = std::fmod(degrees, 360.0);
    if (value < 0.0) {
        value += 360.0;
    }
    return value;
}

// The pair of ring positions enclosing `azimuth`, and the two gains that place
// it between them. Shared by pan_azimuth and pan_ring so the 5.1 ring cannot
// end up panned differently from any other.
struct Pair {
    std::size_t a = 0;
    std::size_t b = 0;
    double ga = 0.0;
    double gb = 0.0;
};

// `sorted` holds ring azimuths in ascending [0, 360) order.
[[nodiscard]] Pair pan_sorted_ring(double azimuth, std::span<const double> sorted) {
    Pair out;
    if (sorted.empty()) {
        return out;
    }
    if (sorted.size() == 1) {
        return {.a = 0, .b = 0, .ga = 1.0, .gb = 0.0};
    }
    // Wrapping past the last speaker lands back on the first.
    out.a = sorted.size() - 1;
    for (std::size_t i = 0; i + 1 < sorted.size(); ++i) {
        if (azimuth >= sorted[i] && azimuth < sorted[i + 1]) {
            out.a = i;
            break;
        }
    }
    out.b = (out.a + 1) % sorted.size();

    const double a_deg = sorted[out.a];
    // Unwrapped, so the arc from a to b is positive whether or not it crosses
    // the 360° seam.
    const double b_deg = out.b > out.a ? sorted[out.b] : sorted[out.b] + 360.0;
    const double u_deg = azimuth >= a_deg ? azimuth : azimuth + 360.0;
    const double arc = b_deg - a_deg;

    if (arc >= 180.0) {
        // No pair encloses this direction: the VBAP system is singular at
        // exactly 180° and solves negative beyond it. Crossfade at constant
        // power across the gap instead - unity at each edge, so it joins the
        // pairwise solution continuously.
        const double t = arc > 0.0 ? (u_deg - a_deg) / arc : 0.0;
        out.ga = std::cos(t * std::numbers::pi / 2.0);
        out.gb = std::sin(t * std::numbers::pi / 2.0);
        return out;
    }

    // 2D VBAP: solve [pa pb] g = u for the unit vectors, clamp, normalize.
    const double ax = std::cos(a_deg * kDegToRad);
    const double ay = std::sin(a_deg * kDegToRad);
    const double bx = std::cos(b_deg * kDegToRad);
    const double by = std::sin(b_deg * kDegToRad);
    const double ux = std::cos(u_deg * kDegToRad);
    const double uy = std::sin(u_deg * kDegToRad);
    const double det = ax * by - ay * bx;
    assert(std::abs(det) > 1e-9);
    double ga = std::max((ux * by - uy * bx) / det, 0.0);
    double gb = std::max((ax * uy - ay * ux) / det, 0.0);
    const double norm = std::sqrt(ga * ga + gb * gb);
    if (norm > 0.0) {
        out.ga = ga / norm;  // Σg² == 1
        out.gb = gb / norm;
    }
    return out;
}

}  // namespace

PanGains pan_azimuth(double azimuth_deg) {
    constexpr std::array<double, kRing.size()> sorted = {
        kRing[0].azimuth_deg, kRing[1].azimuth_deg, kRing[2].azimuth_deg,
        kRing[3].azimuth_deg, kRing[4].azimuth_deg};
    const auto pair = pan_sorted_ring(wrap360(azimuth_deg), sorted);

    PanGains gains{};
    gains[static_cast<std::size_t>(kRing[pair.a].bed_index)] = pair.ga;
    gains[static_cast<std::size_t>(kRing[pair.b].bed_index)] = pair.gb;
    return gains;
}

void pan_ring(double azimuth_deg, std::span<const double> ring_azimuth_deg,
              std::span<double> gains) {
    assert(gains.size() == ring_azimuth_deg.size());
    std::ranges::fill(gains, 0.0);
    if (ring_azimuth_deg.empty()) {
        return;
    }
    // Sort a copy by wrapped azimuth, keeping each speaker's caller-side index
    // so the gains land back where the caller expects them.
    std::vector<std::size_t> order(ring_azimuth_deg.size());
    std::vector<double> sorted(ring_azimuth_deg.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&](std::size_t l, std::size_t r) {
        return wrap360(ring_azimuth_deg[l]) < wrap360(ring_azimuth_deg[r]);
    });
    for (std::size_t i = 0; i < order.size(); ++i) {
        sorted[i] = wrap360(ring_azimuth_deg[order[i]]);
    }

    const auto pair = pan_sorted_ring(wrap360(azimuth_deg), sorted);
    gains[order[pair.a]] += pair.ga;
    // A one-speaker ring reports the same index twice; adding rather than
    // assigning would then give it 1 + 0 either way, but a two-speaker ring
    // whose pair wraps onto itself must not have its first gain overwritten.
    if (pair.b != pair.a) {
        gains[order[pair.b]] += pair.gb;
    }
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
