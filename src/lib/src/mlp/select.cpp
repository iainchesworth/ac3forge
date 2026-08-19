#include "ac3/mlp/select.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

#include "ac3/mlp/predictor.hpp"

namespace ac3::mlp::select {

namespace {

// Exact cost of encoding `significant` with `coefficients`: the payload
// bits the entropy stage will emit, plus this candidate's own header
// overhead (coefficient fields and initialisation data), so a higher-order
// filter has to EARN its extra header bits. Header sizing mirrors the
// provisional block layout in block.cpp.
[[nodiscard]] long long candidate_cost(std::span<const std::int32_t> significant,
                                       const PredictorCoefficients& coefficients) {
    const auto order = std::max(coefficients.a.size(), coefficients.b.size());
    if (order > significant.size()) {
        return std::numeric_limits<long long>::max();
    }
    std::vector<std::int32_t> residual(significant.size());
    PredictorState state{};
    predict_encode(coefficients, significant, residual, state);

    const auto choice = choose_coding_cost(residual);
    const long long header_bits =
        10 * static_cast<long long>(coefficients.a.size() + coefficients.b.size()) +
        24 * static_cast<long long>(order);  // init at up to ~24 bits apiece
    return choice.payload_bits + header_bits;
}

// The candidate palette: passthrough, the integer difference family
// (1 - z^-1)^n for n = 1..3 (JAES 1996's opening predictors, expanded into
// our a-coefficient convention), and the WO Table 1 presets.
[[nodiscard]] const std::vector<PredictorCoefficients>& palette() {
    static const std::vector<PredictorCoefficients> kPalette = [] {
        std::vector<PredictorCoefficients> out;
        out.push_back({});                          // passthrough
        out.push_back({.shift = 0, .a = {-1}});     // first difference
        out.push_back({.shift = 0, .a = {-2, 1}});  // second difference
        out.push_back({.shift = 0, .a = {-3, 3, -1}});  // third difference
        for (int preset = 1; preset < kTable1Cases; ++preset) {
            out.push_back(table1_preset(preset));
        }
        return out;
    }();
    return kPalette;
}

// Sum over channels of each channel's best-predictor cost - the measure
// the matrix search optimizes.
[[nodiscard]] long long total_cost(std::span<const std::vector<std::int32_t>> channels) {
    long long total = 0;
    for (const auto& channel : channels) {
        long long best = std::numeric_limits<long long>::max();
        for (const auto& candidate : palette()) {
            best = std::min(best, candidate_cost(channel, candidate));
        }
        total += best;
    }
    return total;
}

}  // namespace

PredictorCoefficients choose_predictor(std::span<const std::int32_t> significant) {
    PredictorCoefficients best{};
    long long best_cost = std::numeric_limits<long long>::max();
    for (const auto& candidate : palette()) {
        const auto cost = candidate_cost(significant, candidate);
        if (cost < best_cost) {
            best_cost = cost;
            best = candidate;
        }
    }
    return best;
}

std::vector<matrix::Step> choose_matrix(std::span<const std::span<const std::int32_t>> significant,
                                        int max_steps) {
    const auto channel_count = significant.size();
    std::vector<matrix::Step> steps;
    if (channel_count < 2) {
        return steps;
    }

    // Working copies the accepted steps are applied to.
    std::vector<std::vector<std::int32_t>> working(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        working[c].assign(significant[c].begin(), significant[c].end());
    }
    const auto length = working[0].size();

    long long current = total_cost(working);

    for (int round = 0; round < max_steps; ++round) {
        matrix::Step best_step;
        std::vector<std::int32_t> best_target_signal;
        long long best_cost = current;

        for (std::size_t source = 0; source < channel_count; ++source) {
            for (std::size_t target = 0; target < channel_count; ++target) {
                if (source == target) {
                    continue;
                }
                // Least-squares fit of target against source, quantized to
                // m/64 and bounded by the header's coefficient field.
                std::int64_t dot = 0;
                std::int64_t energy = 0;
                for (std::size_t i = 0; i < length; ++i) {
                    dot += static_cast<std::int64_t>(working[target][i]) * working[source][i];
                    energy += static_cast<std::int64_t>(working[source][i]) * working[source][i];
                }
                if (energy == 0) {
                    continue;
                }
                const auto numerator = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    (dot * 64 + (dot >= 0 ? energy / 2 : -energy / 2)) / energy, -511, 511));
                if (numerator == 0) {
                    continue;
                }

                matrix::Step step{static_cast<int>(target), 6,
                                  {{static_cast<int>(source), -numerator}}};

                // Apply to a scratch copy of the target channel and price it.
                std::vector<std::int32_t> modified(length);
                bool fits = true;
                for (std::size_t i = 0; i < length; ++i) {
                    const auto value =
                        working[target][i] +
                        matrix::quantize(
                            static_cast<std::int64_t>(-numerator) * working[source][i], 6);
                    if (value < INT32_MIN || value > INT32_MAX) {
                        fits = false;
                        break;
                    }
                    modified[i] = static_cast<std::int32_t>(value);
                }
                if (!fits) {
                    continue;
                }

                auto scratch = working;
                scratch[target] = modified;
                const auto cost = total_cost(scratch);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_step = step;
                    best_target_signal = std::move(modified);
                }
            }
        }

        if (best_step.terms.empty()) {
            break;  // no step improves the measured cost
        }
        working[static_cast<std::size_t>(best_step.target)] = std::move(best_target_signal);
        steps.push_back(std::move(best_step));
        current = best_cost;
    }
    return steps;
}

MultichannelBlockConfig choose_block_config(std::span<const std::span<const std::int32_t>> channels,
                                            int wordlength) {
    const auto channel_count = channels.size();
    assert(channel_count >= 1 && channel_count <= static_cast<std::size_t>(kMaxBlockChannels));

    // Reproduce the block codec's strip so selection sees the same
    // significant-word domain it will encode.
    std::vector<std::vector<std::int32_t>> significant(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        const auto b1 = detect_constant_lsbs(channels[c], wordlength);
        const auto lsb = static_cast<std::uint32_t>(channels[c][0]) & ((1u << b1) - 1);
        significant[c].resize(channels[c].size());
        for (std::size_t i = 0; i < channels[c].size(); ++i) {
            significant[c][i] = (channels[c][i] - static_cast<std::int32_t>(lsb)) >> b1;
        }
    }

    MultichannelBlockConfig config;
    {
        std::vector<std::span<const std::int32_t>> spans;
        spans.reserve(channel_count);
        for (const auto& channel : significant) {
            spans.emplace_back(channel);
        }
        config.steps = choose_matrix(std::span<const std::span<const std::int32_t>>{spans});
    }

    // Apply the chosen steps, then pick a predictor per post-matrix channel.
    if (!config.steps.empty()) {
        std::vector<std::int64_t> instant(channel_count);
        for (std::size_t i = 0; i < significant[0].size(); ++i) {
            for (std::size_t c = 0; c < channel_count; ++c) {
                instant[c] = significant[c][i];
            }
            matrix::encode_cascade(config.steps, instant);
            for (std::size_t c = 0; c < channel_count; ++c) {
                significant[c][i] = static_cast<std::int32_t>(instant[c]);
            }
        }
    }
    config.coefficients.reserve(channel_count);
    for (const auto& channel : significant) {
        config.coefficients.push_back(choose_predictor(channel));
    }
    return config;
}

}  // namespace ac3::mlp::select
