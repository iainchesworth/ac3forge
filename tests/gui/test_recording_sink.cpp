#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "matroska/matroska.hpp"
#include "mpegts/mpegts.hpp"
#include "recording_sink.hpp"

// RecordingSink exists so a GUI take's encoded frames leave for disk as they
// are produced instead of accumulating until Stop. These tests hold it to
// the standard the streamed CLI paths were held to: for every container the
// sink streams, the file it leaves behind must be what the corresponding
// one-shot writer would have produced for the same frames - byte for byte
// where the format permits (elementary, MPEG-TS via mpegts::Writer's own
// mux-equality contract, the IEC 61937 WAV carrier), and equal to the
// incremental writer's own composed output for Matroska (whose streamed form
// differs from mux() by design - the unknown-size Segment).

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_recording_sink_tests";
    fs::create_directories(dir);
    return dir;
}

std::vector<std::byte> read_file_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    in.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(in.good());
    return bytes;
}

// Real frames, not synthetic bytes: the IEC 61937 path parses each frame's
// own header (sync word, fscod), so only genuine bitstream will do - and
// what is genuine enough for the strictest container serves the rest too.
std::vector<std::vector<std::byte>> silent_ac3_frames(std::size_t count) {
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < count; ++i) {
        auto frame = ac3::build_silent_stereo_frame({});
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }
    return frames;
}

std::vector<std::vector<std::byte>> silent_eac3_units(std::size_t count) {
    const auto unit = ac3::eac3::build_silent_access_unit(
        {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}});
    REQUIRE(unit.has_value());
    return {count, unit->bytes};
}

std::vector<std::span<const std::byte>> as_views(
    const std::vector<std::vector<std::byte>>& frames) {
    return {frames.begin(), frames.end()};
}

std::vector<std::byte> pushed_through(RecordingSink::Container container, bool eac3,
                                      const fs::path& path,
                                      const std::vector<std::vector<std::byte>>& frames) {
    RecordingSink sink;
    REQUIRE(sink.open(path.string(),
                      {.container = container, .eac3 = eac3, .sample_rate = 48000, .channels = 2})
                .empty());
    for (const auto& frame : frames) {
        REQUIRE(sink.push(frame).empty());
    }
    REQUIRE(sink.close().empty());
    CHECK(sink.frames() == frames.size());
    return read_file_bytes(path);
}

}  // namespace

TEST_CASE("RecordingSink's elementary stream is the frames, concatenated", "[gui]") {
    const auto frames = silent_ac3_frames(5);
    const auto file = pushed_through(RecordingSink::Container::kElementary, false,
                                     scratch_dir() / "take.ac3", frames);
    std::vector<std::byte> expected;
    for (const auto& frame : frames) {
        expected.insert(expected.end(), frame.begin(), frame.end());
    }
    CHECK(file == expected);
}

TEST_CASE("RecordingSink's MPEG-TS take is byte-identical to mpegts::mux", "[gui]") {
    const auto frames = silent_ac3_frames(7);
    const auto file = pushed_through(RecordingSink::Container::kMpegts, false,
                                     scratch_dir() / "take.ts", frames);
    const auto one_shot = mpegts::mux(
        mpegts::AudioTrack{.codec = mpegts::AudioCodec::kAc3,
                           .sample_rate = 48000,
                           .channels = 2,
                           .samples_per_frame = ac3::kSamplesPerFrame},
        as_views(frames));
    REQUIRE(one_shot.has_value());
    CHECK(file == *one_shot);
}

TEST_CASE("RecordingSink's IEC 61937 take is byte-identical to the one-shot carrier",
          "[gui]") {
    const bool eac3 = GENERATE(false, true);
    const auto frames = eac3 ? silent_eac3_units(6) : silent_ac3_frames(6);
    const auto file =
        pushed_through(RecordingSink::Container::kSpdif, eac3,
                       scratch_dir() / (eac3 ? "take_eac3.wav" : "take_ac3.wav"), frames);

    const auto payload = ac3::iec61937::wrap_stream(as_views(frames), eac3);
    REQUIRE(payload.has_value());
    const auto one_shot = scratch_dir() / "carrier_one_shot.wav";
    REQUIRE(ac3::io::write_wav_pcm16_raw(one_shot.string(), *payload,
                                         eac3 ? 48000U * 4 : 48000U, 2)
                .has_value());
    CHECK(file == read_file_bytes(one_shot));
}

TEST_CASE("RecordingSink's Matroska take matches matroska::Writer's own composition",
          "[gui]") {
    const auto frames = silent_ac3_frames(40);
    const auto file = pushed_through(RecordingSink::Container::kMatroska, false,
                                     scratch_dir() / "take.mkv", frames);

    auto writer = matroska::Writer::create(
        matroska::AudioTrack{.codec_id = std::string{matroska::kCodecAc3},
                             .sample_rate = 48000,
                             .channels = 2,
                             .samples_per_frame = ac3::kSamplesPerFrame});
    REQUIRE(writer.has_value());
    std::vector<std::byte> expected = writer->header();
    for (const auto& frame : frames) {
        const auto closed = writer->push(frame);
        REQUIRE(closed.has_value());
        expected.insert(expected.end(), closed->begin(), closed->end());
    }
    const auto tail = writer->finalize();
    expected.insert(expected.end(), tail.begin(), tail.end());
    CHECK(file == expected);
}

TEST_CASE("RecordingSink with zero frames removes the file and says so", "[gui]") {
    const auto path = scratch_dir() / "empty_take.ac3";
    RecordingSink sink;
    REQUIRE(sink.open(path.string(), {.container = RecordingSink::Container::kElementary,
                                      .eac3 = false,
                                      .sample_rate = 48000,
                                      .channels = 2})
                .empty());
    CHECK(sink.close() == "Nothing was encoded.");
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("RecordingSink reports an uncreatable destination at open, not at stop", "[gui]") {
    RecordingSink sink;
    const auto problem =
        sink.open((scratch_dir() / "no" / "such" / "dir" / "take.ac3").string(),
                  {.container = RecordingSink::Container::kElementary,
                   .eac3 = false,
                   .sample_rate = 48000,
                   .channels = 2});
    CHECK(problem == "Could not open the output file for writing.");
}
