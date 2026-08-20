#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ac3/io/wav.hpp"

// ac3::io::read_wav/parse_wav (src/forge/src/io/wav.cpp) is the file every
// codec-path test in this suite leans on to get real audio in and decoded
// audio back out - but nothing exercises the parser itself: its RIFF/WAVE
// validation, its PCM16 decode path (every other test only round-trips
// float32, which write_wav_f32 always produces bit-exact), its
// WAVE_FORMAT_EXTENSIBLE handling, or what happens when a data chunk's
// declared size overruns the bytes actually available. These tests build
// the raw RIFF bytes by hand so the parser's own logic - not another writer
// that happens to agree with it - is what is under test.

using Catch::Approx;

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_wav_reader_tests";
    fs::create_directories(dir);
    return dir;
}

void put_le16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put_le32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// A canonical (non-extensible) fmt chunk + data chunk, with `declared_data_
// bytes` written into the data chunk's own size field - independent of how
// many bytes of `payload` are actually appended, so callers can build a file
// whose header lies about its own length.
std::string canonical_wav(std::uint16_t format_tag, std::uint16_t channels,
                          std::uint32_t sample_rate, std::uint16_t bits,
                          const std::string& payload, std::uint32_t declared_data_bytes) {
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * (bits / 8));
    const std::uint32_t byte_rate = sample_rate * block_align;

    std::string fmt;
    put_le16(fmt, format_tag);
    put_le16(fmt, channels);
    put_le32(fmt, sample_rate);
    put_le32(fmt, byte_rate);
    put_le16(fmt, block_align);
    put_le16(fmt, bits);

    std::string out;
    out += "RIFF";
    put_le32(out, static_cast<std::uint32_t>(4 + (8 + fmt.size()) + (8 + payload.size())));
    out += "WAVE";
    out += "fmt ";
    put_le32(out, static_cast<std::uint32_t>(fmt.size()));
    out += fmt;
    out += "data";
    put_le32(out, declared_data_bytes);
    out += payload;
    return out;
}

std::string canonical_wav(std::uint16_t format_tag, std::uint16_t channels,
                          std::uint32_t sample_rate, std::uint16_t bits,
                          const std::string& payload) {
    return canonical_wav(format_tag, channels, sample_rate, bits, payload,
                         static_cast<std::uint32_t>(payload.size()));
}

// WAVE_FORMAT_EXTENSIBLE (tag 0xFFFE): a 40-byte fmt chunk whose real format
// is the leading two bytes of the 16-byte SubFormat GUID, per parse_wav's own
// fmt_at + 32 read.
std::string extensible_wav(std::uint16_t real_format_tag, std::uint16_t channels,
                           std::uint32_t sample_rate, std::uint16_t bits,
                           const std::string& payload) {
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * (bits / 8));
    const std::uint32_t byte_rate = sample_rate * block_align;

    std::string fmt;
    put_le16(fmt, 0xFFFE);
    put_le16(fmt, channels);
    put_le32(fmt, sample_rate);
    put_le32(fmt, byte_rate);
    put_le16(fmt, block_align);
    put_le16(fmt, bits);
    put_le16(fmt, 22);    // cbSize: 22 bytes follow (valid bits + mask + GUID)
    put_le16(fmt, bits);  // wValidBitsPerSample
    put_le32(fmt, 0);     // dwChannelMask: not indicated
    put_le16(fmt, real_format_tag);
    // The rest of the 16-byte SubFormat GUID (KSDATAFORMAT_SUBTYPE_PCM/IEEE_
    // FLOAT's fixed tail) - parse_wav never reads past the leading 2 bytes,
    // so its exact value does not matter here.
    for (int i = 0; i < 14; ++i) {
        fmt.push_back(static_cast<char>(0));
    }
    REQUIRE(fmt.size() == 40);

    std::string out;
    out += "RIFF";
    put_le32(out, static_cast<std::uint32_t>(4 + (8 + fmt.size()) + (8 + payload.size())));
    out += "WAVE";
    out += "fmt ";
    put_le32(out, static_cast<std::uint32_t>(fmt.size()));
    out += fmt;
    out += "data";
    put_le32(out, static_cast<std::uint32_t>(payload.size()));
    out += payload;
    return out;
}

fs::path write_raw(const std::string& name, const std::string& bytes) {
    const auto path = scratch_dir() / name;
    std::ofstream out{path, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

}  // namespace

TEST_CASE("read_wav round-trips write_wav_f32 exactly, multi-channel", "[wav]") {
    const std::vector<std::vector<float>> channels = {
        {0.25f, -0.75f, 0.5f, -1.0f}, {0.1f, -0.2f, 0.3f, -0.4f}, {0.0f, 1.0f, -1.0f, 0.0f}};
    const auto path = scratch_dir() / "float_round_trip.wav";
    REQUIRE(ac3::io::write_wav_f32(path.string(), channels, 48000).has_value());

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 48000);
    REQUIRE(data->channels.size() == 3);
    REQUIRE(data->frame_count() == 4);
    for (std::size_t ch = 0; ch < 3; ++ch) {
        for (std::size_t n = 0; n < 4; ++n) {
            CHECK(data->channels[ch][n] == channels[ch][n]);
        }
    }
}

TEST_CASE("read_wav decodes PCM16 samples with the documented int16/32768 scaling", "[wav]") {
    // Interleaved stereo, hand-picked to cover both signed extremes and the
    // exact halves a wrong shift or a signedness slip would visibly miss.
    // std::int16_t's two's-complement range is asymmetric (-32768..32767),
    // so kMinInt16's exact float quotient is -1.0f while kMaxInt16's is not:
    // that is precisely why the two are checked against different tolerances
    // below, not a quirk of this test.
    std::string payload;
    const std::vector<std::int16_t> samples = {0, 0, 16384, -16384, 32767, -32768};
    for (const auto s : samples) {
        put_le16(payload, static_cast<std::uint16_t>(s));
    }
    const auto path = write_raw("pcm16.wav", canonical_wav(1, 2, 44100, 16, payload));

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 44100);
    REQUIRE(data->channels.size() == 2);
    REQUIRE(data->frame_count() == 3);
    // channel 0: frames 0,1,2 = samples[0], samples[2], samples[4]
    CHECK(data->channels[0][0] == 0.0f);
    CHECK(data->channels[0][1] == Approx(0.5).margin(1e-6));
    CHECK(data->channels[0][2] == Approx(32767.0 / 32768.0).margin(1e-6));
    // channel 1: frames 0,1,2 = samples[1], samples[3], samples[5]
    CHECK(data->channels[1][0] == 0.0f);
    CHECK(data->channels[1][1] == Approx(-0.5).margin(1e-6));
    CHECK(data->channels[1][2] == -1.0f);  // -32768 / 32768 is exact
}

TEST_CASE("read_wav via write_wav_pcm16_raw's own payload decodes back losslessly", "[wav]") {
    // write_wav_pcm16_raw is used for the IEC 61937 burst carrier, where the
    // payload must pass through untouched - this closes the loop by reading
    // it back through the same parser every other WAV path uses.
    std::string payload;
    for (const std::int16_t s : {std::int16_t{1000}, std::int16_t{-1000}, std::int16_t{4},
                                 std::int16_t{-4}}) {
        put_le16(payload, static_cast<std::uint16_t>(s));
    }
    const std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(payload.data()),
                                       reinterpret_cast<const std::byte*>(payload.data()) +
                                           payload.size());
    const auto path = scratch_dir() / "pcm16_raw.wav";
    REQUIRE(ac3::io::write_wav_pcm16_raw(path.string(), bytes, 48000, 2).has_value());

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->channels.size() == 2);
    REQUIRE(data->frame_count() == 2);
    CHECK(data->channels[0][0] == Approx(1000.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[1][0] == Approx(-1000.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[0][1] == Approx(4.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[1][1] == Approx(-4.0 / 32768.0).margin(1e-9));
}

TEST_CASE("read_wav resolves WAVE_FORMAT_EXTENSIBLE via the SubFormat GUID's leading tag",
         "[wav]") {
    SECTION("float32 subformat") {
        std::string payload;
        const float value = -0.5f;
        payload.append(reinterpret_cast<const char*>(&value), sizeof(value));
        const auto path =
            write_raw("extensible_float.wav", extensible_wav(3, 1, 48000, 32, payload));
        const auto data = ac3::io::read_wav(path.string());
        REQUIRE(data.has_value());
        REQUIRE(data->frame_count() == 1);
        CHECK(data->channels[0][0] == -0.5f);
    }
    SECTION("pcm16 subformat") {
        std::string payload;
        put_le16(payload, static_cast<std::uint16_t>(-16384));
        const auto path =
            write_raw("extensible_pcm16.wav", extensible_wav(1, 1, 48000, 16, payload));
        const auto data = ac3::io::read_wav(path.string());
        REQUIRE(data.has_value());
        REQUIRE(data->frame_count() == 1);
        CHECK(data->channels[0][0] == Approx(-0.5).margin(1e-6));
    }
}

TEST_CASE("read_wav clamps to the bytes actually present when the data chunk overstates its size",
         "[wav]") {
    // The header's data-chunk size field claims 1000 bytes; the file only
    // actually holds 8 (two stereo PCM16 frames). parse_wav is documented to
    // clamp `declared` to what is actually available rather than fail - this
    // pins that specific, easy-to-break-by-accident behaviour rather than
    // assuming it.
    std::string payload;
    for (const std::int16_t s : {std::int16_t{100}, std::int16_t{-100}, std::int16_t{200},
                                 std::int16_t{-200}}) {
        put_le16(payload, static_cast<std::uint16_t>(s));
    }
    const auto path =
        write_raw("overstated_size.wav", canonical_wav(1, 2, 48000, 16, payload, 1000));

    const auto data = ac3::io::read_wav(path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->channels.size() == 2);
    CHECK(data->frame_count() == 2);  // 8 bytes / (2 ch * 2 bytes) = 2 frames, not the declared 1000
    CHECK(data->channels[0][0] == Approx(100.0 / 32768.0).margin(1e-9));
    CHECK(data->channels[1][1] == Approx(-200.0 / 32768.0).margin(1e-9));
}

TEST_CASE("read_wav rejects a path that cannot be opened", "[wav]") {
    const auto result = ac3::io::read_wav((scratch_dir() / "does_not_exist.wav").string());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kCannotOpen);
}

TEST_CASE("read_wav rejects data that is not a RIFF/WAVE file", "[wav]") {
    SECTION("too short to even hold a header") {
        // "RIFF", four size bytes, then "WAV" - 11 bytes total, well under
        // the 44-byte minimum a real header needs.
        std::string bytes = "RIFF";
        bytes.resize(8, '\0');
        bytes += "WAV";
        const auto path = write_raw("too_short.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("empty file") {
        std::istringstream in;
        const auto result = ac3::io::read_wav(in);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("wrong magic entirely") {
        std::string bytes(64, '\0');
        bytes.replace(0, 4, "JUNK");
        const auto path = write_raw("wrong_magic.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
    SECTION("RIFF/WAVE magic present but neither fmt nor data chunk exists") {
        std::string bytes = "RIFF";
        put_le32(bytes, 40);
        bytes += "WAVE";
        bytes.append(40, '\0');  // padding, deliberately not "fmt " or "data"
        const auto path = write_raw("no_chunks.wav", bytes);
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kNotRiffWave);
    }
}

TEST_CASE("read_wav rejects sample formats it does not support", "[wav]") {
    SECTION("8-bit PCM") {
        const auto path = write_raw("pcm8.wav", canonical_wav(1, 1, 48000, 8, std::string(4, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("24-bit PCM") {
        const auto path =
            write_raw("pcm24.wav", canonical_wav(1, 1, 48000, 24, std::string(6, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("64-bit IEEE float") {
        const auto path =
            write_raw("float64.wav", canonical_wav(3, 1, 48000, 64, std::string(8, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("a non-PCM, non-float format tag (e.g. ADPCM)") {
        const auto path =
            write_raw("adpcm.wav", canonical_wav(2, 1, 48000, 4, std::string(4, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
    SECTION("zero channels") {
        const auto path =
            write_raw("zero_channels.wav", canonical_wav(1, 0, 48000, 16, std::string(4, '\0')));
        const auto result = ac3::io::read_wav(path.string());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::WavError::kUnsupportedFormat);
    }
}

TEST_CASE("read_wav(std::istream&) parses the same bytes the path overload does", "[wav]") {
    const std::string bytes = canonical_wav(1, 1, 22050, 16, [] {
        std::string payload;
        put_le16(payload, static_cast<std::uint16_t>(1234));
        return payload;
    }());
    std::istringstream in{bytes, std::ios::binary};
    const auto data = ac3::io::read_wav(in);
    REQUIRE(data.has_value());
    CHECK(data->sample_rate == 22050);
    REQUIRE(data->frame_count() == 1);
    CHECK(data->channels[0][0] == Approx(1234.0 / 32768.0).margin(1e-9));
}

TEST_CASE("write_wav_f32 refuses empty channel data and never touches the filesystem", "[wav]") {
    const auto path = scratch_dir() / "must_not_exist.wav";
    fs::remove(path);
    const std::vector<std::vector<float>> empty;
    const auto result = ac3::io::write_wav_f32(path.string(), empty, 48000);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::WavError::kTruncated);
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("describe() gives every WavError a distinct, non-empty message", "[wav]") {
    const std::array errors = {ac3::io::WavError::kCannotOpen, ac3::io::WavError::kNotRiffWave,
                               ac3::io::WavError::kUnsupportedFormat,
                               ac3::io::WavError::kTruncated};
    for (const auto e : errors) {
        CAPTURE(static_cast<int>(e));
        CHECK_FALSE(ac3::io::describe(e).empty());
    }
    for (std::size_t i = 0; i < errors.size(); ++i) {
        for (std::size_t j = i + 1; j < errors.size(); ++j) {
            CHECK(ac3::io::describe(errors[i]) != ac3::io::describe(errors[j]));
        }
    }
}
