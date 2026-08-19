#include "ac3/mlp/predictor.hpp"

#include <cassert>

#include "ac3/mlp/matrix.hpp"

namespace ac3::mlp {

namespace {

// The shared summing node: sum_j a[j]*x_past[j] - sum_j b[j]*y_past[j],
// as an exact integer in units of 1/2^shift. Encoder and decoder call this
// with identical histories, which is the whole losslessness argument.
[[nodiscard]] std::int64_t combination(const PredictorCoefficients& c, const PredictorState& s) {
    std::int64_t acc = 0;
    for (std::size_t j = 0; j < c.a.size(); ++j) {
        acc += static_cast<std::int64_t>(c.a[j]) * s.input[j];
    }
    for (std::size_t j = 0; j < c.b.size(); ++j) {
        acc -= static_cast<std::int64_t>(c.b[j]) * s.output[j];
    }
    return acc;
}

void push_history(std::array<std::int64_t, kMaxPredictorOrder>& history, std::int64_t value) {
    for (std::size_t j = history.size() - 1; j > 0; --j) {
        history[j] = history[j - 1];
    }
    history[0] = value;
}

void check(const PredictorCoefficients& c) {
    assert(c.shift >= 0 && c.shift <= 14);
    assert(c.a.size() <= kMaxPredictorOrder);
    assert(c.b.size() <= kMaxPredictorOrder);
}

}  // namespace

void predict_encode(const PredictorCoefficients& coefficients, std::span<const std::int32_t> input,
                    std::span<std::int32_t> output, PredictorState& state) {
    check(coefficients);
    assert(input.size() == output.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const std::int64_t q = matrix::quantize(combination(coefficients, state),
                                                coefficients.shift);
        const std::int64_t x = input[i];
        const std::int64_t y = x + q;
        // A wildly unstable feedback filter grows the residual without
        // bound; the encoder is responsible for never selecting one (the
        // WO: "Some of the sets of coefficient values allowed result in an
        // unstable encoding filter, and use of these should be avoided").
        assert(y >= INT32_MIN && y <= INT32_MAX);
        output[i] = static_cast<std::int32_t>(y);
        push_history(state.input, x);
        push_history(state.output, y);
    }
}

void predict_decode(const PredictorCoefficients& coefficients,
                    std::span<const std::int32_t> residual, std::span<std::int32_t> output,
                    PredictorState& state) {
    check(coefficients);
    assert(residual.size() == output.size());
    for (std::size_t i = 0; i < residual.size(); ++i) {
        const std::int64_t q = matrix::quantize(combination(coefficients, state),
                                                coefficients.shift);
        const std::int64_t y = residual[i];
        const std::int64_t x = y - q;
        assert(x >= INT32_MIN && x <= INT32_MAX);
        output[i] = static_cast<std::int32_t>(x);
        push_history(state.input, x);
        push_history(state.output, y);
    }
}

PredictorCoefficients table1_preset(int case_index) {
    assert(case_index >= 0 && case_index < kTable1Cases);
    // WO 96/37048 Table 1 (verified against the scanned page): eight
    // second-order cases in quarters, columns a1 a2 b1 b2, for 44.1 kHz.
    static constexpr std::array<std::array<std::int32_t, 4>, 8> kCases{{
        {0, 0, 0, 0},     // case 0: passthrough
        {-6, 3, 3, 2},    // case 1
        {-6, 2, 3, 2},    // case 2
        {-6, 2, -1, 1},   // case 3
        {-7, 3, 0, -2},   // case 4
        {-1, 1, 5, 2},    // case 5
        {-3, 3, 5, 2},    // case 6
        {5, 2, -1, 1},    // case 7
    }};
    const auto& row = kCases[static_cast<std::size_t>(case_index)];
    PredictorCoefficients out;
    out.shift = 2;  // quarters
    out.a = {row[0], row[1]};
    out.b = {row[2], row[3]};
    return out;
}

}  // namespace ac3::mlp
