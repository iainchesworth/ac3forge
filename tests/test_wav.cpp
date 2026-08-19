#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"

// The integer PCM path exists for the lossless codec, so its one obligation
// is exactness: every sample word out must equal the word that went in, at
// both supported widths. (The float path's tests are the encoder/decoder
// suites that consume it; this file covers the integer API those can't.)

namespace {

// A scratch path under the system temp directory, removed by the fixture.
struct TempWav {
    std::string path;
    explicit TempWav(const char* name)
        : path((std::filesystem::temp_directory_path() / name).string()) {}
    ~TempWav() { std::remove(path.c_str()); }
    TempWav(const TempWav&) = delete;
    TempWav& operator=(const TempWav&) = delete;
};

}  // namespace

TEST_CASE("wav: integer PCM16 round trip is exact", "[wav]") {
    TempWav file{"ac3forge_test_pcm16.wav"};
    std::mt19937 rng(0x3A16);
    std::uniform_int_distribution<std::int32_t> dist(-32768, 32767);
    std::vector<std::vector<std::int32_t>> channels(2, std::vector<std::int32_t>(200));
    for (auto& channel : channels) {
        for (auto& sample : channel) {
            sample = dist(rng);
        }
    }
    // The extremes must survive too, not just typical values.
    channels[0][0] = -32768;
    channels[1][0] = 32767;

    REQUIRE(ac3::io::write_wav_pcm(file.path, channels, 48000, 16).has_value());
    const auto read = ac3::io::read_wav_pcm(file.path);
    REQUIRE(read.has_value());
    CHECK(read->sample_rate == 48000);
    CHECK(read->bits == 16);
    CHECK(read->channels == channels);
}

TEST_CASE("wav: integer PCM24 round trip is exact", "[wav]") {
    TempWav file{"ac3forge_test_pcm24.wav"};
    std::mt19937 rng(0x3A24);
    std::uniform_int_distribution<std::int32_t> dist(-(1 << 23), (1 << 23) - 1);
    std::vector<std::vector<std::int32_t>> channels(3, std::vector<std::int32_t>(150));
    for (auto& channel : channels) {
        for (auto& sample : channel) {
            sample = dist(rng);
        }
    }
    channels[0][0] = -(1 << 23);
    channels[1][0] = (1 << 23) - 1;

    REQUIRE(ac3::io::write_wav_pcm(file.path, channels, 96000, 24).has_value());
    const auto read = ac3::io::read_wav_pcm(file.path);
    REQUIRE(read.has_value());
    CHECK(read->sample_rate == 96000);
    CHECK(read->bits == 24);
    CHECK(read->channels == channels);
}

TEST_CASE("wav: the integer reader rejects a float32 file", "[wav]") {
    TempWav file{"ac3forge_test_f32.wav"};
    const std::vector<std::vector<float>> channels(1, std::vector<float>(100, 0.25f));
    REQUIRE(ac3::io::write_wav_f32(file.path, channels, 48000).has_value());
    const auto read = ac3::io::read_wav_pcm(file.path);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == ac3::io::WavError::kUnsupportedFormat);
}

TEST_CASE("wav: write_wav_pcm rejects widths it does not speak", "[wav]") {
    TempWav file{"ac3forge_test_bad_bits.wav"};
    const std::vector<std::vector<std::int32_t>> channels(1, std::vector<std::int32_t>(10));
    const auto result = ac3::io::write_wav_pcm(file.path, channels, 48000, 20);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
}
