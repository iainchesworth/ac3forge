#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"

// ac3cli's 'atmos-adm' command (roadmap B1 phase 3 of 3 - see ROADMAP.md's "ADM BWF reader
// feeding the JOC encoder" entry; src/cli/main.cpp's run_atmos_adm). Real, subprocess-level
// integration tests: the same "run the actual built binary, inspect what it wrote" shape
// tests/test_cli.cpp's own atmos-encode test uses, and for the same reason - main.cpp compiles
// everything into one anonymous-namespace binary with no library surface run_atmos_adm's own
// logic could be linked into this test binary and called directly (see test_cli.cpp's own top
// comment).
//
// A separate file rather than folded into test_cli.cpp: this file's own tests only make sense
// when AC3FORGE_BUILD_ADM turned on ac3adm::ac3adm/ac3::admbridge AND ac3cli was actually built
// (so its own binary has the 'atmos-adm' command at all) - a narrower, two-part condition
// test_cli.cpp's single TARGET-ac3cli gate does not express. See tests/CMakeLists.txt's own
// gating comment for exactly how both conditions are checked before this file is even compiled.
//
// AC3CLI_EXE is supplied the same way as test_cli.cpp's own (see tests/CMakeLists.txt); run_cli
// below is a trimmed copy of test_cli.cpp's own helper of the same name (same reasoning for the
// double-quote wrapping on Windows - see that file's own comment on std::system() and cmd.exe's
// quoting), and the byte-level BW64/ADM fixture helpers are a copy of
// tests/test_adm_bridge.cpp's own flagship-test fixture (same bed L/R + SR-then-centre moving
// object, same known-good ring positions and hold/jump timing) - duplicated per this project's
// own established per-file test-helper convention (see that file's own comment on this) rather
// than shared, and deliberately kept byte-identical to that fixture rather than inventing a new
// one: this file's own job is checking that the real ac3cli binary wires
// parse_bw64 -> admbridge::build -> AtmosEncoder together correctly end to end, not re-proving
// admbridge's own BS.2076-2 §10.3 state machine or coordinate conversion, which
// test_adm_bridge.cpp already does directly against the library API.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_cli_adm_tests";
    fs::create_directories(dir);
    return dir;
}

// See tests/test_cli.cpp's own run_cli for the full reasoning behind the Windows
// double-quote-wrapping workaround this duplicates.
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

using Bytes = std::string;

void put_u16le(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32le(Bytes& out, std::uint32_t value) {
    put_u16le(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    put_u16le(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
}

void put_fourcc(Bytes& out, std::string_view cc) {
    REQUIRE(cc.size() == 4);
    out += cc;
}

void put_fixed(Bytes& out, std::string_view value, std::size_t width) {
    REQUIRE(value.size() == width);
    out += value;
}

void append_chunk(Bytes& out, std::string_view id, const Bytes& content) {
    put_fourcc(out, id);
    put_u32le(out, static_cast<std::uint32_t>(content.size()));
    out += content;
    if (content.size() % 2 != 0) {
        out.push_back('\0');
    }
}

constexpr int kFrame = ac3::kSamplesPerFrame;
constexpr int kTotalFrames = 6;  // 3 frames holding SR, 3 frames holding centre

Bytes build_fmt_chunk_3ch() {
    Bytes fmt;
    put_u16le(fmt, 1);      // WAVE_FORMAT_PCM
    put_u16le(fmt, 3);      // 3 tracks: bed-left, bed-right, moving object
    put_u32le(fmt, 48000);  // sample rate
    put_u32le(fmt, 48000 * 6);
    put_u16le(fmt, 6);   // block align (3 channels * 16 bits)
    put_u16le(fmt, 16);  // bits per sample
    return fmt;
}

// Three tracks: bed-left, bed-right, moving object - byte-identical shape to
// tests/test_adm_bridge.cpp's own build_chna_chunk_3.
Bytes build_chna_chunk_3() {
    Bytes chna;
    put_u16le(chna, 3);  // numTracks
    put_u16le(chna, 3);  // numUIDs
    struct Row {
        std::uint16_t track;
        std::string_view uid, track_ref, pack_ref;
    };
    const Row rows[] = {
        {1, "ATU_00000001", "AT_00019001_01", "AP_00019001"},
        {2, "ATU_00000002", "AT_00019002_01", "AP_00019001"},
        {3, "ATU_00000003", "AT_00039001_01", "AP_00039001"},
    };
    for (const auto& row : rows) {
        put_u16le(chna, row.track);
        put_fixed(chna, row.uid, 12);
        put_fixed(chna, row.track_ref, 14);
        put_fixed(chna, row.pack_ref, 11);
        chna.push_back('\0');
    }
    return chna;
}

// Six frames (9216 samples) of real, distinct, non-silent tones per channel - never
// silence/frame-0 (see this project's own standing lesson: those give false passes).
Bytes build_pcm16_3ch(int frames) {
    Bytes data;
    const double amplitude = 0.3 * 32767.0;
    for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / 48000.0;
        const double left = amplitude * std::sin(2.0 * std::numbers::pi * 300.0 * t);
        const double right = amplitude * std::sin(2.0 * std::numbers::pi * 500.0 * t);
        const double object = amplitude * std::sin(2.0 * std::numbers::pi * 800.0 * t);
        for (const double v : {left, right, object}) {
            put_u16le(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
        }
    }
    return data;
}

// Byte-identical to tests/test_adm_bridge.cpp's own kBridgeTestAdmXml: two DirectSpeakers bed
// channels pinned at the 5.1 ring's L (+30) and R (-30); one Objects channel that holds at SR
// (-110, this project's own kSR ring constant) for 3 frames (0.096s), then jumps (jumpPosition=1,
// no interpolationLength) to dead ahead (0 degrees / centre) and holds.
constexpr std::string_view kAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_9001" audioProgrammeName="CliBridgeTest">
    <audioContentIDRef>ACO_9001</audioContentIDRef>
    <audioContentIDRef>ACO_9002</audioContentIDRef>
  </audioProgramme>
  <audioContent audioContentID="ACO_9001" audioContentName="Bed">
    <audioObjectIDRef>AO_9001</audioObjectIDRef>
  </audioContent>
  <audioContent audioContentID="ACO_9002" audioContentName="Moving">
    <audioObjectIDRef>AO_9002</audioObjectIDRef>
  </audioContent>
  <audioObject audioObjectID="AO_9001" audioObjectName="Bed" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef>
    <audioTrackUIDRef>ATU_00000002</audioTrackUIDRef>
  </audioObject>
  <audioObject audioObjectID="AO_9002" audioObjectName="Moving" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000003</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00019001" audioPackFormatName="Bed" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioChannelFormatIDRef>AC_00019002</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioPackFormat audioPackFormatID="AP_00039001" audioPackFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="BedLeft" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+030</speakerLabel>
      <position coordinate="azimuth">30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019002" audioChannelFormatName="BedRight" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019002_00000001">
      <speakerLabel>M-030</speakerLabel>
      <position coordinate="azimuth">-30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00039001" audioChannelFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000001" rtime="00:00:00.00000" duration="00:00:00.09600">
      <position coordinate="azimuth">-110.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000002" rtime="00:00:00.09600">
      <position coordinate="azimuth">0.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00019001" audioStreamFormatName="PCM_BedLeft" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019001_01" audioTrackFormatName="PCM_BedLeft" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00019002" audioStreamFormatName="PCM_BedRight" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019002</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019002_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019002_01" audioTrackFormatName="PCM_BedRight" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019002</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000002" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019002_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00039001" audioStreamFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00039001_01" audioTrackFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00039001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000003" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
)";

bool write_fixture(const fs::path& path) {
    const auto fmt = build_fmt_chunk_3ch();
    const auto chna = build_chna_chunk_3();
    const Bytes axml(kAdmXml);
    const auto data = build_pcm16_3ch(kTotalFrames * kFrame);

    Bytes body;
    append_chunk(body, "fmt ", fmt);
    append_chunk(body, "chna", chna);
    append_chunk(body, "axml", axml);
    append_chunk(body, "data", data);

    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

double channel_energy(std::span<const float> samples) {
    double energy = 0.0;
    for (const auto v : samples) {
        const double sd = static_cast<double>(v);
        energy += sd * sd;
    }
    return energy;
}

}  // namespace

TEST_CASE("ac3cli atmos-adm parses, bridges and encodes a real ADM BWF master end to end",
         "[cli][atmos-adm]") {
    const auto dir = scratch_dir();
    const auto fixture_path = dir / "atmos_adm_fixture.wav";
    REQUIRE(write_fixture(fixture_path));

    const auto out_path = dir / "atmos_adm_out.ec3";
    const auto log_path = dir / "atmos_adm.log";
    const auto rc =
        run_cli("atmos-adm \"" + fixture_path.string() + "\" \"" + out_path.string() + "\" 448",
                log_path);
    INFO(read_log(log_path));
    CHECK(rc == 0);
    REQUIRE(fs::exists(out_path));
    CHECK(fs::file_size(out_path) > 0);

    // Decode what the CLI actually wrote - not a re-run through the library API - so this test
    // proves the real binary's argument parsing, ac3adm::parse_bw64 call, ac3::admbridge::build
    // call and per-frame AtmosEncoder loop are all wired together correctly, not just that each
    // piece works in isolation (test_adm_bridge.cpp's own flagship test already covers that).
    std::ifstream stream_in{out_path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{stream_in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> stream_bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        stream_bytes[i] = static_cast<std::byte>(raw[i]);
    }

    const auto units = ac3::split_access_units(stream_bytes);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == static_cast<std::size_t>(kTotalFrames));

    ac3::Eac3Decoder decoder;

    // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.
    constexpr int kCCh = 1;
    constexpr int kLCh = 0;
    constexpr int kRCh = 2;
    constexpr int kSRCh = 4;

    for (int f = 0; f < kTotalFrames; ++f) {
        const auto decoded = decoder.decode_access_unit((*units)[static_cast<std::size_t>(f)]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());

        // Check the last frame of each 3-frame hold, the same "settled, not mid-transition"
        // convention test_adm_bridge.cpp's own flagship test (and test_atmos_motion.cpp's before
        // it) use.
        if (f != 2 && f != 5) {
            continue;
        }
        const double energy_l = channel_energy((*decoded)->channels[kLCh]);
        const double energy_r = channel_energy((*decoded)->channels[kRCh]);
        const double energy_c = channel_energy((*decoded)->channels[kCCh]);
        const double energy_sr = channel_energy((*decoded)->channels[kSRCh]);
        CAPTURE(f, energy_l, energy_r, energy_c, energy_sr);

        // The bed is static throughout: both L and R carry real, comparable energy at every
        // checked frame, proving the bed pin survives independent of the object's own motion.
        CHECK(energy_l > 1.0);
        CHECK(energy_r > 1.0);

        if (f == 2) {
            // Block 0: held at azimuth -110 (SR) for [0, 0.096s) - frame 2 ends exactly at
            // 0.096s, still inside the hold.
            CHECK(energy_sr > 1.0);
            CHECK(energy_sr > energy_c);
        } else {
            // Block 1: jumped to azimuth 0 (dead ahead / C) at 0.096s, held afterward.
            CHECK(energy_c > 1.0);
            CHECK(energy_c > energy_sr);
        }
    }
}

TEST_CASE("ac3cli atmos-adm reports a clear diagnosis for a file with no ADM programme",
         "[cli][atmos-adm]") {
    const auto dir = scratch_dir();
    // Same container, empty <axml> chunk: parse_bw64 succeeds (a document with no ADM metadata
    // is valid per BS.2088-1 - see ac3adm::AdmDocument's own comment on this), but
    // admbridge::build then has no audioProgramme to resolve at all - BridgeError::kNoProgramme,
    // the error path this test exercises.
    const auto fixture_path = dir / "atmos_adm_no_programme.wav";
    {
        const auto fmt = build_fmt_chunk_3ch();
        const auto chna = build_chna_chunk_3();
        const auto data = build_pcm16_3ch(kFrame);
        Bytes body;
        append_chunk(body, "fmt ", fmt);
        append_chunk(body, "chna", chna);
        append_chunk(body, "data", data);
        Bytes file;
        put_fourcc(file, "RIFF");
        put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
        put_fourcc(file, "WAVE");
        file += body;
        std::ofstream out(fixture_path, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(file.data(), static_cast<std::streamsize>(file.size()));
    }

    const auto out_path = dir / "atmos_adm_no_programme_out.ec3";
    const auto log_path = dir / "atmos_adm_no_programme.log";
    const auto rc =
        run_cli("atmos-adm \"" + fixture_path.string() + "\" \"" + out_path.string() + "\"",
                log_path);
    CHECK(rc != 0);
    const auto log = read_log(log_path);
    // describe(BridgeError::kNoProgramme) - see src/adm_bridge/src/bridge.cpp - not an opaque
    // crash or a generic "error" with no diagnosis.
    CHECK(log.find("programme") != std::string::npos);
    CHECK_FALSE(fs::exists(out_path));
}

TEST_CASE("ac3cli atmos-adm reports a clear diagnosis for a file that is not a valid BW64/RIFF",
         "[cli][atmos-adm]") {
    const auto dir = scratch_dir();
    const auto bad_path = dir / "atmos_adm_not_riff.wav";
    {
        std::ofstream out(bad_path, std::ios::binary);
        REQUIRE(out.is_open());
        out << "not a RIFF file at all";
    }

    const auto out_path = dir / "atmos_adm_not_riff_out.ec3";
    const auto log_path = dir / "atmos_adm_not_riff.log";
    const auto rc =
        run_cli("atmos-adm \"" + bad_path.string() + "\" \"" + out_path.string() + "\"",
                log_path);
    CHECK(rc != 0);
    const auto log = read_log(log_path);
    // ac3adm::describe(AdmError::...) - never a silent crash or an unlabeled non-zero exit.
    CHECK(log.find("error:") != std::string::npos);
    CHECK_FALSE(fs::exists(out_path));
}
