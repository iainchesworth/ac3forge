#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "ac3/io/wav.hpp"

// WavStreamWriter exists so a live capture session (which can run an hour or
// more) never has to hold the whole take in memory - these tests exercise
// the two things that matter for that use case specifically: that the
// incremental writes read back identically to a one-shot write, and that
// flush_header()'s mid-stream seek-back-and-patch neither corrupts what
// follows nor leaves bytes stranded in a buffer the OS hasn't seen yet.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_wav_stream_tests";
    fs::create_directories(dir);
    return dir;
}

// 4 frames, 3 channels, deliberately non-trivial values so a transposed
// interleave or a byte-order slip would show up as a mismatch rather than
// coincidentally still comparing equal.
constexpr std::uint16_t kChannels = 3;
constexpr std::array<float, 12> kInterleaved = {
    0.25f,  -0.75f, 0.125f,  // frame 0
    -0.5f,  0.5f,   -0.25f,  // frame 1
    0.999f, -0.999f, 0.0f,   // frame 2
    0.333f, -0.666f, 0.111f, // frame 3
};

void check_round_trip(const fs::path& path) {
    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 44100);
    REQUIRE(data->channels.size() == kChannels);
    REQUIRE(data->frame_count() == 4);
    for (std::size_t frame = 0; frame < 4; ++frame) {
        for (std::uint16_t ch = 0; ch < kChannels; ++ch) {
            CHECK(data->channels[ch][frame] == kInterleaved[frame * kChannels + ch]);
        }
    }
}

}  // namespace

TEST_CASE("WavStreamWriter round-trips interleaved samples across several writes", "[wav]") {
    const auto path = scratch_dir() / "round_trip.wav";
    ac3::io::WavStreamWriter writer;
    REQUIRE(writer.open(path.string(), 44100, kChannels).has_value());
    CHECK(writer.is_open());
    CHECK(writer.channels() == kChannels);

    // Two writes, not one - the whole point is that a caller feeds this frame
    // by frame as capture delivers it, not as a single buffer.
    REQUIRE(writer.write(std::span{kInterleaved}.subspan(0, 6)));
    REQUIRE(writer.write(std::span{kInterleaved}.subspan(6, 6)));
    CHECK(writer.frames_written() == 4);
    writer.close();
    CHECK_FALSE(writer.is_open());

    check_round_trip(path);
}

TEST_CASE("WavStreamWriter flush_header mid-stream does not corrupt later writes", "[wav]") {
    const auto path = scratch_dir() / "flush_mid_stream.wav";
    ac3::io::WavStreamWriter writer;
    REQUIRE(writer.open(path.string(), 44100, kChannels).has_value());

    REQUIRE(writer.write(std::span{kInterleaved}.subspan(0, 6)));
    // Simulates the periodic re-patch a long-running session performs so a
    // hard kill doesn't leave a header claiming zero bytes of data.
    writer.flush_header();
    REQUIRE(writer.write(std::span{kInterleaved}.subspan(6, 6)));
    writer.close();

    // If the seek-back after flush_header() left the write cursor anywhere
    // but end-of-file, this would either overwrite frame 0/1's payload or
    // leave a gap - either way the round trip below would fail.
    check_round_trip(path);
}

TEST_CASE("WavStreamWriter flush_header puts bytes on disk before close()", "[wav]") {
    const auto path = scratch_dir() / "flush_before_close.wav";
    ac3::io::WavStreamWriter writer;
    REQUIRE(writer.open(path.string(), 44100, kChannels).has_value());
    REQUIRE(writer.write(std::span{kInterleaved}.subspan(0, 6)));
    writer.flush_header();

    // The property that actually matters for crash survival: once
    // flush_header() returns, the data already written is physically on
    // disk (visible to a second, independent handle on the same path), not
    // sitting in the fstream's own buffer waiting for something else to
    // flush it. close() is deliberately NOT called yet - that would make
    // this indistinguishable from testing close()'s own finalization.
    {
        std::ifstream raw{path, std::ios::binary};
        REQUIRE(raw.is_open());
        raw.seekg(0, std::ios::end);
        const auto size = raw.tellg();
        // 44-byte header + 2 frames * 3 channels * 4 bytes each.
        CHECK(static_cast<std::int64_t>(size) == 44 + 2 * 3 * 4);
    }

    writer.close();
    CHECK(writer.frames_written() == 2);
}

TEST_CASE("WavStreamWriter open() refuses a path it cannot create", "[wav]") {
    // Several nonexistent parent directories - fails to create on both
    // Windows and POSIX without assuming a specific unwritable system path.
    const auto path = scratch_dir() / "no" / "such" / "dir" / "stream.wav";
    ac3::io::WavStreamWriter writer;
    const auto result = writer.open(path.string(), 44100, kChannels);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kCannotOpen);
    CHECK_FALSE(writer.is_open());
}

TEST_CASE("WavStreamWriter open() refuses zero channels", "[wav]") {
    const auto path = scratch_dir() / "zero_channels.wav";
    ac3::io::WavStreamWriter writer;
    const auto result = writer.open(path.string(), 44100, 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
}
