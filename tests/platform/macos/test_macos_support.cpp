#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "coreaudio_names.hpp"

// The macOS backend's pure half, tested directly - see coreaudio_names.hpp's
// own header comment for why "pure" means "touches no live HAL object" here,
// rather than "builds without CoreAudio.framework" the way
// tests/platform/alsa/test_alsa_device_names.cpp's device_names.hpp is pure
// of libasound: CMake only ever selects this file on a real macOS host, so
// CoreAudio.framework is always present when it runs.

using ac3::coreaudio::bytes_per_sample;
using ac3::coreaudio::carrier_rate;
using ac3::coreaudio::classify_pcm;
using ac3::coreaudio::fallback_name;
using ac3::coreaudio::find_physical_format;
using ac3::coreaudio::float_to_samples;
using ac3::coreaudio::physical_format_id;
using ac3::coreaudio::samples_to_float;
using ac3::coreaudio::SampleFormat;
using ac3::audio::BitstreamFormat;

TEST_CASE("E-AC-3 runs the carrier four times as fast as its content") {
    // Same physical fact platform/alsa/device_names.hpp and
    // apps/android/android_support.hpp both encode - see their own
    // tests for the full rationale.
    CHECK(carrier_rate(BitstreamFormat::kAc3, 48000) == 48000);
    CHECK(carrier_rate(BitstreamFormat::kAc3, 44100) == 44100);
    CHECK(carrier_rate(BitstreamFormat::kAc3, 32000) == 32000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 48000) == 192000);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 44100) == 176400);
    CHECK(carrier_rate(BitstreamFormat::kEac3, 32000) == 128000);
}

TEST_CASE("the physical format id matches the bitstream kind") {
    CHECK(physical_format_id(BitstreamFormat::kAc3) == kAudioFormat60958AC3);
    CHECK(physical_format_id(BitstreamFormat::kEac3) == kAudioFormatEnhancedAC3);
}

TEST_CASE("a stream's available formats are searched for a matching id and rate") {
    AudioStreamRangedDescription pcm{};
    pcm.mFormat.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormat.mSampleRate = 48000.0;
    pcm.mSampleRateRange = {48000.0, 48000.0};

    AudioStreamRangedDescription ac3{};
    ac3.mFormat.mFormatID = kAudioFormat60958AC3;
    ac3.mFormat.mChannelsPerFrame = 2;
    ac3.mFormat.mBitsPerChannel = 16;
    // A driver publishing a rate RANGE rather than one point, exactly the
    // shape kAudioStreamPropertyAvailablePhysicalFormats is documented to
    // return.
    ac3.mSampleRateRange = {32000.0, 48000.0};

    const std::array<AudioStreamRangedDescription, 2> formats{pcm, ac3};

    const auto match = find_physical_format(formats, kAudioFormat60958AC3, 48000.0);
    REQUIRE(match.has_value());
    CHECK(match->mChannelsPerFrame == 2);
    CHECK(match->mBitsPerChannel == 16);

    // A rate the range does not cover, and a format id nothing offers.
    CHECK_FALSE(find_physical_format(formats, kAudioFormat60958AC3, 96000.0).has_value());
    CHECK_FALSE(find_physical_format(formats, kAudioFormatEnhancedAC3, 48000.0).has_value());
}

TEST_CASE("PCM classification reads the flags the HAL actually sets") {
    AudioStreamBasicDescription float32{};
    float32.mFormatID = kAudioFormatLinearPCM;
    float32.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    float32.mBitsPerChannel = 32;
    CHECK(classify_pcm(float32) == SampleFormat::kFloat32);

    AudioStreamBasicDescription pcm16{};
    pcm16.mFormatID = kAudioFormatLinearPCM;
    pcm16.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    pcm16.mBitsPerChannel = 16;
    CHECK(classify_pcm(pcm16) == SampleFormat::kPcm16);

    AudioStreamBasicDescription pcm32{};
    pcm32.mFormatID = kAudioFormatLinearPCM;
    pcm32.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    pcm32.mBitsPerChannel = 32;
    CHECK(classify_pcm(pcm32) == SampleFormat::kPcm32);

    // A compressed physical format handed to the PCM classifier by mistake -
    // must not be misread as some PCM width.
    AudioStreamBasicDescription compressed{};
    compressed.mFormatID = kAudioFormat60958AC3;
    CHECK(classify_pcm(compressed) == SampleFormat::kUnsupported);

    // A bit width this backend does not claim to read.
    AudioStreamBasicDescription oddball{};
    oddball.mFormatID = kAudioFormatLinearPCM;
    oddball.mFormatFlags = kAudioFormatFlagIsSignedInteger;
    oddball.mBitsPerChannel = 8;
    CHECK(classify_pcm(oddball) == SampleFormat::kUnsupported);
}

TEST_CASE("float32 samples round-trip unchanged") {
    const std::vector<float> in{-1.0f, 0.0f, 0.5f, 1.0f};
    std::vector<std::byte> raw(in.size() * bytes_per_sample(SampleFormat::kFloat32));
    float_to_samples(in, SampleFormat::kFloat32, raw.data());

    std::vector<float> out(in.size());
    samples_to_float(raw.data(), in.size(), SampleFormat::kFloat32, out);
    CHECK(out == in);
}

TEST_CASE("16-bit samples clamp instead of wrapping") {
    const std::vector<float> in{-2.0f, 2.0f};  // outside [-1, 1] on purpose
    std::vector<std::byte> raw(in.size() * bytes_per_sample(SampleFormat::kPcm16));
    float_to_samples(in, SampleFormat::kPcm16, raw.data());

    std::int16_t low = 0;
    std::int16_t high = 0;
    std::memcpy(&low, raw.data(), sizeof(low));
    std::memcpy(&high, raw.data() + sizeof(low), sizeof(high));
    // Clamped to full scale, not wrapped to the opposite polarity - the same
    // property platform/alsa/monitor.cpp's own convert() guards, and the
    // same asymmetric [-32767, 32767] convention it uses (not -32768).
    CHECK(low == -32767);
    CHECK(high == 32767);

    std::vector<float> back(in.size());
    samples_to_float(raw.data(), in.size(), SampleFormat::kPcm16, back);
    CHECK(back[0] < -0.99f);
    CHECK(back[1] > 0.99f);
}

TEST_CASE("32-bit integer samples round-trip within quantisation error") {
    const std::vector<float> in{-1.0f, 0.25f, 1.0f};
    std::vector<std::byte> raw(in.size() * bytes_per_sample(SampleFormat::kPcm32));
    float_to_samples(in, SampleFormat::kPcm32, raw.data());

    std::vector<float> out(in.size());
    samples_to_float(raw.data(), in.size(), SampleFormat::kPcm32, out);
    for (std::size_t i = 0; i < in.size(); ++i) {
        CHECK(std::abs(out[i] - in[i]) < 0.0001f);
    }
}

TEST_CASE("a device with no CFStringRef UID names nothing openable") {
    // fallback_name is what a caller sees instead of a blank row - never
    // empty itself, matching every other backend's own display-name
    // discipline.
    CHECK(fallback_name("") == "Unnamed audio endpoint");
    CHECK(fallback_name("abc-123") == "Unnamed endpoint abc-123");
}
