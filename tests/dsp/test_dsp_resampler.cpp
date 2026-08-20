#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/dsp/resampler.hpp"

using ac3::dsp::resample;
using ac3::dsp::resample_planar;

namespace {

// Mono sine generator - deliberately local rather than shared with
// tests/audio/test_resampler.cpp's own generate_sine/generate_sine_mono: these are
// two independent test binaries' worth of helpers (ac3::dsp::resample is a
// plain single-channel std::vector<float>, not interleaved multi-channel PCM
// like ac3::audio::DriftResampler operates on), and the rest of tests/
// already keeps this kind of helper local per file rather than sharing a
// header for it.
std::vector<float> generate_sine(std::size_t frames, double freq, double sample_rate,
                                  double amplitude = 0.8) {
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(n) / sample_rate));
    }
    return out;
}

// Average rising-zero-crossing period, converted to a frequency estimate.
// Same technique tests/audio/test_resampler.cpp uses for DriftResampler - cheap
// and robust enough for the tolerances below without an FFT dependency.
double measured_frequency(std::span<const float> mono, double sample_rate) {
    std::vector<std::size_t> crossings;
    for (std::size_t i = 1; i < mono.size(); ++i) {
        if (mono[i - 1] < 0.0f && mono[i] >= 0.0f) {
            crossings.push_back(i);
        }
    }
    REQUIRE(crossings.size() >= 4);
    double total = 0.0;
    for (std::size_t i = 1; i < crossings.size(); ++i) {
        total += static_cast<double>(crossings[i] - crossings[i - 1]);
    }
    const double avg_period = total / static_cast<double>(crossings.size() - 1);
    return sample_rate / avg_period;
}

double rms(std::span<const float> x) {
    double sum_sq = 0.0;
    for (const float v : x) {
        sum_sq += static_cast<double>(v) * static_cast<double>(v);
    }
    return std::sqrt(sum_sq / static_cast<double>(x.size()));
}

std::size_t expected_out_frames(std::size_t in_frames, std::uint32_t input_rate,
                                 std::uint32_t output_rate) {
    const double ratio = static_cast<double>(output_rate) / static_cast<double>(input_rate);
    return static_cast<std::size_t>(std::llround(static_cast<double>(in_frames) * ratio));
}

}  // namespace

TEST_CASE("resample at a 1:1 ratio reproduces the input exactly", "[dsp][resampler]") {
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;  // 2 seconds
    const auto input = generate_sine(kFrames, 440.0, kRate);

    const auto output = resample(input, kRate, kRate);

    REQUIRE(output.size() == input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        CHECK(output[i] == input[i]);
    }
}

TEST_CASE("resample from an empty buffer produces an empty buffer", "[dsp][resampler]") {
    const std::vector<float> empty;
    CHECK(resample(empty, 44100, 48000).empty());
    // Same rate too - the empty check must win over the identity-copy path.
    CHECK(resample(empty, 48000, 48000).empty());
}

TEST_CASE("resample with a zero input or output rate returns empty rather than dividing by zero",
          "[dsp][resampler]") {
    const auto input = generate_sine(1000, 440.0, 48000.0);
    CHECK(resample(input, 0, 48000).empty());
    CHECK(resample(input, 44100, 0).empty());
}

TEST_CASE("resample from 44100 to 48000 preserves a 1kHz tone's frequency", "[dsp][resampler]") {
    constexpr std::uint32_t kInRate = 44100;
    constexpr std::uint32_t kOutRate = 48000;
    constexpr double kFreq = 1000.0;
    constexpr std::size_t kInFrames = kInRate * 3;  // 3 seconds

    const auto input = generate_sine(kInFrames, kFreq, kInRate);
    const auto output = resample(input, kInRate, kOutRate);

    REQUIRE(output.size() == expected_out_frames(kInFrames, kInRate, kOutRate));

    // Trim an edge margin (comfortably wider than the kernel's own
    // half-width) before measuring, so the shortened/truncated kernel right
    // at the very start and end of the buffer can't skew the zero-crossing
    // average.
    constexpr std::size_t kEdgeTrim = 500;
    const std::span<const float> measured_span(output.data() + kEdgeTrim, output.size() - 2 * kEdgeTrim);
    const double measured = measured_frequency(measured_span, kOutRate);
    // A "just repeat/hold the last frame" or "return silence" stub would
    // either fail measured_frequency's own REQUIRE(crossings >= 4) outright
    // (a constant/silent signal has none) or - if it degenerated into some
    // other constant tone - miss this tight a frequency match.
    CHECK(measured == Catch::Approx(kFreq).epsilon(0.03));
}

TEST_CASE("resample from 48000 to 44100 preserves a 1kHz tone's frequency", "[dsp][resampler]") {
    constexpr std::uint32_t kInRate = 48000;
    constexpr std::uint32_t kOutRate = 44100;
    constexpr double kFreq = 1000.0;
    constexpr std::size_t kInFrames = kInRate * 3;  // 3 seconds

    const auto input = generate_sine(kInFrames, kFreq, kInRate);
    const auto output = resample(input, kInRate, kOutRate);

    REQUIRE(output.size() == expected_out_frames(kInFrames, kInRate, kOutRate));

    constexpr std::size_t kEdgeTrim = 500;
    const std::span<const float> measured_span(output.data() + kEdgeTrim, output.size() - 2 * kEdgeTrim);
    const double measured = measured_frequency(measured_span, kOutRate);
    CHECK(measured == Catch::Approx(kFreq).epsilon(0.03));
}

TEST_CASE("resample attenuates a tone above the new Nyquist instead of aliasing it down",
          "[dsp][resampler]") {
    // 48000 -> 24000: new Nyquist is 12000Hz. An 18kHz input tone is well
    // below the INPUT Nyquist (24000Hz, so it's a legitimate, alias-free
    // input signal) but well above the new, lower OUTPUT Nyquist - if the
    // resampler naively decimated without lowpass filtering first, this
    // component would fold (alias) down to 24000 - 18000 = 6000Hz, landing
    // squarely and audibly inside the passband. A working windowed-sinc
    // resampler kills it before decimation instead: kCutoffBackoff *
    // limiting_rate/2 = 0.9 * 24000/2 = 10800Hz, comfortably below 18000Hz,
    // deep into the Blackman-windowed kernel's stopband.
    constexpr std::uint32_t kInRate = 48000;
    constexpr std::uint32_t kOutRate = 24000;
    constexpr double kAboveNyquistFreq = 18000.0;
    constexpr double kAmplitude = 0.8;
    constexpr std::size_t kInFrames = kInRate * 2;  // 2 seconds

    const auto input = generate_sine(kInFrames, kAboveNyquistFreq, kInRate, kAmplitude);
    const auto output = resample(input, kInRate, kOutRate);
    REQUIRE(output.size() == expected_out_frames(kInFrames, kInRate, kOutRate));

    constexpr std::size_t kEdgeTrim = 200;
    const std::span<const float> measured_span(output.data() + kEdgeTrim, output.size() - 2 * kEdgeTrim);

    const double input_rms = kAmplitude / std::numbers::sqrt2;
    const double output_rms = rms(measured_span);
    CAPTURE(input_rms, output_rms);

    // A resampler that just decimated (dropped every other sample) rather
    // than filtering first would reproduce the aliased 6kHz tone at very
    // close to full amplitude, giving output_rms close to input_rms. A
    // working filter drives it down hard; this threshold (10% of the input
    // RMS, ~20dB of attenuation) sits comfortably below where a genuinely
    // filtered stopband component lands but far above what an unfiltered
    // alias would leave.
    CHECK(output_rms < input_rms * 0.1);
}

TEST_CASE("resample_planar matches per-channel resample() and keeps channels independent",
          "[dsp][resampler]") {
    constexpr std::uint32_t kInRate = 44100;
    constexpr std::uint32_t kOutRate = 48000;
    constexpr std::size_t kInFrames = kInRate * 2;  // 2 seconds

    // Genuinely different content per channel - a copy/mixup bug (e.g.
    // resample_planar always resampling channel 0 for every output channel)
    // would show up as channel 1's output matching channel 0's tone instead
    // of its own.
    const auto ch0 = generate_sine(kInFrames, 300.0, kInRate);
    const auto ch1 = generate_sine(kInFrames, 1200.0, kInRate);
    const std::vector<std::vector<float>> channels = {ch0, ch1};

    const auto planar = resample_planar(channels, kInRate, kOutRate);
    REQUIRE(planar.size() == 2);

    const auto individual0 = resample(ch0, kInRate, kOutRate);
    const auto individual1 = resample(ch1, kInRate, kOutRate);

    REQUIRE(planar[0].size() == individual0.size());
    REQUIRE(planar[1].size() == individual1.size());
    for (std::size_t i = 0; i < individual0.size(); ++i) {
        CHECK(planar[0][i] == individual0[i]);
    }
    for (std::size_t i = 0; i < individual1.size(); ++i) {
        CHECK(planar[1][i] == individual1[i]);
    }

    // Each channel's own tone survives the conversion under its own
    // frequency, not swapped with the other's.
    constexpr std::size_t kEdgeTrim = 500;
    const std::span<const float> span0(planar[0].data() + kEdgeTrim, planar[0].size() - 2 * kEdgeTrim);
    const std::span<const float> span1(planar[1].data() + kEdgeTrim, planar[1].size() - 2 * kEdgeTrim);
    CHECK(measured_frequency(span0, kOutRate) == Catch::Approx(300.0).epsilon(0.03));
    CHECK(measured_frequency(span1, kOutRate) == Catch::Approx(1200.0).epsilon(0.03));
}

TEST_CASE("resample preserves the amplitude of a safely in-band tone", "[dsp][resampler]") {
    // 1kHz sits nowhere near either the 44.1kHz input's or 48kHz output's
    // Nyquist, so this is squarely in the filter's flat passband - no
    // significant gain or attenuation should survive the conversion.
    constexpr std::uint32_t kInRate = 44100;
    constexpr std::uint32_t kOutRate = 48000;
    constexpr double kFreq = 1000.0;
    constexpr double kAmplitude = 0.8;
    constexpr std::size_t kInFrames = kInRate * 2;  // 2 seconds

    const auto input = generate_sine(kInFrames, kFreq, kInRate, kAmplitude);
    const auto output = resample(input, kInRate, kOutRate);
    REQUIRE(output.size() == expected_out_frames(kInFrames, kInRate, kOutRate));

    constexpr std::size_t kEdgeTrim = 500;
    const std::span<const float> measured_span(output.data() + kEdgeTrim, output.size() - 2 * kEdgeTrim);

    const double input_rms = kAmplitude / std::numbers::sqrt2;
    const double output_rms = rms(measured_span);
    CAPTURE(input_rms, output_rms);

    // Within 5% of the input's RMS - a filter with a passband gain bug
    // (attenuating or amplifying in-band content, e.g. a wrong kernel
    // normalization) would miss this; silence or a badly-scaled stub would
    // miss it by far more than 5%.
    CHECK(output_rms == Catch::Approx(input_rms).epsilon(0.05));
}
