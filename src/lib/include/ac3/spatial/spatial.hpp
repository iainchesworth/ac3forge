#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

// The spatial/object layer: applications place and move mono sources around
// the listener; the renderer turns the scene into a 5.1 channel bed that
// feeds the AC-3 encoder (or any other sink — nothing here knows about
// AC-3 except the bed's channel order).
//
// Design per docs/RESEARCH.md §6:
// - 2D pairwise amplitude panning (VBAP on the horizontal ring — 5.1 has no
//   height): pick the adjacent speaker pair around the target azimuth, solve
//   the 2x2 system, clamp, and normalize to Σg² = 1 (energy preservation).
// - Speaker geometry per ITU-R BS.775: C 0°, L +30°, R −30°, SL +110°,
//   SR −110° (azimuth counterclockwise from front, degrees).
// - Objects never feed the LFE implicitly; an explicit lfe_send exists.
// - Automation is clocked at the 256-sample block: targets set between
//   blocks, applied with per-sample linear ramps (no zipper noise).
// - The render path performs no allocation.

namespace ac3::spatial {

inline constexpr int kBedChannels = 5;  // AC-3 3/2 order: L, C, R, SL, SR
inline constexpr int kBlockSamples = 256;

// Per-speaker gains for one source direction, AC-3 3/2 channel order.
using PanGains = std::array<double, kBedChannels>;

// Energy-normalized pairwise pan of a direction (degrees, CCW from front,
// any value; normalized internally) onto the 5.1 ring.
[[nodiscard]] PanGains pan_azimuth(double azimuth_deg);

struct ObjectState {
    double azimuth_deg = 0.0;
    double gain = 1.0;      // linear
    double lfe_send = 0.0;  // linear; the only way an object reaches the LFE
};

// Renders mono objects into a 5.1 bed (5 fullbw channels + LFE), one
// 256-sample block at a time, ramping each object's channel gains linearly
// from the previous block's values to the current targets.
class BedRenderer {
public:
    // Returns the object's index. Call before rendering starts (allocates).
    std::size_t add_object(const ObjectState& initial);

    void set_target(std::size_t object, const ObjectState& target);

    // audio: one 256-sample mono span per object, same order as add_object.
    // bed: 6 spans (L, C, R, SL, SR, LFE) of 256 samples each, OVERWRITTEN.
    void render_block(std::span<const std::span<const float>> audio,
                      std::span<const std::span<float>> bed);

private:
    struct Slot {
        ObjectState target;
        PanGains current_gains{};
        double current_lfe = 0.0;
        bool primed = false;  // first block jumps to target instead of ramping
    };
    std::vector<Slot> slots_;
};

}  // namespace ac3::spatial
