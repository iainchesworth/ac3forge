// Opens an ADM BWF (BW64/RF64 + Audio Definition Model) file and prints
// what ac3adm::parse_bw64 found: programme/content/object/pack/channel/track
// counts, the <chna> join table, and the decoded PCM's own shape.
//
// This demonstrates roadmap item B1 phase 1's own API working end to end -
// it is NOT the "end-to-end example" phase 3 refers to, which will show a
// full ADM -> ac3::oba::AtmosEncoder -> E-AC-3 pipeline once phase 2 (the
// object/bed mapping layer) exists. ac3adm::ac3adm has no idea what AC-3,
// E-AC-3 or Atmos are, so this program does not either - it only proves the
// parsed graph is navigable. (It is also built on top of the vendored
// libbw64/libadm - see src/ac3adm/CMakeLists.txt - rather than a hand-rolled
// parser; only ac3adm's own types appear below, never either library's.)
//
// Every real-world ADM BWF master is a production audio file this project
// has no license to embed, so this program writes its own tiny-but-valid
// fixture to a temp file first (the same pattern examples/wav_roundtrip.cpp
// uses for the same reason) rather than shipping a canned one - the fixture
// is adapted from Recommendation ITU-R BS.2076-2 (10/2019) Annex 2 §2's own
// "Object-based example" ("Car" object), the standard's own worked example.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

// Writes a minimal, genuinely valid RIFF/WAVE ADM BWF file to `path`: one
// mono PCM16 track, a <chna> row joining it to the ADM graph, and an <axml>
// document describing one Objects-type audio object. See this file's own
// header comment for where the XML is adapted from.
bool write_fixture(const std::string& path) {
    Bytes fmt;
    put_u16le(fmt, 1);      // WAVE_FORMAT_PCM
    put_u16le(fmt, 1);      // mono
    put_u32le(fmt, 48000);  // sample rate
    put_u32le(fmt, 48000 * 2);
    put_u16le(fmt, 2);   // block align
    put_u16le(fmt, 16);  // bits per sample

    Bytes chna;
    put_u16le(chna, 1);  // numTracks
    put_u16le(chna, 1);  // numUIDs
    put_u16le(chna, 1);  // trackIndex
    chna += "ATU_00000001";
    chna += "AT_00031001_01";
    chna += "AP_00031001";
    chna.push_back('\0');  // pad byte, BS.2088-1 §8.2's audioID struct

    const Bytes axml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="CarsSounds">
    <audioContentIDRef>ACO_1001</audioContentIDRef>
  </audioProgramme>
  <audioContent audioContentID="ACO_1001" audioContentName="Cars">
    <audioObjectIDRef>AO_1001</audioObjectIDRef>
  </audioContent>
  <audioObject audioObjectID="AO_1001" audioObjectName="Car" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00031001" audioPackFormatName="Car" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="Car1" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00031001_00000001">
      <position coordinate="azimuth">-22.5</position>
      <position coordinate="elevation">5.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00031001" audioStreamFormatName="PCM_Car1" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00031001_01" audioTrackFormatName="PCM_Car1" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
)";

    Bytes data;
    for (int frame = 0; frame < 8; ++frame) {
        put_u16le(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(frame * 2000 - 7000)));
    }

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

int main() {
    const auto fixture_path = (std::filesystem::temp_directory_path() / "ac3forge_adm_fixture.wav").string();
    if (!write_fixture(fixture_path)) {
        std::printf("could not write fixture file\n");
        return 1;
    }

    const auto document = ac3adm::parse_bw64(fixture_path);
    if (!document) {
        std::printf("parse_bw64 failed: %.*s\n", static_cast<int>(ac3adm::describe(document.error()).size()),
                    ac3adm::describe(document.error()).data());
        return 1;
    }

    std::printf("PCM: %u Hz, %u-bit, %zu channel(s), %zu frame(s)\n", document->audio.sample_rate,
                document->audio.bits_per_sample, document->audio.channels.size(), document->audio.frame_count());
    std::printf("chna rows: %zu\n", document->chna.size());
    std::printf("ADM graph: %zu programme(s), %zu content(s), %zu object(s), %zu pack format(s), "
                "%zu channel format(s), %zu stream format(s), %zu track format(s), %zu track UID(s)\n",
                document->model.programmes.size(), document->model.contents.size(), document->model.objects.size(),
                document->model.pack_formats.size(), document->model.channel_formats.size(),
                document->model.stream_formats.size(), document->model.track_formats.size(),
                document->model.track_uids.size());

    for (const auto& programme : document->model.programmes) {
        std::printf("  programme %s (%s) -> %zu content(s)\n", programme.id.c_str(), programme.name.c_str(),
                    programme.content_refs.size());
    }
    for (const auto& object : document->model.objects) {
        std::printf("  object %s (%s), start=%.5fs, %zu track UID ref(s)\n", object.id.c_str(), object.name.c_str(),
                    object.start_s, object.track_uid_refs.size());
    }

    std::filesystem::remove(fixture_path);
    return 0;
}
