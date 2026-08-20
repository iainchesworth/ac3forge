#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/dsp/biquad.hpp"

using ac3::dsp::Biquad;
using ac3::dsp::LfeLowpass;

namespace {

// LFE corner this project actually uses (see biquad.hpp's file comment).
constexpr double kCornerHz = 120.0;

// Plain single-tone generator. Real multi-cycle content, not a handful of
// samples or silence - per this project's hard-earned rule, a trivial or
// frame-0 test gives a false pass on a filter with real, time-dependent
// state.
std::vector<float> generate_sine(std::size_t n, double freq, double sample_rate,
                                  double amplitude = 0.5) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) /
                                  sample_rate));
    }
    return out;
}

double rms(std::span<const float> samples) {
    double sum_sq = 0.0;
    for (float s : samples) {
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum_sq / static_cast<double>(samples.size()));
}

// Attenuation of a single tone through a fresh LfeLowpass, measured well
// after the filter's own settling transient has died out (see kSettleFrac
// below), as an RMS ratio in dB - negative means attenuated, 0 means
// unchanged. One second of signal at a realistic sample rate gives dozens
// of cycles even at 40 Hz, so the measurement window is not itself too
// short to be a stable estimate.
double measure_attenuation_db(double freq, std::uint32_t sample_rate) {
    constexpr double kSettleFrac = 0.3;  // seconds' worth of signal, see below
    const auto total = static_cast<std::size_t>(sample_rate);  // 1.0 s
    const auto settle = static_cast<std::size_t>(static_cast<double>(sample_rate) * kSettleFrac);

    const std::vector<float> input = generate_sine(total, freq, sample_rate);
    std::vector<float> output = input;

    LfeLowpass filter(kCornerHz, sample_rate);
    filter.process(output);

    const std::span<const float> in_measure(input.data() + settle, total - settle);
    const std::span<const float> out_measure(output.data() + settle, total - settle);

    const double in_rms = rms(in_measure);
    const double out_rms = rms(out_measure);
    REQUIRE(in_rms > 0.0);
    return 20.0 * std::log10(out_rms / in_rms);
}

}  // namespace

TEST_CASE("a tone well below the corner passes through nearly unattenuated", "[dsp][biquad]") {
    // 40 Hz vs a 120 Hz corner, at a realistic 48 kHz rate. 4th-order
    // Butterworth theory puts this at a small fraction of a dB down; 1 dB
    // (~10.9% amplitude) gives generous room for the digital bilinear-
    // transform implementation to differ from the ideal analog prototype
    // while still catching a badly wrong corner or slope.
    const double attenuation_db = measure_attenuation_db(40.0, 48000);
    REQUIRE(attenuation_db > -1.0);
    REQUIRE(attenuation_db <= 0.0);  // a low-pass never amplifies
}

TEST_CASE("a tone well above the corner is attenuated by tens of dB", "[dsp][biquad]") {
    // 1 kHz vs a 120 Hz corner is nearly 3 octaves up; at ~24 dB/octave a
    // 4th-order Butterworth puts this on the order of -70 dB. 30 dB is a
    // conservative floor, comfortably below the ideal answer, so the test is
    // robust to minor implementation variance while still clearly failing
    // for a broken or no-op filter (which would read ~0 dB here).
    const double attenuation_db = measure_attenuation_db(1000.0, 48000);
    REQUIRE(attenuation_db <= -30.0);
}

TEST_CASE("a tone at the corner frequency is attenuated by roughly -3dB", "[dsp][biquad]") {
    // At f == corner_hz, Butterworth theory (of any order, split into any
    // valid pole grouping) puts the combined magnitude at exactly -3.0103
    // dB in the analog prototype - that is the entire point of tuning the
    // two stages' Q values the way biquad.cpp does rather than giving both
    // stages the same Q. The digital, bilinear-transformed filter should sit
    // close to that, but this test does not assume the exact analytic value
    // holds to high precision through the transform - it checks a sane band
    // (-1 to -6 dB) around -3 dB instead, which still clearly fails for a
    // filter with the wrong corner, the wrong order, or no attenuation at
    // all.
    const double attenuation_db = measure_attenuation_db(kCornerHz, 48000);
    REQUIRE(attenuation_db > -6.0);
    REQUIRE(attenuation_db < -1.0);
}

TEST_CASE("frame-by-frame process() matches processing the whole signal at once",
          "[dsp][biquad]") {
    // The real caller (the encoder's per-frame render path) calls process()
    // once per ~1536-sample AC-3 frame for a run's whole life, carrying
    // state across calls - this is the single most important behavioral
    // contract LfeLowpass has to honor. A composite two-tone signal (below
    // and above the corner) exercises both passband and stopband behavior
    // across the chunk boundaries, and a length that is NOT a multiple of
    // the chunk size (one short final chunk) matches how a real file's last
    // partial frame is handled.
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kChunk = 1536;  // ac3::kSamplesPerFrame
    constexpr std::size_t kTotal = 4 * kChunk + 700;

    std::vector<float> low = generate_sine(kTotal, 40.0, kSampleRate, 0.4);
    std::vector<float> high = generate_sine(kTotal, 1000.0, kSampleRate, 0.4);
    std::vector<float> composite(kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
        composite[i] = low[i] + high[i];
    }

    std::vector<float> whole = composite;
    LfeLowpass whole_filter(kCornerHz, kSampleRate);
    whole_filter.process(whole);

    std::vector<float> chunked = composite;
    LfeLowpass chunked_filter(kCornerHz, kSampleRate);
    std::size_t offset = 0;
    while (offset < kTotal) {
        const std::size_t n = std::min(kChunk, kTotal - offset);
        chunked_filter.process(std::span<float>(chunked.data() + offset, n));
        offset += n;
    }

    REQUIRE(whole.size() == chunked.size());
    for (std::size_t i = 0; i < kTotal; ++i) {
        REQUIRE(chunked[i] == Catch::Approx(whole[i]).margin(1e-6));
    }
}

TEST_CASE("reset() zeroes delay-line state so a later signal carries no old tail",
          "[dsp][biquad]") {
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kToneSamples = 2000;
    constexpr std::size_t kSilenceSamples = 2000;

    // Control: without reset(), a loud tone's filter memory decays into
    // whatever follows it - if it did NOT, this whole test would be trivial
    // (silence-in-gives-silence-out is true of an untouched filter too, so
    // it alone would be a false pass per this project's rule about trivial
    // tests). Confirm the tail is genuinely there first.
    {
        LfeLowpass filter(kCornerHz, kSampleRate);
        std::vector<float> tone = generate_sine(kToneSamples, 300.0, kSampleRate, 0.9);
        filter.process(tone);
        std::vector<float> silence(kSilenceSamples, 0.0f);
        filter.process(silence);
        const bool has_tail =
            std::any_of(silence.begin(), silence.end(), [](float s) { return std::abs(s) > 1e-6f; });
        REQUIRE(has_tail);
    }

    // Now with reset() between the tone and the silence: the tail must be
    // gone, not just smaller.
    {
        LfeLowpass filter(kCornerHz, kSampleRate);
        std::vector<float> tone = generate_sine(kToneSamples, 300.0, kSampleRate, 0.9);
        filter.process(tone);
        filter.reset();
        std::vector<float> silence(kSilenceSamples, 0.0f);
        filter.process(silence);
        for (float s : silence) {
            REQUIRE(std::abs(s) < 1e-9f);
        }
    }
}

TEST_CASE("a default-constructed Biquad is a unity pass-through", "[dsp][biquad]") {
    // b0=1, everything else 0 (biquad.hpp's field defaults) is the identity
    // filter - process() should hand every sample back unchanged, and never
    // accumulate state since z1_/z2_ stay 0 when a1_==a2_==b1_==b2_==0.
    Biquad biquad;
    const std::vector<float> input = generate_sine(64, 1000.0, 48000.0, 0.7);
    for (float s : input) {
        REQUIRE(biquad.process(s) == Catch::Approx(s).margin(1e-7));
    }
}

TEST_CASE("Biquad::reset clears state but keeps coefficients", "[dsp][biquad]") {
    Biquad biquad;
    // An arbitrary stable low-pass-shaped section, just to have non-trivial
    // coefficients that leave real delay-line state behind.
    biquad.set_coefficients(0.1, 0.2, 0.1, -1.0, 0.4);

    const std::vector<float> tone = generate_sine(500, 300.0, 48000.0, 0.9);
    std::vector<float> first_pass(tone.size());
    for (std::size_t i = 0; i < tone.size(); ++i) {
        first_pass[i] = biquad.process(tone[i]);
    }

    // Feeding one more (silent) sample right now would NOT be zero, because
    // state is non-trivial after 500 samples of a loud tone.
    REQUIRE(std::abs(biquad.process(0.0f)) > 1e-6f);

    biquad.reset();

    // Immediately after reset(), a zero input must produce exactly zero
    // output (z1_ == 0), proving state was actually cleared.
    REQUIRE(biquad.process(0.0f) == 0.0f);

    // Coefficients must have survived reset(): replaying the same tone from
    // this fresh (zeroed) state reproduces the first pass's output exactly.
    biquad.reset();
    for (std::size_t i = 0; i < tone.size(); ++i) {
        REQUIRE(biquad.process(tone[i]) == Catch::Approx(first_pass[i]).margin(1e-7));
    }
}
