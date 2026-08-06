#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "ac3/core/tables.hpp"

TEST_CASE("frame geometry constants", "[tables]") {
    STATIC_CHECK(ac3::kSyncWord == 0x0B77);
    STATIC_CHECK(ac3::kSamplesPerFrame == 1536);
    STATIC_CHECK(ac3::kBlocksPerFrame * ac3::kSamplesPerBlock == ac3::kSamplesPerFrame);
}

TEST_CASE("the 19 legal bit rates, ascending", "[tables]") {
    STATIC_CHECK(ac3::kBitratesKbps.size() == 19);
    STATIC_CHECK(ac3::kBitratesKbps.front() == 32);
    STATIC_CHECK(ac3::kBitratesKbps.back() == 640);
    CHECK(std::ranges::is_sorted(ac3::kBitratesKbps));
    CHECK(ac3::is_valid_bitrate(448));
    CHECK_FALSE(ac3::is_valid_bitrate(100));
}

TEST_CASE("sample rate codes (A/52 5.4.1.3)", "[tables]") {
    STATIC_CHECK(ac3::sample_rate_hz(ac3::SampleRate::k48000) == 48000);
    STATIC_CHECK(ac3::sample_rate_hz(ac3::SampleRate::k44100) == 44100);
    STATIC_CHECK(ac3::sample_rate_hz(ac3::SampleRate::k32000) == 32000);
    STATIC_CHECK(static_cast<int>(ac3::SampleRate::k48000) == 0);
    STATIC_CHECK(static_cast<int>(ac3::SampleRate::k44100) == 1);
    STATIC_CHECK(static_cast<int>(ac3::SampleRate::k32000) == 2);
}

TEST_CASE("acmod channel counts (A/52 5.4.2.3)", "[tables]") {
    using ac3::Acmod;
    STATIC_CHECK(ac3::fullbw_channel_count(Acmod::kDualMono) == 2);
    STATIC_CHECK(ac3::fullbw_channel_count(Acmod::k1_0) == 1);
    STATIC_CHECK(ac3::fullbw_channel_count(Acmod::k2_0) == 2);
    STATIC_CHECK(ac3::fullbw_channel_count(Acmod::k3_2) == 5);  // 5.1 minus the LFE
}

TEST_CASE("exact frame sizes at 48 and 32 kHz", "[tables]") {
    using ac3::SampleRate;
    // Verified anchors: 640 kbit/s @ 48 kHz is exactly 2560 bytes per frame.
    STATIC_CHECK(ac3::frame_size_bytes(SampleRate::k48000, 640) == 2560u);
    STATIC_CHECK(ac3::frame_size_bytes(SampleRate::k48000, 448) == 1792u);
    STATIC_CHECK(ac3::frame_size_bytes(SampleRate::k48000, 192) == 768u);
    STATIC_CHECK(ac3::frame_size_bytes(SampleRate::k32000, 640) == 3840u);

    // 44.1 kHz needs the verbatim Table 5.18 column (padding alternation).
    STATIC_CHECK(ac3::frame_size_bytes(SampleRate::k44100, 448) == std::nullopt);

    // Illegal bit rates are rejected at any sample rate.
    STATIC_CHECK(ac3::frame_size_bytes(SampleRate::k48000, 100) == std::nullopt);
}
