#include "ac3/mlp/matrix.hpp"

#include <cassert>

namespace ac3::mlp::matrix {

namespace {

[[nodiscard]] std::int64_t combination(const Step& step, std::span<const std::int64_t> samples) {
    assert(static_cast<std::size_t>(step.target) < samples.size());
    std::int64_t combo = 0;
    for (const auto& [source, numerator] : step.terms) {
        assert(source != step.target);
        assert(static_cast<std::size_t>(source) < samples.size());
        combo += static_cast<std::int64_t>(numerator) * samples[static_cast<std::size_t>(source)];
    }
    return combo;
}

}  // namespace

void encode_cascade(std::span<const Step> steps, std::span<std::int64_t> samples) {
    for (const auto& step : steps) {
        samples[static_cast<std::size_t>(step.target)] += quantize(combination(step, samples), step.shift);
    }
}

void decode_cascade(std::span<const Step> steps, std::span<std::int64_t> samples) {
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        const auto& step = *it;
        samples[static_cast<std::size_t>(step.target)] -= quantize(combination(step, samples), step.shift);
    }
}

}  // namespace ac3::mlp::matrix
