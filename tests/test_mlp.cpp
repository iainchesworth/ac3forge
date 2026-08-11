#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "ac3/mlp/crc.hpp"
#include "ac3/mlp/mlp_tables.hpp"
#include "ac3/mlp/restart_header.hpp"
#include "ac3/mlp/sync.hpp"

// Covers only what's fully specified and built so far: mlp_sync's
// check_nibble, major_sync_crc, and major_sync_info()'s pack/parse round
// trip. block_data()'s compression algorithm, and therefore any real audio
// payload, isn't implemented yet - see docs/concepts/truehd-mlp.md.

namespace {

std::vector<std::byte> to_bytes(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> out;
    for (auto b : bytes) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

}  // namespace

TEST_CASE("major_sync_crc: appending the CRC drives the register to zero", "[mlp]") {
    // No independent catalogue check value exists for this polynomial (it's
    // not one of the well-known CRCs core/crc16.hpp's test can cite against) -
    // so this leans on the structural property every CRC of this shape has:
    // covered data followed by its own CRC always re-computes to zero.
    std::mt19937 rng(0xF8726FBA);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(1, 256);

    for (int trial = 0; trial < 50; ++trial) {
        std::vector<std::byte> msg(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : msg) {
            b = static_cast<std::byte>(byte_dist(rng));
        }
        const std::uint16_t crc = ac3::mlp::major_sync_crc(msg);
        auto with_crc = msg;
        with_crc.push_back(static_cast<std::byte>(crc >> 8));
        with_crc.push_back(static_cast<std::byte>(crc & 0xFF));
        CHECK(ac3::mlp::major_sync_crc(with_crc) == 0x0000);
    }
}

TEST_CASE("major_sync_crc: empty input yields the initial register", "[mlp]") {
    CHECK(ac3::mlp::major_sync_crc({}) == 0x0000);
}

TEST_CASE("major_sync_crc: usable at compile time", "[mlp]") {
    constexpr std::array<std::byte, 2> data{std::byte{0xF8}, std::byte{0x72}};
    constexpr auto value = ac3::mlp::major_sync_crc(data);
    STATIC_CHECK(value == ac3::mlp::major_sync_crc(data));
}

TEST_CASE("compute_check_nibble: XOR of every mlp_sync nibble is 0xF", "[mlp]") {
    // §4.1.1: check_nibble ^ access_unit_length's 3 nibbles ^ input_timing's
    // 4 nibbles == 0xF, by construction. Random-sample the field, don't hand-
    // pick values that happen to work.
    std::mt19937 rng(0x31EA);
    std::uniform_int_distribution<std::uint32_t> length_dist(0, 0xFFF);   // u(12)
    std::uniform_int_distribution<std::uint32_t> timing_dist(0, 0xFFFF);  // u(16)

    for (int trial = 0; trial < 200; ++trial) {
        const auto length = static_cast<std::uint16_t>(length_dist(rng));
        const auto timing = static_cast<std::uint16_t>(timing_dist(rng));
        const auto nibble = ac3::mlp::compute_check_nibble(length, timing);
        CHECK(nibble <= 0xF);

        std::uint8_t total = nibble;
        for (int shift = 8; shift >= 0; shift -= 4) {
            total ^= static_cast<std::uint8_t>((length >> shift) & 0xF);
        }
        for (int shift = 12; shift >= 0; shift -= 4) {
            total ^= static_cast<std::uint8_t>((timing >> shift) & 0xF);
        }
        CHECK(total == 0xF);
    }
}

TEST_CASE("build_major_sync_info: fixed sync words land at their documented offsets", "[mlp]") {
    const ac3::mlp::MajorSyncInfo info{};
    const auto bytes = ac3::mlp::build_major_sync_info(info);

    // §3.1: "the bitfield of 32 bits starting at bit offset 32 from the
    // start of the access unit always equals 0xF8726FBA" - and format_sync
    // is the first field of major_sync_info(), so it's also this buffer's
    // first 32 bits.
    REQUIRE(bytes.size() >= 4);
    CHECK(std::to_integer<std::uint32_t>(bytes[0]) == 0xF8);
    CHECK(std::to_integer<std::uint32_t>(bytes[1]) == 0x72);
    CHECK(std::to_integer<std::uint32_t>(bytes[2]) == 0x6F);
    CHECK(std::to_integer<std::uint32_t>(bytes[3]) == 0xBA);

    // §4.2.3: signature (0xB752) follows format_info (v(32)), i.e. at byte
    // offset 8.
    REQUIRE(bytes.size() >= 10);
    CHECK(std::to_integer<std::uint32_t>(bytes[8]) == 0xB7);
    CHECK(std::to_integer<std::uint32_t>(bytes[9]) == 0x52);
}

TEST_CASE("build_major_sync_info: fixed size in v1 scope (no 16ch tier)", "[mlp]") {
    // format_sync(32)+format_info(32)+signature(16)+flags(16)+reserved(16)+
    // variable_rate(1)+peak_data_rate(15)+substreams(4)+reserved(2)+
    // extended_substream_info(2)+substream_info(8) = 144 bits, plus
    // channel_meaning()'s reserved(6)+3*enabled(1)+reserved(1)+
    // drc_start_up_gain(7)+ch2(6+6)+ch6(5+6+5)+ch8(5+6+6)+reserved(1)+
    // extra_channel_meaning_present(1) = 64 bits, plus major_sync_info_CRC
    // (16). (144+64+16)/8 = 28 bytes exactly - extra_channel_meaning_present
    // is always 0 in v1 scope, so there's no variable-length branch to make
    // this anything but constant.
    const ac3::mlp::MajorSyncInfo info{};
    CHECK(ac3::mlp::build_major_sync_info(info).size() == 28);
}

TEST_CASE("major_sync_info round trip: default (stereo)", "[mlp]") {
    ac3::mlp::MajorSyncInfo info{};
    info.sample_rate = ac3::mlp::SampleRate::k48000;
    info.two_channel_content = ac3::mlp::TwoChannelContent::kStereo;

    const auto bytes = ac3::mlp::build_major_sync_info(info);
    ac3::mlp::MajorSyncInfo parsed{};
    REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));

    CHECK(parsed.sample_rate == info.sample_rate);
    CHECK(parsed.two_channel_content == info.two_channel_content);
    CHECK(parsed.substreams == info.substreams);
}

TEST_CASE("major_sync_info round trip: 5.1 with Surround EX modifier", "[mlp]") {
    using ac3::mlp::Location;

    ac3::mlp::MajorSyncInfo info{};
    info.sample_rate = ac3::mlp::SampleRate::k96000;
    ac3::mlp::Assignment assignment = 0;
    assignment = ac3::mlp::with(assignment, Location::kMain);
    assignment = ac3::mlp::with(assignment, Location::kCentre);
    assignment = ac3::mlp::with(assignment, Location::kLfe);
    assignment = ac3::mlp::with(assignment, Location::kSurround);
    info.six_channel_assignment = assignment;
    info.six_channel_modifier =
        ac3::mlp::surround_modifier(ac3::mlp::SurroundModifier::kSurroundExOrProLogicIIx);
    info.substreams = 2;
    info.meaning.ch6_dialogue_norm = 27;
    info.meaning.ch6_mix_level = 40;

    REQUIRE(ac3::mlp::channel_count(assignment, ac3::mlp::kSixChannelAssignmentBits) == 6);

    const auto bytes = ac3::mlp::build_major_sync_info(info);
    ac3::mlp::MajorSyncInfo parsed{};
    REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));

    CHECK(parsed.sample_rate == info.sample_rate);
    CHECK(parsed.six_channel_assignment == info.six_channel_assignment);
    CHECK(parsed.six_channel_modifier == info.six_channel_modifier);
    CHECK(parsed.substreams == info.substreams);
    CHECK(parsed.meaning.ch6_dialogue_norm == info.meaning.ch6_dialogue_norm);
    CHECK(parsed.meaning.ch6_mix_level == info.meaning.ch6_mix_level);
}

TEST_CASE("major_sync_info round trip: 8-channel, primary assignment table", "[mlp]") {
    using ac3::mlp::Location;

    ac3::mlp::MajorSyncInfo info{};
    ac3::mlp::Assignment assignment = 0;
    for (auto loc : {Location::kMain, Location::kCentre, Location::kLfe, Location::kSurround,
                     Location::kBack}) {
        assignment = ac3::mlp::with(assignment, loc);
    }
    info.eight_channel_assignment = assignment;
    info.eight_channel_use_alternate_table = false;
    info.substreams = 3;

    REQUIRE(ac3::mlp::channel_count(assignment, ac3::mlp::kEightChannelAssignmentBits) == 8);

    const auto bytes = ac3::mlp::build_major_sync_info(info);
    ac3::mlp::MajorSyncInfo parsed{};
    REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));

    CHECK_FALSE(parsed.eight_channel_use_alternate_table);
    CHECK(parsed.eight_channel_assignment == info.eight_channel_assignment);
    CHECK(parsed.substreams == info.substreams);
}

TEST_CASE("major_sync_info round trip: 8-channel, alternate (Tsl/Tsr) assignment table", "[mlp]") {
    ac3::mlp::MajorSyncInfo info{};
    info.eight_channel_use_alternate_table = true;
    info.eight_channel_alternate.main = true;
    info.eight_channel_alternate.centre = true;
    info.eight_channel_alternate.lfe = true;
    info.eight_channel_alternate.surround = true;
    info.eight_channel_alternate.top_side = true;
    info.substreams = 3;

    REQUIRE(info.eight_channel_alternate.channel_count() == 8);

    const auto bytes = ac3::mlp::build_major_sync_info(info);
    ac3::mlp::MajorSyncInfo parsed{};
    REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));

    CHECK(parsed.eight_channel_use_alternate_table);
    CHECK(parsed.eight_channel_alternate.main);
    CHECK(parsed.eight_channel_alternate.centre);
    CHECK(parsed.eight_channel_alternate.lfe);
    CHECK(parsed.eight_channel_alternate.surround);
    CHECK(parsed.eight_channel_alternate.top_side);
}

TEST_CASE("major_sync_info round trip: every sample rate code", "[mlp]") {
    for (const auto rate : {ac3::mlp::SampleRate::k48000, ac3::mlp::SampleRate::k96000,
                            ac3::mlp::SampleRate::k192000, ac3::mlp::SampleRate::k44100,
                            ac3::mlp::SampleRate::k88200, ac3::mlp::SampleRate::k176400}) {
        ac3::mlp::MajorSyncInfo info{};
        info.sample_rate = rate;
        const auto bytes = ac3::mlp::build_major_sync_info(info);
        ac3::mlp::MajorSyncInfo parsed{};
        REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));
        CHECK(parsed.sample_rate == rate);
    }
}

TEST_CASE("parse_major_sync_info rejects a corrupted CRC", "[mlp]") {
    const ac3::mlp::MajorSyncInfo info{};
    auto bytes = ac3::mlp::build_major_sync_info(info);
    bytes.back() ^= std::byte{0xFF};

    ac3::mlp::MajorSyncInfo parsed{};
    CHECK_FALSE(ac3::mlp::parse_major_sync_info(bytes, parsed));
}

TEST_CASE("parse_major_sync_info rejects a bad format_sync", "[mlp]") {
    auto bytes = to_bytes({0x00, 0x00, 0x00, 0x00});
    bytes.resize(28, std::byte{0});
    ac3::mlp::MajorSyncInfo parsed{};
    CHECK_FALSE(ac3::mlp::parse_major_sync_info(bytes, parsed));
}

TEST_CASE("is_restart_sync_word_valid: Table 20", "[mlp]") {
    CHECK(ac3::mlp::is_restart_sync_word_valid(0, ac3::mlp::kRestartSyncWordSubstream0));
    CHECK_FALSE(ac3::mlp::is_restart_sync_word_valid(0, ac3::mlp::kRestartSyncWordSubstream3));
    CHECK(ac3::mlp::is_restart_sync_word_valid(1, ac3::mlp::kRestartSyncWordSubstream0));
    CHECK(ac3::mlp::is_restart_sync_word_valid(1, ac3::mlp::kRestartSyncWordSubstream1Alt));
    CHECK(ac3::mlp::is_restart_sync_word_valid(2, ac3::mlp::kRestartSyncWordSubstream2));
    CHECK(ac3::mlp::is_restart_sync_word_valid(3, ac3::mlp::kRestartSyncWordSubstream3));
    CHECK_FALSE(ac3::mlp::is_restart_sync_word_valid(4, ac3::mlp::kRestartSyncWordSubstream0));
}

TEST_CASE("restart_header_crc: appending the CRC drives the register to zero", "[mlp]") {
    // Same property-based check as major_sync_crc's, generalised to a
    // non-byte-aligned bit count - the one restart_header_crc actually has
    // to handle correctly (see crc.hpp's comment on why).
    std::mt19937 rng(0x31EA31EB);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(1, 64);
    std::uniform_int_distribution<int> extra_bits_dist(0, 7);

    for (int trial = 0; trial < 100; ++trial) {
        std::vector<std::byte> msg(static_cast<std::size_t>(len_dist(rng)));
        for (auto& b : msg) {
            b = static_cast<std::byte>(byte_dist(rng));
        }
        const auto bit_count = msg.size() * 8 - static_cast<std::size_t>(extra_bits_dist(rng));
        const std::uint8_t crc = ac3::mlp::restart_header_crc(msg, bit_count);

        // Splice the CRC's 8 bits in immediately after bit_count, matching
        // what build_restart_header() does via put_bits()/put(), then
        // re-run the CRC over the extended span - it should land on zero.
        ac3::BitWriter w;
        w.put_bits(msg, bit_count);
        w.put(crc, 8);
        const auto with_crc = w.take();
        CHECK(ac3::mlp::restart_header_crc(with_crc, bit_count + 8) == 0x00);
    }
}

namespace {

ac3::mlp::RestartHeader make_restart_header(int substream_index, int max_matrix_chan) {
    ac3::mlp::RestartHeader header{};
    header.substream_index = substream_index;
    header.restart_sync_word = substream_index == 0   ? ac3::mlp::kRestartSyncWordSubstream0
                                : substream_index == 2 ? ac3::mlp::kRestartSyncWordSubstream2
                                                        : ac3::mlp::kRestartSyncWordSubstream3;
    header.output_timing = 12345;
    header.min_chan = 0;
    header.max_chan = static_cast<std::uint8_t>(max_matrix_chan);
    header.max_matrix_chan = static_cast<std::uint8_t>(max_matrix_chan);
    header.dither_shift = 5;
    header.dither_seed = 0x5A5A5A;
    header.max_shift = -3;
    header.max_lsbs = 7;
    header.max_bits_a = 20;
    header.max_bits_b = 24;
    header.error_protect = false;
    header.lossless_check = 0xB7;
    header.channel_assignment.resize(static_cast<std::size_t>(max_matrix_chan) + 1);
    for (std::size_t i = 0; i < header.channel_assignment.size(); ++i) {
        header.channel_assignment[i] = static_cast<std::uint8_t>(i);
    }
    return header;
}

}  // namespace

TEST_CASE("restart_header round trip: substream 0, single channel", "[mlp]") {
    const auto header = make_restart_header(0, 0);

    ac3::BitWriter w;
    const auto bits_written = ac3::mlp::build_restart_header(w, header);
    const auto bytes = w.take();
    CHECK(bits_written <= bytes.size() * 8);
    CHECK(bits_written > bytes.size() * 8 - 8);  // take() pads less than a byte

    ac3::mlp::RestartHeader parsed{};
    REQUIRE(ac3::mlp::parse_restart_header(bytes, 0, parsed));

    CHECK(parsed.restart_sync_word == header.restart_sync_word);
    CHECK(parsed.output_timing == header.output_timing);
    CHECK(parsed.max_matrix_chan == header.max_matrix_chan);
    CHECK(parsed.dither_seed == header.dither_seed);
    CHECK(parsed.max_shift == header.max_shift);
    CHECK(parsed.lossless_check == header.lossless_check);
    CHECK(parsed.channel_assignment == header.channel_assignment);
}

TEST_CASE("restart_header round trip: non-byte-aligned body (7 matrix channels)", "[mlp]") {
    // max_matrix_chan = 6 -> 7 ch_assign entries * 6 bits = 42 bits, which
    // (per crc.hpp's comment) does not land the trailing CRC on a byte
    // boundary - the case that actually exercises restart_header_crc's bit-
    // serial path and put_bits()'s partial-byte splicing.
    const auto header = make_restart_header(2, 6);

    ac3::BitWriter w;
    const auto bits_written = ac3::mlp::build_restart_header(w, header);
    CHECK(bits_written % 8 != 0);
    const auto bytes = w.take();

    ac3::mlp::RestartHeader parsed{};
    REQUIRE(ac3::mlp::parse_restart_header(bytes, 2, parsed));

    CHECK(parsed.max_matrix_chan == 6);
    CHECK(parsed.channel_assignment == header.channel_assignment);
    CHECK(parsed.max_bits_a == header.max_bits_a);
    CHECK(parsed.max_bits_b == header.max_bits_b);
    CHECK(parsed.dither_shift == header.dither_shift);
    CHECK(parsed.max_lsbs == header.max_lsbs);
}

TEST_CASE("restart_header round trip: every legal max_matrix_chan value", "[mlp]") {
    for (int max_matrix_chan = 0; max_matrix_chan <= 15; ++max_matrix_chan) {
        CAPTURE(max_matrix_chan);
        const auto header = make_restart_header(3, max_matrix_chan);

        ac3::BitWriter w;
        (void)ac3::mlp::build_restart_header(w, header);
        const auto bytes = w.take();

        ac3::mlp::RestartHeader parsed{};
        REQUIRE(ac3::mlp::parse_restart_header(bytes, 3, parsed));
        CHECK(parsed.channel_assignment == header.channel_assignment);
    }
}

TEST_CASE("parse_restart_header rejects a corrupted CRC", "[mlp]") {
    const auto header = make_restart_header(0, 3);
    ac3::BitWriter w;
    (void)ac3::mlp::build_restart_header(w, header);
    auto bytes = w.take();
    bytes.back() ^= std::byte{0xFF};

    ac3::mlp::RestartHeader parsed{};
    CHECK_FALSE(ac3::mlp::parse_restart_header(bytes, 0, parsed));
}

TEST_CASE("parse_restart_header rejects a substream mismatch", "[mlp]") {
    const auto header = make_restart_header(0, 3);
    ac3::BitWriter w;
    (void)ac3::mlp::build_restart_header(w, header);
    const auto bytes = w.take();

    ac3::mlp::RestartHeader parsed{};
    CHECK_FALSE(ac3::mlp::parse_restart_header(bytes, 2, parsed));
}

TEST_CASE("build_restart_header composes into an ongoing writer without extra padding", "[mlp]") {
    // The actual reason put_bits() exists: two structures back to back in
    // one writer - the way substream_directory's segments eventually will
    // be assembled - with no padding sneaking in between them. bit_count()
    // being exactly the sum of what each call reported is the property that
    // matters; whichever of the two ends up byte-aligned (not always the
    // first) is incidental to this particular pair of header shapes.
    const auto first = make_restart_header(0, 2);
    const auto second = make_restart_header(2, 1);

    ac3::BitWriter w;
    const auto first_bits = ac3::mlp::build_restart_header(w, first);
    const auto second_bits = ac3::mlp::build_restart_header(w, second);
    CHECK(w.bit_count() == first_bits + second_bits);

    const auto bytes = w.take();
    ac3::mlp::RestartHeader parsed_first{};
    REQUIRE(ac3::mlp::parse_restart_header(bytes, 0, parsed_first));
    CHECK(parsed_first.channel_assignment == first.channel_assignment);
}
