#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <variant>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"

// Per-object placement as a function of time. AtmosEncoder::encode_frame
// already takes a fresh ObjectPlacement every call and ramps the bed
// internally between them; what was missing was a shared way to say WHERE an
// object is at a given moment, so callers stop reimplementing that
// per-frame math independently. Scoped to authored/batch motion - the
// evaluate(time_s) shape is deliberately time-based so a future live-driven
// cursor could reuse it, but that plumbing is not built here.

namespace ac3::oba {

// One authored point in a per-object motion path: a placement anchored to a
// moment in time.
struct Keyframe {
    double time_s = 0.0;
    Position position{};
    double gain = 1.0;
    double lfe_send = 0.0;
};

enum class PathError : std::uint8_t { kNoKeyframes, kDuplicateTimestamp };

// Sparse authored points, linearly interpolated between neighbours. Clamps
// to the first/last keyframe outside their time range - an object holds
// still before its first cue and after its last, rather than extrapolating
// or going silent.
class AC3FORGE_EXPORT KeyframePath {
   public:
    [[nodiscard]] static std::expected<KeyframePath, PathError> create(
        std::vector<Keyframe> keyframes);

    [[nodiscard]] ObjectPlacement evaluate(double time_s) const;

   private:
    explicit KeyframePath(std::vector<Keyframe> sorted) : keyframes_(std::move(sorted)) {}

    std::vector<Keyframe> keyframes_;
};

// A circular orbit in the room's x/y plane, radius 0.5 about its centre,
// starting at phase_rad and completing one revolution every 1/rate_hz
// seconds, held at a constant height/gain/lfe_send. Evaluated in closed
// form so it stays an exact circle - a KeyframePath decimated from the same
// formula would only ever approximate it with straight chords.
class AC3FORGE_EXPORT OrbitPath {
   public:
    OrbitPath(double rate_hz, double phase_rad, double height, double gain, double lfe_send);

    [[nodiscard]] ObjectPlacement evaluate(double time_s) const;

   private:
    double rate_hz_;
    double phase_rad_;
    double height_;
    double gain_;
    double lfe_send_;
};

// A per-object placement over time - authored keyframes or a closed-form
// generator - behind one evaluate(t) interface, so a caller (CLI, GUI, and
// eventually a live-driven cursor) doesn't need to know which kind it holds.
class AC3FORGE_EXPORT ObjectPath {
   public:
    ObjectPath(KeyframePath path) : path_(std::move(path)) {}
    ObjectPath(const OrbitPath& path) : path_(path) {}

    [[nodiscard]] ObjectPlacement evaluate(double time_s) const;

   private:
    std::variant<KeyframePath, OrbitPath> path_;
};

// Sugar for the common case: an ObjectPath that is nothing but an orbit.
[[nodiscard]] AC3FORGE_EXPORT ObjectPath make_orbit_path(double rate_hz, double phase_rad,
                                                         double height, double gain,
                                                         double lfe_send);

// N objects, each with a path: their placements at one instant, in path
// order. What both the CLI's and the GUI's per-frame encode loops call once
// per frame to get the span encode_frame() wants.
[[nodiscard]] AC3FORGE_EXPORT std::vector<ObjectPlacement> evaluate_placements(
    std::span<const ObjectPath> paths, double time_s);

}  // namespace ac3::oba
