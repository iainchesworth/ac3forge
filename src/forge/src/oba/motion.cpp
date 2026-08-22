#include "ac3/oba/motion.hpp"

#include <algorithm>
#include <cmath>
#include <expected>
#include <numbers>
#include "ac3/oba/atmos.hpp"
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace ac3::oba {

namespace {

double lerp(double a, double b, double f) { return a + (b - a) * f; }

ObjectPlacement placement_of(const Keyframe& k) {
    return {.position = k.position, .gain = k.gain, .lfe_send = k.lfe_send};
}

ObjectPlacement interpolate(const Keyframe& a, const Keyframe& b, double time_s) {
    const double f = (time_s - a.time_s) / (b.time_s - a.time_s);
    return {.position = {.x = lerp(a.position.x, b.position.x, f),
                         .y = lerp(a.position.y, b.position.y, f),
                         .z = lerp(a.position.z, b.position.z, f)},
            .gain = lerp(a.gain, b.gain, f),
            .lfe_send = lerp(a.lfe_send, b.lfe_send, f)};
}

}  // namespace

std::expected<KeyframePath, PathError> KeyframePath::create(std::vector<Keyframe> keyframes) {
    if (keyframes.empty()) {
        return std::unexpected(PathError::kNoKeyframes);
    }
    std::ranges::sort(keyframes, {}, &Keyframe::time_s);
    const auto duplicate = std::ranges::adjacent_find(
        keyframes, [](const Keyframe& a, const Keyframe& b) { return a.time_s == b.time_s; });
    if (duplicate != keyframes.end()) {
        return std::unexpected(PathError::kDuplicateTimestamp);
    }
    return KeyframePath{std::move(keyframes)};
}

ObjectPlacement KeyframePath::evaluate(double time_s) const {
    if (time_s <= keyframes_.front().time_s) {
        return placement_of(keyframes_.front());
    }
    if (time_s >= keyframes_.back().time_s) {
        return placement_of(keyframes_.back());
    }
    const auto next = std::ranges::upper_bound(keyframes_, time_s, {}, &Keyframe::time_s);
    return interpolate(*(next - 1), *next, time_s);
}

OrbitPath::OrbitPath(double rate_hz, double phase_rad, double height, double gain,
                     double lfe_send)
    : rate_hz_(rate_hz), phase_rad_(phase_rad), height_(height), gain_(gain),
      lfe_send_(lfe_send) {}

ObjectPlacement OrbitPath::evaluate(double time_s) const {
    const double angle = 2.0 * std::numbers::pi * rate_hz_ * time_s + phase_rad_;
    return {.position = {.x = 0.5 + 0.5 * std::sin(angle),
                         .y = 0.5 - 0.5 * std::cos(angle),
                         .z = height_},
            .gain = gain_,
            .lfe_send = lfe_send_};
}

ObjectPlacement ObjectPath::evaluate(double time_s) const {
    return std::visit([time_s](const auto& path) { return path.evaluate(time_s); }, path_);
}

ObjectPath make_orbit_path(double rate_hz, double phase_rad, double height, double gain,
                           double lfe_send) {
    return ObjectPath{OrbitPath{rate_hz, phase_rad, height, gain, lfe_send}};
}

std::vector<ObjectPlacement> evaluate_placements(std::span<const ObjectPath> paths,
                                                  double time_s) {
    std::vector<ObjectPlacement> placement;
    placement.reserve(paths.size());
    for (const auto& path : paths) {
        placement.push_back(path.evaluate(time_s));
    }
    return placement;
}

}  // namespace ac3::oba
