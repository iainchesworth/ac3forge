#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ac3adm/ac3adm.hpp"

// These tests build BW64/RF64 fixtures byte-by-byte, independently of
// src/ac3adm's own implementation (which is itself just a thin translation
// layer over the vendored libbw64/libadm - see src/ac3adm/CMakeLists.txt),
// rather than round-tripping data this same code produced - the same
// reasoning test_mpegts.cpp and test_matroska.cpp document for their own
// independent readers/writers, applied in the opposite direction here since
// ac3adm::ac3adm is a reader with no writer counterpart in this repo (phase 1
// of roadmap item B1, see ROADMAP.md - a writer is not part of this
// project's own scope at all, ADM/BW64 masters are produced by third-party
// production tools).
//
// Every test below goes through the public ac3adm::parse_bw64() API only -
// libadm and libbw64 already have their own upstream test suites for their
// own internals (chunk-walking, XML/schema validation), so what is worth
// re-testing here is this module's own boundary: does a real BW64 file
// carrying a given ADM shape come out as the right ac3adm::AdmModel/
// AdmDocument, and does a malformed one come back as the right AdmError.
//
// The embedded ADM XML for the "Car" fixture is adapted from Recommendation
// ITU-R BS.2076-2 (10/2019) Annex 2 §2's own "Object-based example" (the
// "Car" object), trimmed to one audioBlockFormat instead of three for
// brevity - it is the standard's own worked example, not invented data.

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

void put_u64le(Bytes& out, std::uint64_t value) {
    put_u32le(out, static_cast<std::uint32_t>(value & 0xFFFF'FFFFu));
    put_u32le(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFF'FFFFu));
}

void put_fourcc(Bytes& out, std::string_view cc) {
    REQUIRE(cc.size() == 4);
    out += cc;
}

// A fixed-width ADM ID field (BS.2088-1 §8.2's UID[12]/trackRef[14]/
// packRef[11]), written verbatim at exactly `width` bytes.
void put_fixed(Bytes& out, std::string_view value, std::size_t width) {
    REQUIRE(value.size() == width);
    out += value;
}

// Appends one chunk: 4-byte ID, 4-byte declared size, content, and a pad
// byte if the content length is odd (BS.2088-1 §4's own note on chunk
// alignment). `declared_size` lets a test force 0xFFFFFFFF to exercise the
// <ds64>-resolution path even for a chunk far smaller than 4 GB - legal for
// a real writer to do too, and the only practical way to test that path
// without an actual multi-gigabyte fixture.
void append_chunk(Bytes& out, std::string_view id, const Bytes& content,
                   std::optional<std::uint32_t> declared_size = std::nullopt) {
    put_fourcc(out, id);
    put_u32le(out, declared_size.value_or(static_cast<std::uint32_t>(content.size())));
    out += content;
    if (content.size() % 2 != 0) {
        out.push_back('\0');
    }
}

Bytes build_fmt_chunk(std::uint16_t channels, std::uint32_t sample_rate, std::uint16_t bits_per_sample) {
    Bytes fmt;
    put_u16le(fmt, 1);  // WAVE_FORMAT_PCM, BS.2088-1 §2.6.2
    put_u16le(fmt, channels);
    put_u32le(fmt, sample_rate);
    const auto block_align = static_cast<std::uint16_t>(channels * (bits_per_sample / 8));
    put_u32le(fmt, sample_rate * block_align);
    put_u16le(fmt, block_align);
    put_u16le(fmt, bits_per_sample);
    return fmt;
}

// One track, one UID - BS.2088-1 §8.3.1's "simple stereo" shape reduced to
// mono, matching this fixture's one-channel <fmt>.
Bytes build_chna_chunk() {
    Bytes chna;
    put_u16le(chna, 1);  // numTracks
    put_u16le(chna, 1);  // numUIDs
    put_u16le(chna, 1);  // trackIndex (1-based)
    put_fixed(chna, "ATU_00000001", 12);
    put_fixed(chna, "AT_00031001_01", 14);
    put_fixed(chna, "AP_00031001", 11);
    chna.push_back('\0');  // pad byte, §8.2
    return chna;
}

Bytes build_pcm16_data(int frames) {
    Bytes data;
    for (int frame = 0; frame < frames; ++frame) {
        const auto sample = static_cast<std::int16_t>((frame * 4000) - 12000);
        put_u16le(data, static_cast<std::uint16_t>(sample));
    }
    return data;
}

std::string_view kCarAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
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
      <width>12.5</width>
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

// A minimal DirectSpeakers channel (BS.2076-2 §5.4.3.1, Table 12) - one
// block naming a single loudspeaker position by label and polar coordinate.
//
// IDs use a "9001" suffix rather than a low, standard-looking one: libadm's parseXml() always
// merges the file's own content into a document pre-populated with BS.2076-2 Annex A's "common
// definitions" (see find_by_id's own comment below), and it turns out the standard set already
// defines an "AC_00010001"/"AB_00010001_00000001" of its own for the very first DirectSpeakers
// layout - reusing that ID here collided ("Duplicate Id AC_00010001 found") until this fixture
// picked one clear of the common set's low ID space instead, confirmed empirically against
// libadm's own vendored resources/common_definitions.xml.
std::string_view kDirectSpeakersAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="FrontLeft" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+030</speakerLabel>
      <position coordinate="azimuth">30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
)";

// A minimal first-order HOA channel (BS.2076-2 §5.4.3.4, Table 18) - degree/
// order/normalization identify one ACN/N3D component. Same "9001" suffix
// reasoning as kDirectSpeakersAdmXml above - "AC_00040102" is itself one of
// the common set's own HOA definitions.
std::string_view kHoaAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00049001" audioChannelFormatName="N3D_ACN_1" typeLabel="0004" typeDefinition="HOA">
    <audioBlockFormat audioBlockFormatID="AB_00049001_00000001">
      <degree>1</degree>
      <order>-1</order>
      <normalization>N3D</normalization>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
)";

// A Cartesian Objects channel (BS.2076-2 Table 16/17) with a jumpPosition
// (§10.3) - exercises the cartesian branch of ac3adm::Position and the
// channelLock/jumpPosition fields adm_model.cpp's convert() populates.
std::string_view kCartesianObjectAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioChannelFormat audioChannelFormatID="AC_00031002" audioChannelFormatName="Car2" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00031002_00000001">
      <cartesian>1</cartesian>
      <position coordinate="X">-0.2</position>
      <position coordinate="Y">0.5</position>
      <position coordinate="Z">0.0</position>
      <jumpPosition interpolationLength="0.05">1</jumpPosition>
    </audioBlockFormat>
  </audioChannelFormat>
</audioFormatExtended>
)";

// Wraps fmt/chna/axml/data into a classic 32-bit "RIFF....WAVE" file -
// BS.2088-1's own compatibility case (§2.5) for a file that stays under the
// 4 GB threshold, which is what most real ADM BWF masters actually are.
//
// An empty `chna` or `axml` means "this chunk is genuinely absent", not "present with zero
// bytes of content" - the two are different things a reader must tell apart (BS.2088-1 §9 rule
// 2: <axml> is optional; a 0-byte <axml> is instead just a chunk holding an empty, non-
// well-formed "document"). libbw64's own parseChnaChunk() also hard-rejects a <chna> chunk
// under 4 bytes ("illegal chna chunk size"), so a real 0-byte <chna> is not even a legal fixture
// to write in the first place - skipping the append when empty covers both chunks correctly
// with one rule, confirmed necessary by actually hitting both failure modes.
Bytes build_riff(const Bytes& fmt, const Bytes& chna, const Bytes& axml, const Bytes& data) {
    Bytes body;
    append_chunk(body, "fmt ", fmt);
    if (!chna.empty()) {
        append_chunk(body, "chna", chna);
    }
    if (!axml.empty()) {
        append_chunk(body, "axml", axml);
    }
    append_chunk(body, "data", data);

    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));  // "WAVE" + body, classic RIFF ckSize
    put_fourcc(file, "WAVE");
    file += body;
    return file;
}

// Wraps the same four chunks into an RF64 file with a real <ds64> chunk
// (BS.2088-1 §§2.4, 4), deliberately declaring <data>'s own 32-bit ckSize
// as 0xFFFFFFFF so its real size has to come from ds64's dataSizeLow/High -
// the specific mechanism this container format exists for.
Bytes build_rf64(const Bytes& fmt, const Bytes& chna, const Bytes& axml, const Bytes& data) {
    // See build_riff()'s own comment just above: an empty chna/axml means "absent", so it
    // contributes nothing to either the total size below or the chunk list itself.
    const auto chunk_total = [](const Bytes& content) {
        return content.empty() ? std::size_t{0} : 8 + content.size() + (content.size() % 2);
    };
    const std::size_t ds64_content_size = 28;  // bw64Size(8) + dataSize(8) + dummy(8) + tableLength(4), no table[]
    const std::size_t ds64_total = 8 + ds64_content_size + (ds64_content_size % 2);
    const std::uint64_t bw64_size = 4 /* "WAVE" */ + ds64_total + chunk_total(fmt) + chunk_total(chna) +
                                     chunk_total(axml) + chunk_total(data);

    Bytes ds64_content;
    put_u64le(ds64_content, bw64_size);
    put_u64le(ds64_content, data.size());  // dataSizeLow/High
    put_u64le(ds64_content, 0);            // dummyLow/High, §4.2
    put_u32le(ds64_content, 0);            // tableLength - no other chunk needs 64-bit resolution here

    Bytes body;
    append_chunk(body, "ds64", ds64_content);
    append_chunk(body, "fmt ", fmt);
    if (!chna.empty()) {
        append_chunk(body, "chna", chna);
    }
    if (!axml.empty()) {
        append_chunk(body, "axml", axml);
    }
    append_chunk(body, "data", data, 0xFFFFFFFFu);  // forces ds64 resolution

    Bytes file;
    put_fourcc(file, "RF64");
    put_u32le(file, 0xFFFFFFFFu);  // top-level size also unresolved
    put_fourcc(file, "WAVE");
    file += body;
    return file;
}

Bytes minimal_fixture_bytes(bool as_rf64) {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    const Bytes axml(kCarAdmXml);
    const auto data = build_pcm16_data(4);
    return as_rf64 ? build_rf64(fmt, chna, axml, data) : build_riff(fmt, chna, axml, data);
}

// A one-channel BW64 file wrapping the given axml content, with no <chna>
// entry (not needed by the typeDefinition-specific tests below - they only
// look at doc->model, not doc->chna).
Bytes wrap_axml_only(std::string_view axml_xml) {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto data = build_pcm16_data(2);
    return build_riff(fmt, Bytes{}, Bytes(axml_xml), data);
}

// libadm's own parseXml() always merges the file's own content into a document already
// pre-populated with BS.2076-2 Annex A's "common definitions" (43 pack formats, 300 each of
// channel/stream/track formats, one per standard loudspeaker layout - confirmed by grepping
// libadm's own vendored resources/common_definitions.xml). Real ADM files rely on being able to
// reference those IDs (e.g. a stereo bed's pack format "AP_00010002") without re-declaring them
// locally, so ac3adm::ac3adm deliberately keeps them in the resulting AdmModel rather than
// filtering them back out - phase 2 needs exactly this, a pack/channel/stream/track format
// reference that resolves regardless of whether the file re-declared it or relied on the common
// set. That means pack_formats/channel_formats/stream_formats/track_formats are never just "what
// this one fixture defined" the way programmes/contents/objects/track_uids still are (common_
// definitions.xml defines none of those four) - this helper finds one specific element by ID out
// of a collection that may also hold the common set, rather than assuming it is the only/first
// entry.
//
// `id` is std::string_view, not const std::string& - every call site here passes a string
// literal, which would otherwise bind to a temporary std::string constructed just for the call.
// GCC 15's -Wdangling-reference (real, -Werror on the Linux/GCC preset - not something MSVC
// catches, confirmed by hitting this only on the Linux leg) flags the caller's own `const auto&`
// binding to this function's returned container element as a possible dangling reference in
// that situation, even though the returned reference is into `elements` and never aliases `id`
// at all - string_view sidesteps the whole question since a literal converts to it without
// constructing anything owning.
template <typename T>
const T& find_by_id(const std::vector<T>& elements, std::string_view id) {
    auto it = std::find_if(elements.begin(), elements.end(), [&](const T& e) { return e.id == id; });
    REQUIRE(it != elements.end());
    return *it;
}

}  // namespace

TEST_CASE("parses a minimal RIFF/WAVE ADM file", "[adm]") {
    std::istringstream stream(minimal_fixture_bytes(false));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());

    SECTION("PCM audio") {
        CHECK(doc->audio.sample_rate == 48000);
        CHECK(doc->audio.bits_per_sample == 16);
        REQUIRE(doc->audio.channels.size() == 1);
        REQUIRE(doc->audio.frame_count() == 4);
        // (frame * 4000 - 12000) / 32768, frame 0..3 - decoded independently
        // of ac3adm's own PCM-decoding arithmetic to actually check it.
        CHECK(doc->audio.channels[0][0] == Catch::Approx(-12000.0f / 32768.0f));
        CHECK(doc->audio.channels[0][3] == Catch::Approx(0.0f / 32768.0f));
    }

    SECTION("chna join table") {
        REQUIRE(doc->chna.size() == 1);
        CHECK(doc->chna[0].track_index == 1);
        CHECK(doc->chna[0].uid == "ATU_00000001");
        CHECK(doc->chna[0].track_ref == "AT_00031001_01");
        CHECK(doc->chna[0].pack_ref == "AP_00031001");
    }

    SECTION("ADM object graph") {
        REQUIRE(doc->model.programmes.size() == 1);
        CHECK(doc->model.programmes[0].id == "APR_1001");
        CHECK(doc->model.programmes[0].content_refs == std::vector<std::string>{"ACO_1001"});

        REQUIRE(doc->model.contents.size() == 1);
        CHECK(doc->model.contents[0].object_refs == std::vector<std::string>{"AO_1001"});

        REQUIRE(doc->model.objects.size() == 1);
        CHECK(doc->model.objects[0].id == "AO_1001");
        CHECK(doc->model.objects[0].pack_format_refs == std::vector<std::string>{"AP_00031001"});
        CHECK(doc->model.objects[0].track_uid_refs == std::vector<std::string>{"ATU_00000001"});

        const auto& pack_format = find_by_id(doc->model.pack_formats, "AP_00031001");
        CHECK(pack_format.type == ac3adm::TypeDefinition::kObjects);
        CHECK(pack_format.channel_format_refs == std::vector<std::string>{"AC_00031001"});

        const auto& channel = find_by_id(doc->model.channel_formats, "AC_00031001");
        CHECK(channel.type == ac3adm::TypeDefinition::kObjects);
        REQUIRE(channel.block_formats.size() == 1);
        const auto& block = channel.block_formats[0];
        CHECK(block.id == "AB_00031001_00000001");
        CHECK_FALSE(block.cartesian);
        REQUIRE(std::holds_alternative<ac3adm::PolarPosition>(block.position));
        const auto& polar = std::get<ac3adm::PolarPosition>(block.position);
        CHECK(polar.azimuth_deg == Catch::Approx(-22.5));
        CHECK(polar.elevation_deg == Catch::Approx(5.0));
        CHECK(polar.distance == Catch::Approx(1.0));
        CHECK(block.width == Catch::Approx(12.5));

        REQUIRE(doc->model.track_uids.size() == 1);
        CHECK(doc->model.track_uids[0].uid == "ATU_00000001");
        CHECK(doc->model.track_uids[0].has_sample_rate);
        CHECK(doc->model.track_uids[0].sample_rate == 48000);
    }
}

TEST_CASE("parses the same content via RF64 with a ds64-resolved data chunk", "[adm]") {
    std::istringstream stream(minimal_fixture_bytes(true));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    CHECK(doc->audio.sample_rate == 48000);
    REQUIRE(doc->audio.frame_count() == 4);
    REQUIRE(doc->model.objects.size() == 1);
    CHECK(doc->model.objects[0].id == "AO_1001");
}

TEST_CASE("rejects a file that is not RIFF/RF64/BW64", "[adm]") {
    std::istringstream stream(std::string("NOPE") + std::string(8, '\0'));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    // libbw64 reports "not a recognized container" the same way it reports
    // "could not open" - a single std::runtime_error family with no
    // distinguishing exception type (see src/ac3adm/src/adm.cpp's own
    // comment on parse_bw64_path) - so this, too, surfaces as kCannotOpen
    // rather than the more specific kNotRiff.
    CHECK(doc.error() == ac3adm::AdmError::kCannotOpen);
}

TEST_CASE("malformed XML in axml surfaces as kMalformedXml", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    // A genuinely unterminated tag (no closing '>', truncated mid-attribute) - not just a
    // mismatched close tag. libadm's own XML layer (rapidxml, vendored as a private
    // dependency) turned out to be lenient about mismatched close tags - a
    // "<a><b></a>" style fixture parses "successfully" (in whatever shape rapidxml
    // produces for it) and only fails later, as an ADM-structure complaint
    // (AdmError::kMalformedAdm, see the next test and src/ac3adm/src/adm.cpp's own
    // comment on why). This fixture instead breaks XML tokenizing itself, which is
    // needed to actually reach kMalformedXml - confirmed empirically, not assumed.
    const Bytes axml = "<audioFormatExtended><audioObject audioObjectID=\"AO_1";
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error() == ac3adm::AdmError::kMalformedXml);
}

TEST_CASE("a missing required ADM attribute surfaces as kMalformedXml", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    // audioObjectID (Required=Yes, BS.2076-2 Table 24) is missing.
    const Bytes axml =
        R"(<audioFormatExtended version="ITU-R_BS.2076-2"><audioObject audioObjectName="Nameless"/></audioFormatExtended>)";
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    // Not kMalformedAdm, despite "missing a required ADM attribute" sounding like the more
    // ADM-shaped complaint of the two: libadm's own mandatory-attribute check
    // (xml_parser_helper.hpp's parseAttribute()) throws a plain, untyped std::runtime_error
    // rather than one of its own adm::error:: types - confirmed by catching and printing the
    // real exception during development, not assumed from the enum's own naming. See
    // ac3adm::AdmError's own doc comment (ac3adm.hpp) and src/ac3adm/src/adm.cpp's
    // read_adm_model() for the full explanation.
    CHECK(doc.error() == ac3adm::AdmError::kMalformedXml);
}

TEST_CASE("a duplicate ADM element ID surfaces as kMalformedAdm", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    // Two audioProgramme elements sharing one ID - libadm's own id_assignment/duplicate-id
    // check (xml_parser.cpp) throws adm::error::XmlParsingDuplicateId, a real
    // adm::error::AdmException subclass, unlike the missing-attribute case just above - this is
    // what AdmError::kMalformedAdm is actually reachable for.
    const Bytes axml =
        R"(<audioFormatExtended version="ITU-R_BS.2076-2">
             <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="A"/>
             <audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="B"/>
           </audioFormatExtended>)";
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error() == ac3adm::AdmError::kMalformedAdm);
}

// Test name deliberately ASCII-only (no "section-sign clause number" the way this file's other
// comments cite one) - ctest's own Windows test invocation mangles a non-ASCII TEST_CASE name
// when passing it as a Catch2 --name filter argument, turning it into a byte no test actually
// matches ("No test cases matched") and failing the ctest entry outright even though the test
// itself is fine - confirmed by hitting exactly that under `ctest -j`, not assumed.
TEST_CASE("chna rows with trackIndex 0 (unused placeholders, BS.2088-1 clause 8.2) pass through unfiltered",
          "[adm]") {
    // libbw64's own ChnaChunk::numTracks() recomputes the field from the *distinct* trackIndex
    // values actually present (chunks.hpp) rather than trusting the declared one, and
    // bw64::readFile rejects a mismatch - so numTracks here is 2 (the real index 1 plus the
    // placeholder 0), not 1, even though only one row is a real track.
    Bytes chna;
    put_u16le(chna, 2);  // numTracks (see above - counts the placeholder's 0 as distinct too)
    put_u16le(chna, 2);  // numUIDs (one real, one placeholder)
    put_u16le(chna, 1);
    put_fixed(chna, "ATU_00000001", 12);
    put_fixed(chna, "AT_00031001_01", 14);
    put_fixed(chna, "AP_00031001", 11);
    chna.push_back('\0');
    put_u16le(chna, 0);  // §8.2: trackIndex 0 = unused row
    put_fixed(chna, std::string(12, '\0'), 12);
    put_fixed(chna, std::string(14, '\0'), 14);
    put_fixed(chna, std::string(11, '\0'), 11);
    chna.push_back('\0');

    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const Bytes axml(kCarAdmXml);
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, axml, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    // ac3adm::ac3adm surfaces every row as-is (ChnaEntry's own comment in model.hpp: "callers
    // that want 'the entries for track N' filter by track_index themselves") - it does not drop
    // placeholder rows itself, so both come through here.
    REQUIRE(doc->chna.size() == 2);
    CHECK(doc->chna[0].track_index == 1);
    CHECK(doc->chna[0].uid == "ATU_00000001");
    CHECK(doc->chna[1].track_index == 0);
}

TEST_CASE("a file with no axml chunk still parses, with an empty ADM model", "[adm]") {
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto chna = build_chna_chunk();
    const auto data = build_pcm16_data(2);
    std::istringstream stream(build_riff(fmt, chna, Bytes{}, data));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    CHECK(doc->model.programmes.empty());
    CHECK(doc->model.objects.empty());
    REQUIRE(doc->audio.frame_count() == 2);
}

TEST_CASE("parses a DirectSpeakers channel's speakerLabel and polar position", "[adm][model]") {
    std::istringstream stream(wrap_axml_only(kDirectSpeakersAdmXml));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    const auto& channel = find_by_id(doc->model.channel_formats, "AC_00019001");
    CHECK(channel.type == ac3adm::TypeDefinition::kDirectSpeakers);
    REQUIRE(channel.block_formats.size() == 1);
    const auto& block = channel.block_formats[0];
    REQUIRE(block.speaker_labels.size() == 1);
    CHECK(block.speaker_labels[0] == "M+030");
    REQUIRE(std::holds_alternative<ac3adm::PolarPosition>(block.position));
    CHECK(std::get<ac3adm::PolarPosition>(block.position).azimuth_deg == Catch::Approx(30.0));
}

TEST_CASE("parses HOA order/degree/normalization", "[adm][model]") {
    std::istringstream stream(wrap_axml_only(kHoaAdmXml));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    const auto& channel = find_by_id(doc->model.channel_formats, "AC_00049001");
    CHECK(channel.type == ac3adm::TypeDefinition::kHoa);
    const auto& block = channel.block_formats.at(0);
    REQUIRE(block.has_hoa_order);
    CHECK(block.hoa_order == -1);
    REQUIRE(block.has_hoa_degree);
    CHECK(block.hoa_degree == 1);
    CHECK(block.hoa_normalization == "N3D");
}

TEST_CASE("parses Cartesian object positions and jumpPosition", "[adm][model]") {
    std::istringstream stream(wrap_axml_only(kCartesianObjectAdmXml));
    auto doc = ac3adm::parse_bw64(stream);
    REQUIRE(doc.has_value());
    const auto& channel = find_by_id(doc->model.channel_formats, "AC_00031002");
    const auto& block = channel.block_formats.at(0);
    CHECK(block.cartesian);
    REQUIRE(std::holds_alternative<ac3adm::CartesianPosition>(block.position));
    const auto& cart = std::get<ac3adm::CartesianPosition>(block.position);
    CHECK(cart.x == Catch::Approx(-0.2));
    CHECK(cart.y == Catch::Approx(0.5));
    REQUIRE(block.has_jump_position);
    CHECK(block.jump_position);
    REQUIRE(block.has_interpolation_length);
    CHECK(block.interpolation_length_s == Catch::Approx(0.05));
}

TEST_CASE("ADM sample-based time format matches the equivalent decimal form", "[adm][model]") {
    // BS.2076-2 §5.11's own example: "01:34:16.12000S48000 is the same as 01:34:16.25000".
    const auto fmt = build_fmt_chunk(1, 48000, 16);
    const auto data = build_pcm16_data(2);

    const Bytes axml_samples(
        R"(<audioFormatExtended version="ITU-R_BS.2076-2">
             <audioObject audioObjectID="AO_1001" audioObjectName="X" start="01:34:16.12000S48000"/>
           </audioFormatExtended>)");
    const Bytes axml_decimal(
        R"(<audioFormatExtended version="ITU-R_BS.2076-2">
             <audioObject audioObjectID="AO_1001" audioObjectName="X" start="01:34:16.25000"/>
           </audioFormatExtended>)");

    std::istringstream stream_samples(build_riff(fmt, Bytes{}, axml_samples, data));
    std::istringstream stream_decimal(build_riff(fmt, Bytes{}, axml_decimal, data));
    auto doc_samples = ac3adm::parse_bw64(stream_samples);
    auto doc_decimal = ac3adm::parse_bw64(stream_decimal);
    REQUIRE(doc_samples.has_value());
    REQUIRE(doc_decimal.has_value());
    REQUIRE(doc_samples->model.objects.size() == 1);
    REQUIRE(doc_decimal->model.objects.size() == 1);
    CHECK(doc_samples->model.objects[0].start_s == Catch::Approx(doc_decimal->model.objects[0].start_s));
}

TEST_CASE("describe() returns a non-empty string for every AdmError", "[adm]") {
    using ac3adm::AdmError;
    for (const auto error : {AdmError::kCannotOpen, AdmError::kNotRiff, AdmError::kMissingFmt,
                              AdmError::kMissingData, AdmError::kUnsupportedFormat, AdmError::kMalformedXml,
                              AdmError::kMalformedAdm, AdmError::kOther}) {
        CHECK_FALSE(ac3adm::describe(error).empty());
    }
}
