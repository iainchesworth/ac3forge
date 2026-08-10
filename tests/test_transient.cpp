#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "ac3/encoder/transient.hpp"

namespace {

// Non-overlapping 512-sample windows of a steady tone/silence: the biquad
// and peak-tree history only care about a continuous signal flowing
// through, not the encoder's own overlapping-window convention, so this is
// enough to exercise the detector on its own terms.
std::vector<std::array<double, 512>> windows_of(const std::vector<double>& signal) {
    std::vector<std::array<double, 512>> out;
    for (std::size_t i = 0; i + 512 <= signal.size(); i += 512) {
        std::array<double, 512> w{};
        for (std::size_t n = 0; n < 512; ++n) {
            w[n] = signal[i + n];
        }
        out.push_back(w);
    }
    return out;
}

}  // namespace

TEST_CASE("a steady tone never trips the transient detector", "[transient]") {
    std::vector<double> signal(512 * 8);
    for (std::size_t n = 0; n < signal.size(); ++n) {
        signal[n] = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 / 48000.0 *
                                   static_cast<double>(n));
    }
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    for (const auto& w : windows_of(signal)) {
        CHECK_FALSE(detector.detect(w));
    }
}

TEST_CASE("digital silence never trips the transient detector", "[transient]") {
    std::vector<double> signal(512 * 4, 0.0);
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    for (const auto& w : windows_of(signal)) {
        CHECK_FALSE(detector.detect(w));
    }
}

TEST_CASE("a sudden loud onset in a block's second half trips the detector",
         "[transient]") {
    // Three windows of silence to settle history and clear the first-pass
    // guard, then a fourth window that is silent in its first half and a
    // loud 1 kHz tone in its second half - exactly the case §8.2.2 defines
    // blksw for.
    std::vector<double> signal(512 * 3, 0.0);
    signal.resize(512 * 4, 0.0);
    for (std::size_t n = 512 * 3 + 256; n < signal.size(); ++n) {
        signal[n] = 0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 / 48000.0 *
                                   static_cast<double>(n));
    }
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    const auto windows = windows_of(signal);
    for (std::size_t i = 0; i + 1 < windows.size(); ++i) {
        CHECK_FALSE(detector.detect(windows[i]));
    }
    CHECK(detector.detect(windows.back()));
}

TEST_CASE("an onset in the first half alone does not trip blksw", "[transient]") {
    // The mirror case: loud tone starts at the very beginning of a block
    // (its FIRST half) and holds steady through the second half. §8.2.2
    // defines blksw from the SECOND half only, and a steady second half
    // relative to its own (now-loud) first half is not a transient.
    std::vector<double> signal(512 * 3, 0.0);
    signal.resize(512 * 4, 0.0);
    for (std::size_t n = 512 * 3; n < signal.size(); ++n) {
        signal[n] = 0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 / 48000.0 *
                                   static_cast<double>(n));
    }
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    const auto windows = windows_of(signal);
    for (std::size_t i = 0; i + 1 < windows.size(); ++i) {
        CHECK_FALSE(detector.detect(windows[i]));
    }
    CHECK_FALSE(detector.detect(windows.back()));
}
