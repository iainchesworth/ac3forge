#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_frame.hpp"

namespace {

// TS 102 366 §H.2.1.2.1, transcribed as the decoder, not as the inverse of the
// writer. A round trip against the writer's own logic would agree with itself
// however wrong it was; agreeing with the standard's pseudocode is the point.
std::uint32_t read_variable_bits(ac3::BitReader& r, int group_bits) {
    std::uint32_t value = 0;
    while (true) {
        value += r.read(group_bits);
        if (r.read_bit() == 0) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

std::vector<std::byte> encode_variable_bits(std::uint32_t value, int group_bits) {
    ac3::BitWriter w;
    ac3::emdf::put_variable_bits(w, value, group_bits);
    const int size = ac3::emdf::variable_bits_size(value, group_bits);
    CHECK(static_cast<int>(w.bit_count()) == size);
    return w.take();
}

// The offset of the first set bit pattern equal to the EMDF sync word, in bits
// from the start of the frame, or npos. §H.1 puts the container in a reserved
// space whose position depends on how many bits the audio took, so finding it
// is a scan - which is exactly why it has a sync word at all.
std::size_t find_emdf_sync(std::span<const std::byte> frame) {
    const std::size_t total = frame.size() * 8;
    for (std::size_t bit = 0; bit + 16 <= total; ++bit) {
        ac3::BitReader r{frame};
        r.skip(bit);
        if (r.read(16) == ac3::emdf::kSyncWord) {
            return bit;
        }
    }
    return static_cast<std::size_t>(-1);
}

}  // namespace

TEST_CASE("variable_bits matches the standard's decoder", "[emdf]") {
    for (const int n : {2, 5, 8, 11}) {
        CAPTURE(n);
        for (const std::uint32_t value :
             {0u, 1u, 2u, 7u, 255u, 256u, 1000u, 4095u, 65535u, 100000u}) {
            CAPTURE(value);
            const auto bytes = encode_variable_bits(value, n);
            ac3::BitReader r{bytes};
            CHECK(read_variable_bits(r, n) == value);
        }
    }
}

TEST_CASE("variable_bits spends the fewest groups it can", "[emdf]") {
    // Table H.2.1: one group covers [0, 2^n), two cover the next 2^2n values.
    // Getting the group_offset wrong makes the boundary values collide - two
    // encodings for one value, and a decoder one group out of step.
    STATIC_CHECK(true);
    CHECK(ac3::emdf::variable_bits_size(0, 8) == 9);
    CHECK(ac3::emdf::variable_bits_size(255, 8) == 9);
    CHECK(ac3::emdf::variable_bits_size(256, 8) == 18);   // 2^8, first 2-group
    CHECK(ac3::emdf::variable_bits_size(65791, 8) == 18); // 2^8 + 2^16 - 1
    CHECK(ac3::emdf::variable_bits_size(65792, 8) == 27);

    // The boundary pair must decode to adjacent values, not the same one.
    for (const std::uint32_t value : {255u, 256u, 65791u, 65792u}) {
        const auto bytes = encode_variable_bits(value, 8);
        ac3::BitReader r{bytes};
        CHECK(read_variable_bits(r, 8) == value);
    }
}

TEST_CASE("EMDF container carries its payloads verbatim", "[emdf]") {
    const std::vector<std::byte> oamd{std::byte{0xDE}, std::byte{0xAD}};
    const std::vector<std::byte> joc{std::byte{0xBE}, std::byte{0xEF}, std::byte{0x01}};
    const std::array<ac3::emdf::Payload, 2> payloads{{
        {.id = ac3::emdf::kPayloadIdOamd, .bytes = oamd},
        {.id = ac3::emdf::kPayloadIdJoc, .bytes = joc},
    }};
    const auto container = ac3::emdf::build_container(payloads, 1);

    ac3::BitReader r{container};
    CHECK(r.read(16) == 0x5838);
    const auto length = r.read(16);
    // §H.2.2.1.2 measures the container, which emdf_sync precedes; the four
    // bytes of sync are therefore not part of the count.
    CHECK(length == container.size() - 4);

    CHECK(r.read(2) == 0);  // emdf_version
    CHECK(r.read(3) == 0);  // key_id

    for (const auto& expected : payloads) {
        CHECK(r.read(5) == static_cast<std::uint32_t>(expected.id));
        // §H.2.1.3 with TS 103 420 Table 56's values.
        CHECK(r.read(1) == 0);  // smploffste
        CHECK(r.read(1) == 0);  // duratione
        CHECK(r.read(1) == 1);  // groupide
        CHECK(read_variable_bits(r, 2) == 1);  // groupid
        // TS 103 420 Table 56 says codecdatae is 1 and TS 102 366 §H.2.2.3.7
        // says it "shall be set to '0'". Dolby's own reference streams send 0,
        // and since the payload config has no length of its own, the eight
        // reserved bits a 1 drags in shift everything after them - so this is
        // not a stylistic choice, it decides whether the container parses.
        CHECK(r.read(1) == 0);  // codecdatae
        CHECK(r.read(1) == 0);  // discard_unknown_payload
        CHECK(r.read(1) == 1);  // payload_frame_aligned
        CHECK(r.read(1) == 0);  // create_duplicate
        CHECK(r.read(1) == 0);  // remove_duplicate
        CHECK(r.read(5) == 0);  // priority
        CHECK(r.read(2) == 0);  // proc_allowed

        const auto size = read_variable_bits(r, 8);
        REQUIRE(size == expected.bytes.size());
        for (const auto byte : expected.bytes) {
            CHECK(r.read(8) == std::to_integer<std::uint32_t>(byte));
        }
    }

    CHECK(r.read(5) == 0);      // the payload list terminates
    CHECK(r.read(2) == 0b10);   // protection_length_primary: 32 bits
    CHECK(r.read(2) == 0b01);   // protection_length_secondary: 8 bits
    CHECK(r.read(32) == 0);     // protection_bits_primary
    CHECK(r.read(8) == 0);      // protection_bits_secondary
    CHECK_FALSE(r.overflowed());
}

TEST_CASE("parse_container decodes back to the payloads it was given", "[emdf]") {
    const std::vector<std::byte> oamd{std::byte{0xDE}, std::byte{0xAD}, std::byte{0x00}};
    const std::vector<std::byte> joc{std::byte{0xBE}, std::byte{0xEF}, std::byte{0x01}, std::byte{0xFF}};
    const std::array<ac3::emdf::Payload, 2> payloads{{
        {.id = ac3::emdf::kPayloadIdOamd, .bytes = oamd},
        {.id = ac3::emdf::kPayloadIdJoc, .bytes = joc},
    }};
    const auto container = ac3::emdf::build_container(payloads, 2);

    const auto result = ac3::emdf::parse_container(container);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    const auto& decoded = **result;
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].id == ac3::emdf::kPayloadIdOamd);
    CHECK(decoded[0].bytes == oamd);
    CHECK(decoded[1].id == ac3::emdf::kPayloadIdJoc);
    CHECK(decoded[1].bytes == joc);
}

TEST_CASE("parse_container decodes a container that does not start at bit 0", "[emdf]") {
    // §H.2.2.1.1's own justification for scanning rather than a fixed offset:
    // nothing says the container starts where a decoder might expect it to.
    const std::vector<std::byte> oamd{std::byte{0x01}, std::byte{0x02}};
    const std::array<ac3::emdf::Payload, 1> payloads{
        {{.id = ac3::emdf::kPayloadIdOamd, .bytes = oamd}}};
    const auto container = ac3::emdf::build_container(payloads);

    ac3::BitWriter w;
    w.put(0b0101101, 7);  // arbitrary, non-byte-aligned leading noise
    for (const auto byte : container) {
        w.put(std::to_integer<std::uint32_t>(byte), 8);
    }
    const auto data = w.take();

    const auto result = ac3::emdf::parse_container(data);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    REQUIRE((*result)->size() == 1);
    CHECK((**result)[0].bytes == oamd);
}

TEST_CASE("parse_container tolerates data with no EMDF at all", "[emdf]") {
    const std::vector<std::byte> silence(64, std::byte{0x00});
    const auto result = ac3::emdf::parse_container(silence);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->has_value());

    // Not just zeros: the sync word genuinely absent from real content too.
    std::vector<std::byte> noise(64);
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<std::byte>((i * 37 + 11) & 0xFF);
    }
    const auto noise_result = ac3::emdf::parse_container(noise);
    REQUIRE(noise_result.has_value());
    CHECK_FALSE(noise_result->has_value());
}

TEST_CASE("parse_container rejects a container truncated after the sync word", "[emdf]") {
    const std::vector<std::byte> oamd{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    const std::array<ac3::emdf::Payload, 1> payloads{
        {{.id = ac3::emdf::kPayloadIdOamd, .bytes = oamd}}};
    const auto container = ac3::emdf::build_container(payloads);

    for (const std::size_t cut : {std::size_t{4}, container.size() / 2, container.size() - 1}) {
        CAPTURE(cut);
        const std::vector<std::byte> truncated(container.begin(),
                                               container.begin() + static_cast<std::ptrdiff_t>(cut));
        const auto result = ac3::emdf::parse_container(truncated);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::emdf::ParseError::kTruncated);
    }
}

TEST_CASE("parse_container refuses a payload config outside Table 56's shape", "[emdf]") {
    // Hand-built, not through put_payload_config (private to emdf.cpp): a
    // container whose smploffste is set, which no stream this project
    // produces ever does and this decoder does not know how to follow past.
    ac3::BitWriter body;
    body.put(0, 2);  // emdf_version
    body.put(0, 3);  // key_id
    body.put(ac3::emdf::kPayloadIdOamd, 5);
    body.put(1, 1);  // smploffste: the deviation under test
    body.put(0, 1);  // duratione
    body.put(1, 1);  // groupide
    body.put(0, 2);  // groupid value: one group, no offset
    body.put(0, 1);  // read_more: last (only) group
    body.put(0, 1);  // codecdatae
    body.put(0, 1);  // discard_unknown_payload
    body.put(1, 1);  // payload_frame_aligned
    body.put(0, 1);  // create_duplicate
    body.put(0, 1);  // remove_duplicate
    body.put(0, 5);  // priority
    body.put(0, 2);  // proc_allowed
    body.put(1, 8);  // emdf_payload_size value: one group, size 1
    body.put(0, 1);  // read_more: last (only) group
    body.put(0x42, 8);  // the one payload byte
    body.put(0, 5);      // terminator
    body.put(0b10, 2);
    body.put(0b01, 2);
    body.put(0, 32);
    body.put(0, 8);
    const auto payload_bytes = body.take();

    ac3::BitWriter out;
    out.put(ac3::emdf::kSyncWord, 16);
    out.put(static_cast<std::uint32_t>(payload_bytes.size()), 16);
    for (const auto byte : payload_bytes) {
        out.put(std::to_integer<std::uint32_t>(byte), 8);
    }
    const auto data = out.take();

    const auto result = ac3::emdf::parse_container(data);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::emdf::ParseError::kUnsupportedConfig);
}

TEST_CASE("an EMDF container rides in a block skip field", "[emdf][eac3]") {
    const std::vector<std::byte> payload(6, std::byte{0x5A});
    const std::array<ac3::emdf::Payload, 1> payloads{
        {{.id = ac3::emdf::kPayloadIdOamd, .bytes = payload}}};
    const auto container = ac3::emdf::build_container(payloads);

    const ac3::eac3::FrameConfig config{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true};
    const auto plain = ac3::eac3::build_silent_frame(config);
    const auto carrying = ac3::eac3::build_silent_frame(config, container);
    REQUIRE(plain.has_value());
    REQUIRE(carrying.has_value());

    // frmsiz is signalled, not derived, so carrying metadata must not change
    // the frame's length - the container displaces padding, nothing else.
    CHECK(plain->size() == carrying->size());
    CHECK(ac3::crc16(std::span<const std::byte>{*carrying}.subspan(2)) == 0x0000);
    CHECK(find_emdf_sync(*plain) == static_cast<std::size_t>(-1));

    const std::size_t at = find_emdf_sync(*carrying);
    REQUIRE(at != static_cast<std::size_t>(-1));

    // The container is INSIDE the audio blocks, not after them: §5.4.3.58's
    // skip field sits in block 0 between the bit-allocation fields and the
    // mantissas. Dolby's own DD+ JOC streams carry it there and leave
    // auxdatae at 0 - checked against the DD+ test signals in their Online
    // Delivery Kit - and their decoder does not look in the aux field.
    const std::size_t total = carrying->size() * 8;
    CHECK(at < total / 2);

    ac3::BitReader tail{*carrying};
    tail.skip(total - 18);
    CHECK(tail.read(1) == 0);  // auxdatae: nothing in the aux field

    // skipflde has to be set for the block-level field to exist at all, and it
    // lives in audfrm. bsi is 54 bits with addbsie == 0, then audfrm's
    // expstre, ahte, snroffststr(2), transproce, blkswe, dithflage, bamode,
    // frmfgaincode, dbaflde put skipflde at bit 64.
    ac3::BitReader frm{*carrying};
    frm.skip(64);
    CHECK(frm.read(1) == 1);  // skipflde
    // ... and a frame with nothing to carry must leave it clear, or every
    // block would pay a bit for a field that is never used.
    ac3::BitReader plain_frm{*plain};
    plain_frm.skip(64);
    CHECK(plain_frm.read(1) == 0);
}

TEST_CASE("addbsi announces object audio", "[emdf][eac3]") {
    const ac3::eac3::FrameConfig config{.bitrate_kbps = 448,
                                        .acmod = ac3::Acmod::k3_2,
                                        .lfe = true,
                                        .oba_complexity_index = 10};
    const auto frame = ac3::eac3::build_silent_frame(config);
    REQUIRE(frame.has_value());

    // bsi up to addbsie: sync(16) strmtyp(2) substreamid(3) frmsiz(11) fscod(2)
    // numblkscod(2) acmod(3) lfeon(1) bsid(5) dialnorm(5) compre(1) mixmdate(1)
    // infomdate(1) = 53 bits.
    ac3::BitReader r{*frame};
    r.skip(53);
    CHECK(r.read(1) == 1);  // addbsie
    CHECK(r.read(6) == 1);  // addbsil: two bytes, coded as bytes - 1
    CHECK(r.read(7) == 0);  // reserved
    CHECK(r.read(1) == 1);  // flag_ec3_extension_type_a
    CHECK(r.read(8) == 10); // complexity_index_type_a

    // §8.3.2.2 caps the object count at 16.
    CHECK(ac3::eac3::build_silent_frame({.oba_complexity_index = 17}).error() ==
          ac3::FrameError::kInvalidObjectAudio);
}
