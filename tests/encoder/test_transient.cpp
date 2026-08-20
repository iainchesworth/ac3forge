#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "ac3/encoder/transient.hpp"

namespace {

// Consecutive 256-sample segments of a continuous signal, in stream order -
// the detector's own calling convention (one call per channel per block
// period, each carrying that period's NEW samples; the previous segment is
// the comparison baseline via the persistent filter/tree state).
std::vector<std::array<float, 256>> segments_of(const std::vector<float>& signal) {
    std::vector<std::array<float, 256>> out;
    for (std::size_t i = 0; i + 256 <= signal.size(); i += 256) {
        std::array<float, 256> s{};
        for (std::size_t n = 0; n < 256; ++n) {
            s[n] = signal[i + n];
        }
        out.push_back(s);
    }
    return out;
}

std::vector<float> tone(std::size_t from, std::size_t count, double amplitude) {
    std::vector<float> signal(from + count, 0.0F);
    for (std::size_t n = from; n < signal.size(); ++n) {
        signal[n] = static_cast<float>(
            amplitude *
            std::sin(2.0 * std::numbers::pi * 1000.0 / 48000.0 * static_cast<double>(n)));
    }
    return signal;
}

}  // namespace

TEST_CASE("a steady tone never trips the transient detector", "[transient]") {
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    for (const auto& s : segments_of(tone(0, 256 * 16, 0.5))) {
        CHECK_FALSE(detector.detect(s));
    }
}

TEST_CASE("digital silence never trips the transient detector", "[transient]") {
    std::vector<float> signal(256 * 8, 0.0F);
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    for (const auto& s : segments_of(signal)) {
        CHECK_FALSE(detector.detect(s));
    }
}

TEST_CASE("a sudden loud onset trips the detector in the onset's own segment",
         "[transient]") {
    // Seven segments of silence to settle history and clear the first-call
    // guard, then a loud 1 kHz tone from segment 7's own first sample -
    // exactly the case §8.2.2 defines blksw for: a transient in the 256 new
    // samples of one block period.
    const auto segments = segments_of(tone(256 * 7, 256 * 2, 0.9));
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK_FALSE(detector.detect(segments[i]));
    }
    CHECK(detector.detect(segments[7]));
    // The segment after the onset holds the tone steady at its new level -
    // no ratio jump against its own (now-loud) predecessor, so no switch.
    CHECK_FALSE(detector.detect(segments[8]));
}

TEST_CASE("the very first segment a detector sees never trips it", "[transient]") {
    // A fresh detector's only baseline is synthetic silence; §8.2.2's
    // comparisons against it would flag any non-silent opening - so the
    // first call's result is suppressed even for a full-scale onset. The
    // SECOND segment, steady at the same level, must not trip either (the
    // suppressed pass still primed real history).
    const auto segments = segments_of(tone(0, 256 * 2, 0.9));
    ac3::TransientDetector detector(ac3::SampleRate::k48000);
    CHECK_FALSE(detector.detect(segments[0]));
    CHECK_FALSE(detector.detect(segments[1]));
}
