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

// The ITU-R BS.775 ring, in AC-3 3/2 channel order, degrees counterclockwise
// from front. Everything spatial — the panner here, and the soundfield
// analysis the front ends draw — is defined against this one array so the
// geometry cannot drift between them.
inline constexpr std::array<double, kBedChannels> kSpeakerAzimuthDeg = {
    30.0,    // L
    0.0,     // C
    -30.0,   // R
    110.0,   // SL
    -110.0,  // SR
};

// Per-speaker gains for one source direction, AC-3 3/2 channel order.
using PanGains = std::array<double, kBedChannels>;

// Energy-normalized pairwise pan of a direction (degrees, CCW from front,
// any value; normalized internally) onto the 5.1 ring.
[[nodiscard]] PanGains pan_azimuth(double azimuth_deg);

// The same pan onto an ARBITRARY horizontal ring, which is what any layout
// wider than 5.1 needs: 7.1 puts its side surrounds at 90° and its rears at
// 150°, so a source at 110° belongs to a different pair there than it does on
// the 5.1 ring. `ring_azimuth_deg` may be in any order and any range; `gains`
// takes one entry per ring member and is OVERWRITTEN, with Sum(g^2) == 1 for a
// non-empty ring.
//
// Two speakers more than 180° apart leave an arc no pair can enclose - the
// hole behind a front-only pair being the obvious case, where the VBAP system
// is singular and both gains solve negative. Across such an arc this
// crossfades at constant power instead, which agrees with the pairwise
// solution at both edges and never drops the source into silence.
void pan_ring(double azimuth_deg, std::span<const double> ring_azimuth_deg,
              std::span<double> gains);

// The same pan, addressed by a room-anchored position instead of an angle:
// x runs 0 at the left wall to 1 at the right and y 0 at the front wall to 1
// at the back, which is TS 103 420 §4.2.1's system. A source at the exact
// centre of the room has no direction at all and stays at the front.
//
// There is no z. A 5.1 ring has no height speakers, so elevation cannot be
// rendered and a raised source folds onto the ring at its azimuth, at full
// level - a legacy 5.1 decoder has to hear everything, or backward
// compatibility means nothing. The height survives in the object metadata
// instead, which is the entire reason the object layer exists.
//
// The consequence is worth stating plainly: two sources at the same azimuth
// and different heights get IDENTICAL bed gains, and nothing downstream can
// tell them apart from the bed alone.
[[nodiscard]] PanGains pan_room(double x, double y);

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
