#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/mdct.hpp"
#include "ac3/core/window.hpp"
#include "golden/mdct_goldens.hpp"

namespace {

double max_abs_diff(std::span<const double> a, std::span<const double> b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    return worst;
}

std::array<double, 256> forward_of(const std::array<double, 512>& input) {
    std::array<double, 512> windowed{};
    std::array<double, 256> coeffs{};
    ac3::apply_analysis_window(input, windowed);
    ac3::mdct512_forward(windowed, coeffs);
    return coeffs;
}

struct ShortCoeffs {
    std::array<double, 128> first;
    std::array<double, 128> second;
};

ShortCoeffs short_forward_of(const std::array<double, 512>& input) {
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(input, windowed);
    const std::span<const double, 512> full(windowed);
    ShortCoeffs out{};
    ac3::mdct256_forward_first(full.first<256>(), out.first);
    ac3::mdct256_forward_second(full.last<256>(), out.second);
    return out;
}

}  // namespace

TEST_CASE("analysis window reproduces spec Table 7.33", "[window]") {
    // The published table is rounded to 5 decimals; our full-precision value
    // must round to exactly the printed value.
    for (std::size_t n = 0; n < 256; ++n) {
        CAPTURE(n);
        CHECK(std::abs(ac3::kAnalysisWindow[n] - ac3::golden::kTable733[n]) < 5.01e-6);
    }
}

TEST_CASE("analysis window matches the independent numpy evaluation", "[window]") {
    CHECK(max_abs_diff(ac3::kAnalysisWindow, ac3::golden::kKbdWindow512) < 1e-12);
}

TEST_CASE("analysis window symmetry and Princen-Bradley condition", "[window]") {
    const auto& w = ac3::kAnalysisWindow;
    for (std::size_t n = 0; n < 256; ++n) {
        CAPTURE(n);
        // 8.2.3.1: 256 coefficients used back-to-back, symmetric.
        CHECK(w[n] == w[511 - n]);
        // KBD construction: w[n]^2 + w[n+256]^2 == 1 exactly (this is what
        // makes 50%-overlap TDAC reconstruction possible).
        CHECK(std::abs(w[n] * w[n] + w[n + 256] * w[n + 256] - 1.0) < 1e-12);
    }
}

TEST_CASE("forward MDCT matches numpy goldens", "[mdct]") {
    CHECK(max_abs_diff(forward_of(ac3::golden::kGoldenImpulse0Input),
                       ac3::golden::kGoldenImpulse0Coeffs) < 1e-10);
    CHECK(max_abs_diff(forward_of(ac3::golden::kGoldenImpulse100Input),
                       ac3::golden::kGoldenImpulse100Coeffs) < 1e-10);
    CHECK(max_abs_diff(forward_of(ac3::golden::kGoldenDcInput), ac3::golden::kGoldenDcCoeffs) <
          1e-10);
    CHECK(max_abs_diff(forward_of(ac3::golden::kGoldenSineInput), ac3::golden::kGoldenSineCoeffs) <
          1e-10);
    CHECK(max_abs_diff(forward_of(ac3::golden::kGoldenRandomInput),
                       ac3::golden::kGoldenRandomCoeffs) < 1e-10);
}

TEST_CASE("short forward MDCT matches numpy goldens", "[mdct]") {
    const auto check = [](const std::array<double, 512>& input, std::span<const double> first,
                          std::span<const double> second) {
        const auto coeffs = short_forward_of(input);
        CHECK(max_abs_diff(coeffs.first, first) < 1e-10);
        CHECK(max_abs_diff(coeffs.second, second) < 1e-10);
    };
    check(ac3::golden::kGoldenImpulse0Input, ac3::golden::kGoldenImpulse0Short1Coeffs,
          ac3::golden::kGoldenImpulse0Short2Coeffs);
    check(ac3::golden::kGoldenImpulse100Input, ac3::golden::kGoldenImpulse100Short1Coeffs,
          ac3::golden::kGoldenImpulse100Short2Coeffs);
    check(ac3::golden::kGoldenDcInput, ac3::golden::kGoldenDcShort1Coeffs,
          ac3::golden::kGoldenDcShort2Coeffs);
    check(ac3::golden::kGoldenSineInput, ac3::golden::kGoldenSineShort1Coeffs,
          ac3::golden::kGoldenSineShort2Coeffs);
    check(ac3::golden::kGoldenRandomInput, ac3::golden::kGoldenRandomShort1Coeffs,
          ac3::golden::kGoldenRandomShort2Coeffs);
}

TEST_CASE("IMDCT of silence is silence", "[mdct]") {
    std::array<double, 256> coeffs{};
    std::array<double, 512> x{};
    x.fill(1.0);  // must be overwritten with zeros
    ac3::imdct512_windowed(coeffs, x);
    for (const double v : x) {
        CHECK(v == 0.0);
    }
}

TEST_CASE("short IMDCT of silence is silence", "[mdct]") {
    std::array<double, 256> coeffs{};
    std::array<double, 512> x{};
    x.fill(1.0);  // must be overwritten with zeros
    ac3::imdct256_pair_windowed(coeffs, x);
    for (const double v : x) {
        CHECK(v == 0.0);
    }
}

TEST_CASE("TDAC round-trip through the normative inverse reconstructs input", "[mdct]") {
    // Encode 50%-overlapped blocks with the forward transform, decode each
    // with the normative 7.9.4.1 inverse, and reconstruct via the step-6
    // overlap-add: pcm[n] = 2 * (x[n] + delay[n]). Interior samples must
    // come back bit-near-exact; this jointly validates the window, both
    // transforms, and the -2/N vs x2 level convention.
    constexpr int kHop = 256;
    constexpr int kLength = 2048;

    std::mt19937 rng(0x52A3);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<double> input(kLength);
    for (auto& s : input) {
        s = dist(rng);
    }

    const int block_count = (kLength - 512) / kHop + 1;  // 7 blocks
    std::vector<std::array<double, 512>> decoded(static_cast<std::size_t>(block_count));
    for (int b = 0; b < block_count; ++b) {
        std::array<double, 512> block{};
        for (int n = 0; n < 512; ++n) {
            block[static_cast<std::size_t>(n)] = input[static_cast<std::size_t>(b * kHop + n)];
        }
        std::array<double, 512> windowed{};
        std::array<double, 256> coeffs{};
        ac3::apply_analysis_window(block, windowed);
        ac3::mdct512_forward(windowed, coeffs);
        ac3::imdct512_windowed(coeffs, decoded[static_cast<std::size_t>(b)]);
    }

    double worst = 0.0;
    for (int b = 1; b < block_count; ++b) {
        const auto& current = decoded[static_cast<std::size_t>(b)];
        const auto& previous = decoded[static_cast<std::size_t>(b - 1)];
        for (int n = 0; n < kHop; ++n) {
            const double pcm =
                2.0 * (current[static_cast<std::size_t>(n)] +
                       previous[static_cast<std::size_t>(kHop + n)]);
            worst = std::max(worst,
                             std::abs(pcm - input[static_cast<std::size_t>(b * kHop + n)]));
        }
    }
    CHECK(worst < 1e-10);
}

TEST_CASE("long-short-long overlap-add reconstructs input across the transition", "[mdct]") {
    // §7.9.2: a block-switched block's two 128-coefficient half-transforms
    // are interleaved bin-by-bin into one ordinary 256-coefficient set before
    // being handed to (in the decoder's case, read back out of) the same
    // transmission path a long block uses - so this builds that interleaved
    // set by hand and feeds it to imdct256_pair_windowed, exactly as the
    // decoder wiring will. Blocks straddling the switch (2 and 4) are what
    // actually exercise the overlap-add boundary between a long and a short
    // transform's windowed output.
    constexpr int kHop = 256;
    constexpr int kLength = 2048;
    constexpr int kBlockCount = (kLength - 512) / kHop + 1;  // 7 blocks
    constexpr bool kShort[kBlockCount] = {false, false, true, true, false, false, false};

    std::mt19937 rng(0x8811);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<double> input(kLength);
    for (auto& s : input) {
        s = dist(rng);
    }

    std::vector<std::array<double, 512>> decoded(static_cast<std::size_t>(kBlockCount));
    for (int b = 0; b < kBlockCount; ++b) {
        std::array<double, 512> block{};
        for (int n = 0; n < 512; ++n) {
            block[static_cast<std::size_t>(n)] = input[static_cast<std::size_t>(b * kHop + n)];
        }
        std::array<double, 512> windowed{};
        ac3::apply_analysis_window(block, windowed);
        if (!kShort[b]) {
            std::array<double, 256> coeffs{};
            ac3::mdct512_forward(windowed, coeffs);
            ac3::imdct512_windowed(coeffs, decoded[static_cast<std::size_t>(b)]);
            continue;
        }
        const std::span<const double, 512> full(windowed);
        std::array<double, 128> first{};
        std::array<double, 128> second{};
        ac3::mdct256_forward_first(full.first<256>(), first);
        ac3::mdct256_forward_second(full.last<256>(), second);
        std::array<double, 256> interleaved{};
        for (int k = 0; k < 128; ++k) {
            interleaved[static_cast<std::size_t>(2 * k)] = first[static_cast<std::size_t>(k)];
            interleaved[static_cast<std::size_t>(2 * k + 1)] = second[static_cast<std::size_t>(k)];
        }
        ac3::imdct256_pair_windowed(interleaved, decoded[static_cast<std::size_t>(b)]);
    }

    double worst = 0.0;
    for (int b = 1; b < kBlockCount; ++b) {
        const auto& current = decoded[static_cast<std::size_t>(b)];
        const auto& previous = decoded[static_cast<std::size_t>(b - 1)];
        for (int n = 0; n < kHop; ++n) {
            const double pcm =
                2.0 * (current[static_cast<std::size_t>(n)] +
                       previous[static_cast<std::size_t>(kHop + n)]);
            worst = std::max(worst,
                             std::abs(pcm - input[static_cast<std::size_t>(b * kHop + n)]));
        }
    }
    CHECK(worst < 1e-10);
}
