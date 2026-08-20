// A real ADM BWF master, all the way to a Dolby Atmos E-AC-3 (DD+ JOC) elementary stream.
//
// Roadmap item B1 phase 3 of 3 (the last piece - phase 1 is ac3adm::ac3adm, src/ac3adm; phase 2 is
// ac3::admbridge, src/admbridge). This is a minimal, standalone illustration of the same pipeline
// ac3cli's 'atmos-adm' command drives for real: ac3adm::parse_bw64() reads the container + ADM XML
// graph, ac3::admbridge::build() maps it onto ac3::oba::AtmosEncoder's flat object-list input
// shape (one bed speaker feed pinned in place, one dynamic object panned by its own authored
// motion), and a plain per-frame loop calls ac3::oba::evaluate_placements() plus
// AtmosEncoder::encode_frame() the same way every other Atmos example in this directory does. The
// CLI command and this example deliberately share nothing but that library API - see
// docs/library/adm-bridge.md's own note on why no separate "driving loop" abstraction exists.
//
// Like examples/read_adm.cpp, this writes its own tiny-but-valid BW64/ADM fixture to a temp file
// first, rather than shipping a real production master this project has no license to embed: one
// DirectSpeakers bed channel pinned at the front-centre speaker, and one Objects channel that
// holds hard right (azimuth -90 - BS.2076-2 Clause 8: positive azimuth is left, so negative is
// right) for half the clip and then jumps hard left (azimuth +90) for the rest (§10.3's
// jumpPosition=1 state machine - see ac3::admbridge::build_channel_path's own comment for the full
// walkthrough), so the encoded stream's own channel balance visibly tracks the authored ADM
// automation rather than staying static throughout.
//
// Run with `--write-fixture <path>` to just write that same fixture to a real file and exit,
// skipping the parse/bridge/encode demo below - see main()'s own comment on why
// tools/ci/run_codec_matrix.sh uses exactly this to drive a real `ac3cli atmos-adm` invocation.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/admbridge/bridge.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3adm/ac3adm.hpp"

namespace {

using Bytes = std::string;

void put_u16le(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32le(Bytes& out, std::uint32_t value) {
    put_u16le(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    put_u16le(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
}

void append_chunk(Bytes& out, std::string_view id, const Bytes& content) {
    out += id;
    put_u32le(out, static_cast<std::uint32_t>(content.size()));
    out += content;
    if (content.size() % 2 != 0) {
        out.push_back('\0');
    }
}

constexpr int kFrame = ac3::kSamplesPerFrame;
constexpr int kHoldFrames = 3;                    // frames per half of the clip
constexpr int kTotalFrames = 2 * kHoldFrames;      // 6 frames total (~0.192s @ 48kHz)

// Two tracks: a Bed channel (bed centre speaker) and a Moving object, one <chna> row each.
Bytes build_chna() {
    Bytes chna;
    put_u16le(chna, 2);  // numTracks
    put_u16le(chna, 2);  // numUIDs
    struct Row {
        std::uint16_t track;
        std::string_view uid, track_ref, pack_ref;
    };
    const Row rows[] = {
        {1, "ATU_00000001", "AT_00019001_01", "AP_00019001"},
        {2, "ATU_00000002", "AT_00039001_01", "AP_00039001"},
    };
    for (const auto& row : rows) {
        put_u16le(chna, row.track);
        chna += row.uid;
        chna += row.track_ref;
        chna += row.pack_ref;
        chna.push_back('\0');  // pad byte, BS.2088-1 §8.2's audioID struct
    }
    return chna;
}

constexpr std::string_view kAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_9001" audioProgrammeName="Example">
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
  </audioObject>
  <audioObject audioObjectID="AO_9002" audioObjectName="Moving" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000002</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00019001" audioPackFormatName="Bed" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioPackFormat audioPackFormatID="AP_00039001" audioPackFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="BedCentre" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+000</speakerLabel>
      <position coordinate="azimuth">0.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00039001" audioChannelFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000001" rtime="00:00:00.00000" duration="00:00:00.09600">
      <position coordinate="azimuth">-90.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000002" rtime="00:00:00.09600">
      <position coordinate="azimuth">90.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00019001" audioStreamFormatName="PCM_Bed" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019001_01" audioTrackFormatName="PCM_Bed" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00039001" audioStreamFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00039001_01" audioTrackFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00039001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000002" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
)";

// Two real, distinct, non-silent tones (never silence/frame-0 - a silent or single-frame fixture
// would "pass" this pipeline even if the bridge or the encoder were badly broken): the bed at
// 300 Hz, the moving object at 800 Hz.
Bytes build_pcm16() {
    Bytes data;
    const double amplitude = 0.3 * 32767.0;
    for (int frame = 0; frame < kTotalFrames * kFrame; ++frame) {
        const double t = static_cast<double>(frame) / 48000.0;
        const double bed = amplitude * std::sin(2.0 * std::numbers::pi * 300.0 * t);
        const double object = amplitude * std::sin(2.0 * std::numbers::pi * 800.0 * t);
        for (const double v : {bed, object}) {
            put_u16le(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
        }
    }
    return data;
}

bool write_fixture(const std::string& path) {
    Bytes fmt;
    put_u16le(fmt, 1);      // WAVE_FORMAT_PCM
    put_u16le(fmt, 2);      // 2 tracks: bed, moving object
    put_u32le(fmt, 48000);  // sample rate
    put_u32le(fmt, 48000 * 4);
    put_u16le(fmt, 4);   // block align
    put_u16le(fmt, 16);  // bits per sample

    const auto chna = build_chna();
    const Bytes axml(kAdmXml);
    const auto data = build_pcm16();

    Bytes body;
    append_chunk(body, "fmt ", fmt);
    append_chunk(body, "chna", chna);
    append_chunk(body, "axml", axml);
    append_chunk(body, "data", data);

    Bytes file;
    file += "RIFF";
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    file += "WAVE";
    file += body;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    // --write-fixture <path>: writes only the fixture below to `path` and exits, skipping the
    // parse/bridge/encode demo that follows. Exists so tools/ci/run_codec_matrix.sh (a bash
    // script with no access to this file's own C++ helpers) can reuse this exact fixture to drive
    // a real `ac3cli atmos-adm` invocation against a real file on disk, rather than a fourth copy
    // of the same byte-level BW64/ADM chunk-writing logic already duplicated (per this project's
    // own established per-file test-fixture convention) across this file, examples/read_adm.cpp
    // and tests/cli/test_cli_atmos_adm.cpp - three was already the considered limit; a shell script
    // reimplementing RIFF chunk framing in bash was not a fourth worth having.
    if (argc >= 3 && std::string_view{argv[1]} == "--write-fixture") {
        if (!write_fixture(argv[2])) {
            std::printf("could not write fixture file\n");
            return 1;
        }
        return 0;
    }

    const auto fixture_path =
        (std::filesystem::temp_directory_path() / "ac3forge_encode_adm_fixture.wav").string();
    if (!write_fixture(fixture_path)) {
        std::printf("could not write fixture file\n");
        return 1;
    }

    // Step 1: ac3adm::ac3adm (phase 1) - container + ADM graph.
    const auto document = ac3adm::parse_bw64(fixture_path);
    if (!document) {
        std::printf("parse_bw64 failed: %.*s\n",
                    static_cast<int>(ac3adm::describe(document.error()).size()),
                    ac3adm::describe(document.error()).data());
        std::filesystem::remove(fixture_path);
        return 1;
    }

    // Step 2: ac3::admbridge (phase 2) - bed/object classification, coordinate conversion, and
    // §10.3 position/gain automation, mapped onto AtmosEncoder's flat object-list input shape.
    const auto bridged = ac3::admbridge::build(*document);
    std::filesystem::remove(fixture_path);
    if (!bridged) {
        std::printf("admbridge::build failed: %.*s\n",
                    static_cast<int>(ac3::admbridge::describe(bridged.error()).size()),
                    ac3::admbridge::describe(bridged.error()).data());
        return 1;
    }

    std::printf("bridged %zu channel(s) from %s\n", bridged->channel_count(),
                document->model.programmes.front().name.c_str());
    for (std::size_t i = 0; i < bridged->channel_count(); ++i) {
        std::printf("  %s: %s\n", bridged->channel_ids[i].c_str(),
                    bridged->is_bed[i] ? "bed speaker feed" : "dynamic object");
    }

    // Step 3: drive AtmosEncoder::encode_frame() in a loop - ac3::oba::evaluate_placements()
    // reads each channel's ac3::oba::ObjectPath at the frame's own end time, exactly the pattern
    // ac3cli's own atmos-path/atmos-encode/atmos-adm commands and every other Atmos example in
    // this directory use.
    const auto objects = static_cast<int>(bridged->channel_count());
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, objects};

    const auto total_samples = bridged->pcm.empty() ? std::size_t{0} : bridged->pcm.front().size();
    const auto total_frames = total_samples / static_cast<std::size_t>(kFrame);
    std::vector<std::span<const float>> views(bridged->channel_count());
    std::vector<std::byte> stream;

    for (std::size_t f = 0; f < total_frames; ++f) {
        const auto start = f * static_cast<std::size_t>(kFrame);
        for (std::size_t ch = 0; ch < bridged->channel_count(); ++ch) {
            views[ch] = bridged->pcm[ch].subspan(start, static_cast<std::size_t>(kFrame));
        }
        const double t = static_cast<double>(start + static_cast<std::size_t>(kFrame)) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(bridged->paths, t);

        // Step 4: write - the raw elementary E-AC-3 stream, same convention every Atmos-encode
        // path in this project uses (container wrapping, if wanted, is a separate later step via
        // ac3cli's own mkv/mp4/fmp4/ts commands).
        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("encode_frame failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto out_path =
        (std::filesystem::temp_directory_path() / "ac3forge_encode_adm_out.ec3").string();
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(stream.data()), static_cast<std::streamsize>(stream.size()));
    const bool wrote = static_cast<bool>(out);
    out.close();
    std::filesystem::remove(out_path);
    if (!wrote) {
        std::printf("could not write output stream\n");
        return 1;
    }

    std::size_t bed_count = 0;
    for (const bool is_bed : bridged->is_bed) {
        bed_count += is_bed ? 1 : 0;
    }
    std::printf("%zu bytes of DD+ JOC E-AC-3 from %zu ADM-authored frame(s): %zu bed speaker "
                "feed(s) + %zu dynamic object(s)\n",
                stream.size(), total_frames, bed_count, bridged->channel_count() - bed_count);
    return 0;
}
