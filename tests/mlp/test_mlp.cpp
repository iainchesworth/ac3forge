#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include "ac3/emdf/emdf.hpp"
#include "ac3/mlp/block.hpp"
#include "ac3/mlp/crc.hpp"
#include "ac3/mlp/extra_data.hpp"
#include "ac3/mlp/huffman.hpp"
#include "ac3/mlp/matrix.hpp"
#include "ac3/mlp/mlp_tables.hpp"
#include "ac3/mlp/predictor.hpp"
#include "ac3/mlp/restart_header.hpp"
#include "ac3/mlp/rice.hpp"
#include "ac3/mlp/select.hpp"
#include "ac3/mlp/stream.hpp"
#include "ac3/mlp/sync.hpp"
#include "ac3/oba/oamd.hpp"

// The whole ac3::mlp surface: framing (mlp_sync, major_sync_info() with the
// 16ch extension, restart headers, the three CRC/parity schemes,
// terminators, EXTRA_DATA()), the block codec (B1 strip, DC offsets, the
// WO-Huffman entropy layer, predictors, the PMQ matrix), stream assembly
// with FIFO timing, automatic encoder selection, and the Atmos structural
// layer. The block-header field ORDER is this project's provisional
// packing - see docs/concepts/truehd-mlp.md.

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

// --- rice --------------------------------------------------------------

TEST_CASE("rice::zigzag round trip across the full int32_t range", "[mlp]") {
    for (const std::int32_t value :
         {0, 1, -1, 2, -2, 1000, -1000, std::numeric_limits<std::int32_t>::max(),
          std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::min() + 1}) {
        CAPTURE(value);
        CHECK(ac3::mlp::rice::zigzag_decode(ac3::mlp::rice::zigzag_encode(value)) == value);
    }

    std::mt19937 rng(0x7196);
    std::uniform_int_distribution<std::int32_t> dist(std::numeric_limits<std::int32_t>::min(),
                                                      std::numeric_limits<std::int32_t>::max());
    for (int trial = 0; trial < 500; ++trial) {
        const auto value = dist(rng);
        CHECK(ac3::mlp::rice::zigzag_decode(ac3::mlp::rice::zigzag_encode(value)) == value);
    }
}

TEST_CASE("rice::zigzag maps small magnitudes to small non-negative codes", "[mlp]") {
    // 0,-1,1,-2,2,... -> 0,1,2,3,4,... - the property that makes zigzag the
    // right map for a two-sided Laplacian source ahead of a code built for
    // magnitudes.
    CHECK(ac3::mlp::rice::zigzag_encode(0) == 0);
    CHECK(ac3::mlp::rice::zigzag_encode(-1) == 1);
    CHECK(ac3::mlp::rice::zigzag_encode(1) == 2);
    CHECK(ac3::mlp::rice::zigzag_encode(-2) == 3);
    CHECK(ac3::mlp::rice::zigzag_encode(2) == 4);
}

TEST_CASE("rice::encode/decode round trip for every k, many values", "[mlp]") {
    std::mt19937 rng(0x31EB);
    std::uniform_int_distribution<std::uint32_t> value_dist(0, 1u << 20);

    for (int k = 0; k <= 16; ++k) {
        CAPTURE(k);
        for (int trial = 0; trial < 50; ++trial) {
            const auto value = value_dist(rng);
            ac3::BitWriter w;
            ac3::mlp::rice::encode(w, value, k);
            CHECK(static_cast<int>(w.bit_count()) == ac3::mlp::rice::encoded_length(value, k));

            const auto bytes = w.take();
            ac3::BitReader r(bytes);
            CHECK(ac3::mlp::rice::decode(r, k) == value);
        }
    }
}

TEST_CASE("rice::encode: k=0 degenerates to plain unary", "[mlp]") {
    for (const std::uint32_t value : {0u, 1u, 5u, 20u}) {
        ac3::BitWriter w;
        ac3::mlp::rice::encode(w, value, 0);
        CHECK(w.bit_count() == value + 1);
    }
}

TEST_CASE("rice::encoded_length grows with k for small values, shrinks for large", "[mlp]") {
    // A large k wastes bits on a small value (the k-bit remainder field
    // dominates); a small k wastes bits on a large value (the unary
    // quotient dominates) - this is *why* MLP picks k adaptively per block.
    CHECK(ac3::mlp::rice::encoded_length(0, 8) > ac3::mlp::rice::encoded_length(0, 0));
    CHECK(ac3::mlp::rice::encoded_length(1u << 20, 0) > ac3::mlp::rice::encoded_length(1u << 20, 8));
}

// --- matrix --------------------------------------------------------------

TEST_CASE("matrix::quantize rounds half away from zero", "[mlp]") {
    CHECK(ac3::mlp::matrix::quantize(0, 4) == 0);
    CHECK(ac3::mlp::matrix::quantize(8, 4) == 1);    // exact
    CHECK(ac3::mlp::matrix::quantize(7, 4) == 0);    // rounds down (< half)
    CHECK(ac3::mlp::matrix::quantize(-7, 4) == 0);
    CHECK(ac3::mlp::matrix::quantize(9, 4) == 1);
    CHECK(ac3::mlp::matrix::quantize(-9, 4) == -1);
    CHECK(ac3::mlp::matrix::quantize(100, 0) == 100);  // shift 0 is a no-op
}

TEST_CASE("matrix: sum/difference rotation round trips", "[mlp]") {
    // JAES 2004 Sec. 4.1's own example: "the tendency of the matrix process
    // to rotate a stereo mix from left/right to sum/difference."
    // channel 0 <- L + R (sum), channel 1 stays R; a second step recovers L
    // = sum - R is implicit in the decode direction, this just exercises
    // one lifting step directly.
    const std::array<ac3::mlp::matrix::Step, 1> steps{
        {0, 0, {{1, 1}}}  // target=0 (L), shift=0, add 1*R
    };

    std::array<std::int64_t, 2> samples{100, 40};  // {L, R}
    ac3::mlp::matrix::encode_cascade(steps, samples);
    CHECK(samples[0] == 140);  // L' = L + R
    CHECK(samples[1] == 40);   // R unchanged

    ac3::mlp::matrix::decode_cascade(steps, samples);
    CHECK(samples[0] == 100);
    CHECK(samples[1] == 40);
}

TEST_CASE("matrix: random cascades round trip exactly", "[mlp]") {
    std::mt19937 rng(0x6D6C70);
    std::uniform_int_distribution<int> channel_count_dist(2, 6);
    std::uniform_int_distribution<std::int32_t> numerator_dist(-64, 64);
    std::uniform_int_distribution<int> shift_dist(0, 6);
    std::uniform_int_distribution<std::int64_t> sample_dist(-(1 << 20), 1 << 20);
    std::uniform_int_distribution<int> step_count_dist(1, 8);

    for (int trial = 0; trial < 200; ++trial) {
        const int channels = channel_count_dist(rng);
        const int step_count = step_count_dist(rng);

        std::vector<ac3::mlp::matrix::Step> steps;
        for (int s = 0; s < step_count; ++s) {
            ac3::mlp::matrix::Step step;
            step.target = s % channels;
            step.shift = shift_dist(rng);
            for (int ch = 0; ch < channels; ++ch) {
                if (ch != step.target) {
                    step.terms.emplace_back(ch, numerator_dist(rng));
                }
            }
            steps.push_back(std::move(step));
        }

        std::vector<std::int64_t> original(static_cast<std::size_t>(channels));
        for (auto& v : original) {
            v = sample_dist(rng);
        }

        auto samples = original;
        ac3::mlp::matrix::encode_cascade(steps, samples);
        ac3::mlp::matrix::decode_cascade(steps, samples);

        CHECK(samples == original);
    }
}

TEST_CASE("matrix: empty cascade is a no-op", "[mlp]") {
    std::array<std::int64_t, 3> samples{1, 2, 3};
    const std::array<ac3::mlp::matrix::Step, 0> steps{};
    ac3::mlp::matrix::encode_cascade(steps, samples);
    CHECK(samples == std::array<std::int64_t, 3>{1, 2, 3});
}

// --- huffman --------------------------------------------------------------

TEST_CASE("huffman::table2 exhaustive round trip with spec codeword lengths", "[mlp]") {
    // WO 96/37048 Table 2's own lengths: 8,8,7,6,5,4,3,2 for -7..0 and
    // 2,3,4,5,6,7,8,8 for 1..8.
    constexpr std::array<int, 16> kLengths{8, 8, 7, 6, 5, 4, 3, 2, 2, 3, 4, 5, 6, 7, 8, 8};
    for (int value = ac3::mlp::huffman::kTable2Min; value <= ac3::mlp::huffman::kTable2Max;
         ++value) {
        CAPTURE(value);
        ac3::BitWriter w;
        ac3::mlp::huffman::encode_table2(w, value);
        CHECK(static_cast<int>(w.bit_count()) ==
              kLengths[static_cast<std::size_t>(value + 7)]);
        CHECK(static_cast<int>(w.bit_count()) == ac3::mlp::huffman::table2_length(value));

        const auto bytes = w.take();
        ac3::BitReader r(bytes);
        CHECK(ac3::mlp::huffman::decode_table2(r) == value);
    }
}

TEST_CASE("huffman::table2 decodes back-to-back sequences (prefix-free in practice)", "[mlp]") {
    // Sequential decode of a concatenated stream is the property
    // prefix-freeness exists to provide - test it directly.
    std::mt19937 rng(0x1996);
    std::uniform_int_distribution<int> dist(ac3::mlp::huffman::kTable2Min,
                                            ac3::mlp::huffman::kTable2Max);
    std::vector<int> values(500);
    ac3::BitWriter w;
    for (auto& v : values) {
        v = dist(rng);
        ac3::mlp::huffman::encode_table2(w, v);
    }
    const auto bytes = w.take();
    ac3::BitReader r(bytes);
    for (const auto v : values) {
        REQUIRE(ac3::mlp::huffman::decode_table2(r) == v);
    }
}

TEST_CASE("huffman::significant word round trip across all seventeen tables", "[mlp]") {
    std::mt19937 rng(0x37048);
    for (int n = ac3::mlp::huffman::kMinN; n <= ac3::mlp::huffman::kMaxN; ++n) {
        CAPTURE(n);
        const std::int32_t lo = -(std::int32_t{1} << n) + 1;
        const std::int32_t hi = std::int32_t{1} << n;
        std::uniform_int_distribution<std::int32_t> dist(lo, hi);

        std::vector<std::int32_t> values{lo, 0, hi};  // boundaries first
        for (int i = 0; i < 100; ++i) {
            values.push_back(dist(rng));
        }

        ac3::BitWriter w;
        for (const auto v : values) {
            ac3::mlp::huffman::encode_significant(w, v, n);
        }
        const auto bytes = w.take();
        ac3::BitReader r(bytes);
        for (const auto v : values) {
            REQUIRE(ac3::mlp::huffman::decode_significant(r, n) == v);
        }
    }
}

TEST_CASE("huffman::significant_length matches what encode actually writes", "[mlp]") {
    std::mt19937 rng(0x2026);
    for (const int n : {3, 7, 12, 19}) {
        std::uniform_int_distribution<std::int32_t> dist(-(std::int32_t{1} << n) + 1,
                                                          std::int32_t{1} << n);
        for (int i = 0; i < 50; ++i) {
            const auto v = dist(rng);
            CAPTURE(n, v);
            ac3::BitWriter w;
            ac3::mlp::huffman::encode_significant(w, v, n);
            CHECK(static_cast<int>(w.bit_count()) ==
                  ac3::mlp::huffman::significant_length(v, n));
        }
    }
}

TEST_CASE("huffman::select_n picks the smallest fitting table", "[mlp]") {
    CHECK(ac3::mlp::huffman::select_n(-7, 8) == 3);
    CHECK(ac3::mlp::huffman::select_n(0, 8) == 3);
    CHECK(ac3::mlp::huffman::select_n(0, 9) == 4);   // 9 > 2^3
    CHECK(ac3::mlp::huffman::select_n(-8, 0) == 4);  // -8 < -2^3+1
    CHECK(ac3::mlp::huffman::select_n(-15, 16) == 4);
    CHECK(ac3::mlp::huffman::select_n(-524287, 524288) == 19);
}

TEST_CASE("huffman::small tables round trip", "[mlp]") {
    struct Range {
        std::span<const ac3::mlp::huffman::SmallCode> table;
        int lo;
        int hi;
    };
    const std::array<Range, 3> kRanges{{
        {ac3::mlp::huffman::kTable4, -1, 2},
        {ac3::mlp::huffman::kTable5, -2, 2},
        {ac3::mlp::huffman::kTable6, -3, 3},
    }};

    std::mt19937 rng(0xB752);
    for (const auto& range : kRanges) {
        std::uniform_int_distribution<int> dist(range.lo, range.hi);
        std::vector<int> values(200);
        ac3::BitWriter w;
        for (auto& v : values) {
            v = dist(rng);
            ac3::mlp::huffman::encode_small(w, range.table, v);
        }
        const auto bytes = w.take();
        ac3::BitReader r(bytes);
        for (const auto v : values) {
            REQUIRE(ac3::mlp::huffman::decode_small(r, range.table) == v);
        }
    }
}

TEST_CASE("huffman::pcm round trip, including widths beyond Table 3's cap", "[mlp]") {
    std::mt19937 rng(0xF872);
    for (const int n : {1, 5, 19, 23}) {
        CAPTURE(n);
        const std::int32_t lo = -(std::int32_t{1} << n) + 1;
        const std::int32_t hi = std::int32_t{1} << n;
        std::uniform_int_distribution<std::int32_t> dist(lo, hi);
        for (int i = 0; i < 50; ++i) {
            const auto v = i < 2 ? (i == 0 ? lo : hi) : dist(rng);
            ac3::BitWriter w;
            ac3::mlp::huffman::encode_pcm(w, v, n);
            CHECK(static_cast<int>(w.bit_count()) == n + 1);
            const auto bytes = w.take();
            ac3::BitReader r(bytes);
            CHECK(ac3::mlp::huffman::decode_pcm(r, n) == v);
        }
    }
}

// --- predictor ------------------------------------------------------------

namespace {

std::vector<std::int32_t> random_samples(std::mt19937& rng, std::size_t count, int bits) {
    std::uniform_int_distribution<std::int32_t> dist(-(std::int32_t{1} << (bits - 1)),
                                                     (std::int32_t{1} << (bits - 1)) - 1);
    std::vector<std::int32_t> out(count);
    for (auto& v : out) {
        v = dist(rng);
    }
    return out;
}

void check_predictor_round_trip(const ac3::mlp::PredictorCoefficients& coefficients,
                                std::span<const std::int32_t> samples) {
    ac3::mlp::PredictorState encode_state{};
    ac3::mlp::PredictorState decode_state{};

    std::vector<std::int32_t> residual(samples.size());
    ac3::mlp::predict_encode(coefficients, samples, residual, encode_state);

    std::vector<std::int32_t> reconstructed(samples.size());
    ac3::mlp::predict_decode(coefficients, residual, reconstructed, decode_state);

    REQUIRE(std::equal(samples.begin(), samples.end(), reconstructed.begin()));
    // The two states must also agree exactly - that is what makes
    // block-to-block continuation lossless without re-initialisation.
    CHECK(encode_state.input == decode_state.input);
    CHECK(encode_state.output == decode_state.output);
}

}  // namespace

TEST_CASE("predictor: every WO Table 1 preset round trips on 24-bit audio", "[mlp]") {
    std::mt19937 rng(0x441);
    const auto samples = random_samples(rng, 576, 24);  // the WO's example block length
    for (int preset = 0; preset < ac3::mlp::kTable1Cases; ++preset) {
        CAPTURE(preset);
        check_predictor_round_trip(ac3::mlp::table1_preset(preset), samples);
    }
}

TEST_CASE("predictor: random FIR-only coefficients up to 8th order round trip", "[mlp]") {
    // Arbitrary A with empty B cannot blow up (no feedback), so the whole
    // coefficient space is safely testable here.
    std::mt19937 rng(0x1164);
    std::uniform_int_distribution<std::int32_t> coeff_dist(-192, 192);
    std::uniform_int_distribution<std::size_t> order_dist(1, 8);

    for (int trial = 0; trial < 50; ++trial) {
        ac3::mlp::PredictorCoefficients coefficients;
        coefficients.shift = 6;  // m/64
        coefficients.a.resize(order_dist(rng));
        for (auto& c : coefficients.a) {
            c = coeff_dist(rng);
        }
        const auto samples = random_samples(rng, 200, 20);
        check_predictor_round_trip(coefficients, samples);
    }
}

TEST_CASE("predictor: a stable IIR filter round trips", "[mlp]") {
    // Mild feedback (well inside stability) exercises the recursive path
    // with random data without risking residual blow-up.
    ac3::mlp::PredictorCoefficients coefficients;
    coefficients.shift = 6;
    coefficients.a = {-64, 32};  // -1.0, +0.5 in m/64
    coefficients.b = {32, -16};  // +0.5, -0.25

    std::mt19937 rng(0x31EA);
    const auto samples = random_samples(rng, 1000, 24);
    check_predictor_round_trip(coefficients, samples);
}

TEST_CASE("predictor: state carries across consecutive blocks", "[mlp]") {
    // Encoding one long run must equal encoding two half runs with the
    // state carried over - the property the WO's short-header/repeat
    // mechanism (and MLP's restart intervals) rely on.
    const auto coefficients = ac3::mlp::table1_preset(5);
    std::mt19937 rng(0x0B77);
    const auto samples = random_samples(rng, 400, 24);

    std::vector<std::int32_t> one_shot(samples.size());
    {
        ac3::mlp::PredictorState state{};
        ac3::mlp::predict_encode(coefficients, samples, one_shot, state);
    }

    std::vector<std::int32_t> split(samples.size());
    {
        ac3::mlp::PredictorState state{};
        const std::span<const std::int32_t> all{samples};
        const std::span<std::int32_t> out{split};
        ac3::mlp::predict_encode(coefficients, all.first(200), out.first(200), state);
        ac3::mlp::predict_encode(coefficients, all.subspan(200), out.subspan(200), state);
    }

    CHECK(one_shot == split);
}

TEST_CASE("predictor: seeded history reproduces mid-stream decode (restart semantics)", "[mlp]") {
    // A decoder joining mid-stream with transmitted initialisation data
    // must reconstruct exactly - seed a fresh state from the encoder side
    // and decode only the tail.
    const auto coefficients = ac3::mlp::table1_preset(2);
    std::mt19937 rng(0x5838);
    const auto samples = random_samples(rng, 300, 24);

    ac3::mlp::PredictorState encode_state{};
    std::vector<std::int32_t> residual(samples.size());
    ac3::mlp::predict_encode(coefficients, samples, residual, encode_state);

    // Re-encode just the head to capture the state at the split point.
    ac3::mlp::PredictorState mid_state{};
    std::vector<std::int32_t> head(150);
    ac3::mlp::predict_encode(coefficients, std::span{samples}.first(150), head, mid_state);

    // Seed a decoder with that state (what restart initialisation data is
    // for) and decode only the tail.
    std::vector<std::int32_t> tail(150);
    ac3::mlp::predict_decode(coefficients, std::span{residual}.subspan(150), tail, mid_state);

    CHECK(std::equal(tail.begin(), tail.end(), samples.begin() + 150));
}

// --- block codec ----------------------------------------------------------

namespace {

void check_block_round_trip(std::span<const std::int32_t> samples, int wordlength,
                            const ac3::mlp::PredictorCoefficients& coefficients) {
    ac3::BitWriter w;
    ac3::mlp::encode_block(w, samples, wordlength, coefficients);
    const auto bytes = w.take();

    std::vector<std::int32_t> decoded(samples.size());
    ac3::BitReader r(bytes);
    REQUIRE(ac3::mlp::decode_block(r, wordlength, decoded));
    REQUIRE(std::equal(samples.begin(), samples.end(), decoded.begin()));
}

}  // namespace

TEST_CASE("block: random 24-bit audio round trips with every Table 1 preset", "[mlp]") {
    std::mt19937 rng(0x18A);
    const auto samples = random_samples(rng, 576, 24);
    for (int preset = 0; preset < ac3::mlp::kTable1Cases; ++preset) {
        CAPTURE(preset);
        check_block_round_trip(samples, 24, ac3::mlp::table1_preset(preset));
    }
}

TEST_CASE("block: assorted block lengths and wordlengths round trip", "[mlp]") {
    std::mt19937 rng(0x18B);
    const auto coefficients = ac3::mlp::table1_preset(1);
    for (const auto [length, bits] : {std::pair{40, 16}, {160, 20}, {576, 24}, {1536, 24}}) {
        CAPTURE(length, bits);
        const auto samples = random_samples(rng, static_cast<std::size_t>(length), bits);
        check_block_round_trip(samples, bits == 16 ? 20 : bits, coefficients);
    }
}

TEST_CASE("block: digital black takes the empty table and stays tiny", "[mlp]") {
    const std::vector<std::int32_t> silence(576, 0);
    ac3::BitWriter w;
    ac3::mlp::encode_block(w, silence, 24, ac3::mlp::table1_preset(1));
    const auto bytes = w.take();
    // WO: an empty-table block conveys "no data at all in the Huffman coded
    // data part" and omits coefficients and initialisation - just the
    // 2+5+24-bit header stub, well under a handful of bytes.
    CHECK(bytes.size() <= 4);

    std::vector<std::int32_t> decoded(576, -1);
    ac3::BitReader r(bytes);
    REQUIRE(ac3::mlp::decode_block(r, 24, decoded));
    CHECK(std::all_of(decoded.begin(), decoded.end(),
                      [](std::int32_t v) { return v == 0; }));
}

TEST_CASE("block: constant low bits are stripped into the LSB word", "[mlp]") {
    // 16-bit audio carried in 24-bit words: low 8 bits are a constant zero
    // pattern, which B1 detection should strip. Compare against the same
    // signal without padding: the padded encoding should cost no more than
    // a few header bits extra.
    std::mt19937 rng(0x18C);
    const auto narrow = random_samples(rng, 400, 16);
    std::vector<std::int32_t> padded(narrow.size());
    for (std::size_t i = 0; i < narrow.size(); ++i) {
        padded[i] = narrow[i] << 8;
    }

    const auto coefficients = ac3::mlp::table1_preset(2);
    check_block_round_trip(padded, 24, coefficients);

    ac3::BitWriter w_narrow;
    ac3::mlp::encode_block(w_narrow, narrow, 16, coefficients);
    ac3::BitWriter w_padded;
    ac3::mlp::encode_block(w_padded, padded, 24, coefficients);
    CHECK(w_padded.bit_count() <= w_narrow.bit_count() + 16);
}

TEST_CASE("block: nonzero constant LSB pattern round trips", "[mlp]") {
    std::mt19937 rng(0x18D);
    const auto narrow = random_samples(rng, 200, 12);
    std::vector<std::int32_t> shifted(narrow.size());
    for (std::size_t i = 0; i < narrow.size(); ++i) {
        shifted[i] = (narrow[i] << 4) | 0b0110;  // constant nonzero low bits
    }
    check_block_round_trip(shifted, 20, ac3::mlp::table1_preset(3));
}

TEST_CASE("block: a smooth signal compresses well below PCM", "[mlp]") {
    // A gently-rising ramp from zero is highly predictable; with a
    // first-order difference the residual is a small constant, so the coded
    // size should land far under wordlength bits/sample. (A large starting
    // offset no longer spoils this: the LSB word's DC slot absorbs it - see
    // "block: a DC pedestal costs nothing" below.)
    std::vector<std::int32_t> samples(576);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int32_t>(i) * 3;
    }
    ac3::mlp::PredictorCoefficients diff;
    diff.shift = 0;
    diff.a = {-1};  // first-order difference
    check_block_round_trip(samples, 24, diff);

    ac3::BitWriter w;
    ac3::mlp::encode_block(w, samples, 24, diff);
    CHECK(w.bit_count() < samples.size() * 24 / 3);
}

TEST_CASE("block: full-scale residuals fall back to PCM and round trip", "[mlp]") {
    // Alternating near-full-scale 24-bit values through a passthrough
    // predictor leave residuals wider than Table 3's cap - the PCM path.
    std::vector<std::int32_t> samples(64);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = (i % 2 == 0) ? 8388000 : -8388000;
    }
    check_block_round_trip(samples, 24, ac3::mlp::PredictorCoefficients{.shift = 0});
}

TEST_CASE("block: header pack/parse round trips and rejects corruption", "[mlp]") {
    ac3::mlp::BlockHeader header;
    header.coding = ac3::mlp::BlockCoding::kSignificant;
    header.n = 9;
    header.b1 = 4;
    header.lsb_word = 0b1010;
    header.coefficients = ac3::mlp::table1_preset(6);
    header.init = {123, -456};

    ac3::BitWriter w;
    ac3::mlp::build_block_header(w, header, 24);
    const auto bytes = w.take();

    ac3::mlp::BlockHeader parsed;
    {
        ac3::BitReader r(bytes);
        REQUIRE(ac3::mlp::parse_block_header(r, 24, parsed));
    }
    CHECK(parsed.coding == header.coding);
    CHECK(parsed.n == header.n);
    CHECK(parsed.b1 == header.b1);
    CHECK(parsed.lsb_word == header.lsb_word);
    CHECK(parsed.coefficients.shift == header.coefficients.shift);
    CHECK(parsed.coefficients.a == header.coefficients.a);
    CHECK(parsed.coefficients.b == header.coefficients.b);
    CHECK(parsed.init == header.init);

    // Corrupt the coding field to the reserved value (0b11): reject.
    auto corrupt = bytes;
    corrupt[0] |= std::byte{0xC0};
    ac3::BitReader r(corrupt);
    CHECK_FALSE(ac3::mlp::parse_block_header(r, 24, parsed));
}

// --- stream assembler -----------------------------------------------------

TEST_CASE("stream: multi-AU round trip with periodic major sync", "[mlp]") {
    ac3::mlp::StreamConfig config;
    config.sample_rate = ac3::mlp::SampleRate::k48000;
    config.wordlength = 24;
    config.major_sync_interval = 8;
    config.coefficients = {ac3::mlp::table1_preset(1)};

    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder(24);

    std::mt19937 rng(0x600D);
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(config.sample_rate));
    CHECK(frame == 40);

    for (int au = 0; au < 20; ++au) {
        CAPTURE(au);
        const auto samples = random_samples(rng, frame, 24);
        const auto bytes = encoder.encode_access_unit(samples);

        // access_unit_length is the whole unit in 16-bit words (§4.1.2).
        const auto length_words =
            ((std::to_integer<std::uint32_t>(bytes[0]) & 0xF) << 8) |
            std::to_integer<std::uint32_t>(bytes[1]);
        CHECK(static_cast<std::size_t>(length_words) * 2 == bytes.size());

        // §4.1.1: the XOR of mlp_sync's eight nibbles is 0xF.
        std::uint8_t nibbles = 0;
        for (int i = 0; i < 4; ++i) {
            nibbles ^= std::to_integer<std::uint8_t>(bytes[static_cast<std::size_t>(i)]) >> 4;
            nibbles ^= std::to_integer<std::uint8_t>(bytes[static_cast<std::size_t>(i)]) & 0xF;
        }
        CHECK(nibbles == 0xF);

        // input_timing advances by one frame per AU, mod 65536 (§4.1.3),
        // riding kFifoDelayFrames ahead of presentation time: these small
        // access units never disturb the constant wound-up FIFO delay, so
        // delivery stays exactly one full delay early (§2.7).
        const auto timing = (std::to_integer<std::uint32_t>(bytes[2]) << 8) |
                            std::to_integer<std::uint32_t>(bytes[3]);
        const auto delay =
            static_cast<std::uint32_t>(ac3::mlp::kFifoDelayFrames) * frame;
        CHECK(timing ==
              ((static_cast<std::uint32_t>(au) * frame + 65536 - delay) & 0xFFFF));

        // §3.1/§2.5: format_sync at bit offset 32 on major-sync AUs only -
        // the first AU and every eighth after it.
        const bool expect_major = au % 8 == 0;
        const bool is_major = std::to_integer<std::uint8_t>(bytes[4]) == 0xF8 &&
                              std::to_integer<std::uint8_t>(bytes[5]) == 0x72 &&
                              std::to_integer<std::uint8_t>(bytes[6]) == 0x6F &&
                              std::to_integer<std::uint8_t>(bytes[7]) == 0xBA;
        CHECK(is_major == expect_major);

        std::vector<std::int32_t> decoded;
        REQUIRE(decoder.decode_access_unit(bytes, decoded));
        REQUIRE(std::equal(samples.begin(), samples.end(), decoded.begin()));
    }
    CHECK(decoder.sample_rate() == ac3::mlp::SampleRate::k48000);
}

TEST_CASE("stream: every sample rate frames and round trips", "[mlp]") {
    std::mt19937 rng(0x48F);
    for (const auto rate :
         {ac3::mlp::SampleRate::k48000, ac3::mlp::SampleRate::k96000,
          ac3::mlp::SampleRate::k192000, ac3::mlp::SampleRate::k44100,
          ac3::mlp::SampleRate::k88200, ac3::mlp::SampleRate::k176400}) {
        ac3::mlp::StreamConfig config;
        config.sample_rate = rate;
        config.coefficients = {ac3::mlp::table1_preset(2)};
        ac3::mlp::StreamEncoder encoder(config);
        ac3::mlp::StreamDecoder decoder(24);

        const auto frame =
            static_cast<std::size_t>(ac3::mlp::samples_per_access_unit(rate));
        for (int au = 0; au < 3; ++au) {
            const auto samples = random_samples(rng, frame, 24);
            const auto bytes = encoder.encode_access_unit(samples);
            std::vector<std::int32_t> decoded;
            REQUIRE(decoder.decode_access_unit(bytes, decoded));
            REQUIRE(std::equal(samples.begin(), samples.end(), decoded.begin()));
        }
        CHECK(decoder.sample_rate() == rate);
    }
}

TEST_CASE("stream: decoding must start at a major sync", "[mlp]") {
    ac3::mlp::StreamConfig config;
    config.coefficients = {ac3::mlp::table1_preset(1)};
    ac3::mlp::StreamEncoder encoder(config);

    std::mt19937 rng(0xDEC0);
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(config.sample_rate));
    const auto first = encoder.encode_access_unit(random_samples(rng, frame, 24));
    const auto samples = random_samples(rng, frame, 24);
    const auto second = encoder.encode_access_unit(samples);

    std::vector<std::int32_t> decoded;

    // A fresh decoder fed the minor AU first has no stream context: reject.
    ac3::mlp::StreamDecoder cold(24);
    CHECK_FALSE(cold.decode_access_unit(second, decoded));

    // Fed in order, both decode.
    ac3::mlp::StreamDecoder warm(24);
    REQUIRE(warm.decode_access_unit(first, decoded));
    REQUIRE(warm.decode_access_unit(second, decoded));
    CHECK(std::equal(samples.begin(), samples.end(), decoded.begin()));
}

TEST_CASE("stream: corruption anywhere is detected", "[mlp]") {
    ac3::mlp::StreamConfig config;
    config.coefficients = {ac3::mlp::table1_preset(3)};
    ac3::mlp::StreamEncoder encoder(config);

    std::mt19937 rng(0xBAD);
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(config.sample_rate));
    const auto bytes = encoder.encode_access_unit(random_samples(rng, frame, 24));

    for (const std::size_t position : {std::size_t{0}, std::size_t{2}, std::size_t{5},
                                       bytes.size() / 2, bytes.size() - 1}) {
        CAPTURE(position);
        auto corrupt = bytes;
        corrupt[position] ^= std::byte{0x40};
        ac3::mlp::StreamDecoder decoder(24);
        std::vector<std::int32_t> decoded;
        CHECK_FALSE(decoder.decode_access_unit(corrupt, decoded));
    }

    // Truncation is also caught (the length field no longer matches).
    auto truncated = bytes;
    truncated.resize(bytes.size() - 2);
    ac3::mlp::StreamDecoder decoder(24);
    std::vector<std::int32_t> decoded;
    CHECK_FALSE(decoder.decode_access_unit(truncated, decoded));
}

TEST_CASE("stream: digital-black access units stay compact", "[mlp]") {
    ac3::mlp::StreamConfig config;
    config.coefficients = {ac3::mlp::table1_preset(1)};
    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder(24);

    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(config.sample_rate));
    const std::vector<std::int32_t> silence(frame, 0);

    const auto major = encoder.encode_access_unit(silence);
    const auto minor = encoder.encode_access_unit(silence);

    // Minor silent AU: 4 sync + 2 directory + tiny empty-coded segment + 2
    // parity/CRC - comfortably under 16 bytes.
    CHECK(minor.size() <= 16);
    CHECK(major.size() > minor.size());  // the 28-byte major sync + restart header

    std::vector<std::int32_t> decoded;
    REQUIRE(decoder.decode_access_unit(major, decoded));
    CHECK(std::all_of(decoded.begin(), decoded.end(),
                      [](std::int32_t v) { return v == 0; }));
    REQUIRE(decoder.decode_access_unit(minor, decoded));
    CHECK(std::all_of(decoded.begin(), decoded.end(),
                      [](std::int32_t v) { return v == 0; }));
}

// --- multichannel blocks + matrix wiring ----------------------------------

namespace {

std::vector<std::vector<std::int32_t>> random_channels(std::mt19937& rng, std::size_t count,
                                                       std::size_t length, int bits) {
    std::vector<std::vector<std::int32_t>> out(count);
    for (auto& channel : out) {
        channel = random_samples(rng, length, bits);
    }
    return out;
}

std::vector<std::span<const std::int32_t>> const_spans(
    const std::vector<std::vector<std::int32_t>>& channels) {
    std::vector<std::span<const std::int32_t>> out;
    out.reserve(channels.size());
    for (const auto& channel : channels) {
        out.emplace_back(channel);
    }
    return out;
}

void check_multichannel_block_round_trip(const std::vector<std::vector<std::int32_t>>& channels,
                                         int wordlength,
                                         const ac3::mlp::MultichannelBlockConfig& config) {
    const auto spans = const_spans(channels);
    ac3::BitWriter w;
    ac3::mlp::encode_block_channels(
        w, std::span<const std::span<const std::int32_t>>{spans}, wordlength, config);
    const auto bytes = w.take();

    std::vector<std::vector<std::int32_t>> decoded(channels.size());
    std::vector<std::span<std::int32_t>> out_spans;
    for (auto& channel : decoded) {
        channel.resize(channels[0].size());
        out_spans.emplace_back(channel);
    }
    ac3::BitReader r(bytes);
    REQUIRE(ac3::mlp::decode_block_channels(
        r, wordlength, std::span<const std::span<std::int32_t>>{out_spans}));
    REQUIRE(decoded == channels);
}

}  // namespace

TEST_CASE("multichannel block: stereo sum/difference matrix round trips", "[mlp]") {
    std::mt19937 rng(0x2C44);
    auto channels = random_channels(rng, 2, 160, 24);

    ac3::mlp::MultichannelBlockConfig config;
    // A lifting-style rotation toward mid/side: ch1 -= ch0, then
    // ch0 += ch1/2 - the JAES paper's "rotate a stereo mix from left/right
    // to sum/difference" as two primitive steps.
    config.steps = {
        {1, 0, {{0, -1}}},
        {0, 1, {{1, 1}}},
    };
    config.coefficients = {ac3::mlp::table1_preset(1), ac3::mlp::table1_preset(1)};
    check_multichannel_block_round_trip(channels, 24, config);
}

TEST_CASE("multichannel block: six channels with a random cascade round trip", "[mlp]") {
    std::mt19937 rng(0x6C11);
    std::uniform_int_distribution<std::int32_t> coeff_dist(-64, 64);

    for (int trial = 0; trial < 10; ++trial) {
        auto channels = random_channels(rng, 6, 160, 20);

        ac3::mlp::MultichannelBlockConfig config;
        for (int s = 0; s < 4; ++s) {
            ac3::mlp::matrix::Step step;
            step.target = s % 6;
            step.shift = 4;
            for (int c = 0; c < 6; ++c) {
                if (c != step.target) {
                    step.terms.emplace_back(c, coeff_dist(rng));
                }
            }
            config.steps.push_back(std::move(step));
        }
        for (int c = 0; c < 6; ++c) {
            config.coefficients.push_back(ac3::mlp::table1_preset(c % ac3::mlp::kTable1Cases));
        }
        check_multichannel_block_round_trip(channels, 20, config);
    }
}

TEST_CASE("multichannel block: decorrelating matrix beats no matrix on correlated input",
          "[mlp]") {
    // Two nearly-identical channels: subtracting one from the other leaves
    // a small difference signal, which must code smaller than coding both
    // full-strength channels - the entire reason the matrix stage exists
    // (WO: "the encoded data rate is minimised by reducing commonality
    // between channels").
    std::mt19937 rng(0xC0);
    const auto base = random_samples(rng, 576, 20);
    std::uniform_int_distribution<std::int32_t> noise(-15, 16);
    std::vector<std::vector<std::int32_t>> channels(2);
    channels[0] = base;
    channels[1].resize(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        channels[1][i] = base[i] + noise(rng);
    }
    const auto spans = const_spans(channels);

    ac3::mlp::MultichannelBlockConfig without;
    without.coefficients = {ac3::mlp::PredictorCoefficients{}, ac3::mlp::PredictorCoefficients{}};

    ac3::mlp::MultichannelBlockConfig with = without;
    with.steps = {{1, 0, {{0, -1}}}};  // ch1 -= ch0

    ac3::BitWriter w_without;
    ac3::mlp::encode_block_channels(
        w_without, std::span<const std::span<const std::int32_t>>{spans}, 24, without);
    ac3::BitWriter w_with;
    ac3::mlp::encode_block_channels(
        w_with, std::span<const std::span<const std::int32_t>>{spans}, 24, with);

    CHECK(w_with.bit_count() < w_without.bit_count());
    check_multichannel_block_round_trip(channels, 24, with);
}

TEST_CASE("multichannel block: a silent channel takes the empty coding", "[mlp]") {
    std::mt19937 rng(0x510);
    std::vector<std::vector<std::int32_t>> channels(3);
    channels[0] = random_samples(rng, 160, 24);
    channels[1].assign(160, 0);  // digital black
    channels[2] = random_samples(rng, 160, 24);

    ac3::mlp::MultichannelBlockConfig config;
    config.coefficients = {ac3::mlp::table1_preset(1), ac3::mlp::table1_preset(1),
                           ac3::mlp::table1_preset(1)};
    check_multichannel_block_round_trip(channels, 24, config);
}

TEST_CASE("stream: six-channel stream with matrix round trips, channel count in-band", "[mlp]") {
    ac3::mlp::StreamConfig config;
    config.sample_rate = ac3::mlp::SampleRate::k48000;
    config.channels = 6;
    config.matrix = {
        {1, 0, {{0, -1}}},  // R -= L
        {4, 0, {{2, -1}}},  // Rs -= Ls (channel indices 2/4 arbitrary here)
    };
    for (int c = 0; c < 6; ++c) {
        config.coefficients.push_back(ac3::mlp::table1_preset(1 + c % 3));
    }

    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder(24);

    std::mt19937 rng(0x51C);
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(config.sample_rate));

    for (int au = 0; au < 10; ++au) {
        CAPTURE(au);
        const auto channels = random_channels(rng, 6, frame, 24);
        const auto spans = const_spans(channels);
        const auto bytes = encoder.encode_access_unit(
            std::span<const std::span<const std::int32_t>>{spans});

        std::vector<std::vector<std::int32_t>> decoded;
        REQUIRE(decoder.decode_access_unit(bytes, decoded));
        REQUIRE(decoded == channels);
    }
    CHECK(decoder.channels() == 6);
}

TEST_CASE("stream: the single-channel convenience API remains a one-channel stream", "[mlp]") {
    ac3::mlp::StreamConfig config;
    config.coefficients = {ac3::mlp::table1_preset(2)};

    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder(24);

    std::mt19937 rng(0x1C11);
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(config.sample_rate));
    const auto samples = random_samples(rng, frame, 24);
    const auto bytes = encoder.encode_access_unit(std::span<const std::int32_t>{samples});

    std::vector<std::int32_t> decoded;
    REQUIRE(decoder.decode_access_unit(bytes, decoded));
    CHECK(decoded == samples);
    CHECK(decoder.channels() == 1);
}

// --- encoder selection -----------------------------------------------------

TEST_CASE("select: a smooth ramp picks a differencing predictor", "[mlp]") {
    std::vector<std::int32_t> ramp(576);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<std::int32_t>(i) * 5;
    }
    const auto chosen = ac3::mlp::select::choose_predictor(ramp);
    // Whatever it picked, it must beat passthrough on the exact cost measure.
    CHECK_FALSE(chosen.a.empty());
}

TEST_CASE("select: white noise picks passthrough (nothing to predict)", "[mlp]") {
    std::mt19937 rng(0x5E11);
    const auto noise = random_samples(rng, 576, 20);
    const auto chosen = ac3::mlp::select::choose_predictor(noise);
    // Full-scale white noise is unpredictable; the palette's differencing
    // and IIR candidates all AMPLIFY it, so passthrough must win.
    CHECK(chosen.a.empty());
    CHECK(chosen.b.empty());
}

TEST_CASE("select: correlated channels get a decorrelating step", "[mlp]") {
    std::mt19937 rng(0x5E12);
    const auto base = random_samples(rng, 576, 20);
    std::uniform_int_distribution<std::int32_t> noise(-15, 16);
    std::vector<std::vector<std::int32_t>> channels(2);
    channels[0] = base;
    channels[1].resize(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        channels[1][i] = base[i] + noise(rng);
    }
    const auto spans = const_spans(channels);
    const auto steps = ac3::mlp::select::choose_matrix(
        std::span<const std::span<const std::int32_t>>{spans});
    REQUIRE_FALSE(steps.empty());
}

TEST_CASE("select: independent channels get no matrix", "[mlp]") {
    std::mt19937 rng(0x5E13);
    auto channels = random_channels(rng, 2, 576, 20);
    const auto spans = const_spans(channels);
    const auto steps = ac3::mlp::select::choose_matrix(
        std::span<const std::span<const std::int32_t>>{spans});
    CHECK(steps.empty());
}

TEST_CASE("stream: automatic mode round trips and beats the naive config", "[mlp]") {
    // Correlated stereo, gently band-limited so prediction has something to
    // do: automatic selection must round-trip exactly AND produce smaller
    // access units than passthrough-everything.
    std::mt19937 rng(0xA07);
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));

    // A wandering-walk base signal (band-limited-ish), second channel
    // correlated with it.
    std::vector<std::vector<std::int32_t>> channels(2, std::vector<std::int32_t>(frame));
    std::uniform_int_distribution<std::int32_t> walk(-4000, 4000);
    std::uniform_int_distribution<std::int32_t> noise(-63, 64);
    std::int32_t value = 0;
    for (std::size_t i = 0; i < frame; ++i) {
        value += walk(rng);
        value = std::clamp(value, -4000000, 4000000);
        channels[0][i] = value;
        channels[1][i] = value + noise(rng);
    }
    const auto spans = const_spans(channels);

    ac3::mlp::StreamConfig automatic;
    automatic.channels = 2;
    automatic.automatic = true;
    ac3::mlp::StreamEncoder auto_encoder(automatic);

    ac3::mlp::StreamConfig naive;
    naive.channels = 2;
    ac3::mlp::StreamEncoder naive_encoder(naive);

    const auto auto_bytes = auto_encoder.encode_access_unit(
        std::span<const std::span<const std::int32_t>>{spans});
    const auto naive_bytes = naive_encoder.encode_access_unit(
        std::span<const std::span<const std::int32_t>>{spans});

    CHECK(auto_bytes.size() < naive_bytes.size());

    ac3::mlp::StreamDecoder decoder(24);
    std::vector<std::vector<std::int32_t>> decoded;
    REQUIRE(decoder.decode_access_unit(auto_bytes, decoded));
    REQUIRE(decoded == channels);
}

// --- end-of-stream terminators, FIFO timing, DC offset ----------------------

TEST_CASE("stream: terminators mark the final access unit and carry the pad count", "[mlp]") {
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));
    std::mt19937 rng(0xE05);
    ac3::mlp::StreamConfig config;
    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder(24);

    std::vector<std::int32_t> decoded;
    for (int i = 0; i < 2; ++i) {
        const auto samples = random_samples(rng, frame, 16);
        const auto unit = encoder.encode_access_unit(std::span<const std::int32_t>{samples});
        REQUIRE(decoder.decode_access_unit(unit, decoded));
        CHECK_FALSE(decoder.end_of_stream());
    }

    // The last unit: 7 of its samples are padding the caller appended.
    auto samples = random_samples(rng, frame, 16);
    std::fill(samples.end() - 7, samples.end(), 0);
    const std::array<std::span<const std::int32_t>, 1> channels{samples};
    const auto unit = encoder.encode_access_unit(
        std::span<const std::span<const std::int32_t>>{channels}, ac3::mlp::EndOfStream{7});
    REQUIRE(decoder.decode_access_unit(unit, decoded));
    CHECK(decoder.end_of_stream());
    CHECK(decoder.zero_samples_appended() == 7);

    // §4.6: nothing may follow the terminated stream.
    const auto after = random_samples(rng, frame, 16);
    ac3::mlp::StreamConfig again;
    ac3::mlp::StreamEncoder second_encoder(again);
    const auto extra = second_encoder.encode_access_unit(std::span<const std::int32_t>{after});
    CHECK_FALSE(decoder.decode_access_unit(extra, decoded));
}

TEST_CASE("stream: a zero-pad final unit writes terminatorB and reads back", "[mlp]") {
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));
    std::mt19937 rng(0xE06);
    ac3::mlp::StreamConfig config;
    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder(24);

    const auto samples = random_samples(rng, frame, 16);
    const std::array<std::span<const std::int32_t>, 1> channels{samples};
    const auto unit = encoder.encode_access_unit(
        std::span<const std::span<const std::int32_t>>{channels}, ac3::mlp::EndOfStream{0});
    std::vector<std::int32_t> decoded;
    REQUIRE(decoder.decode_access_unit(unit, decoded));
    CHECK(decoder.end_of_stream());
    CHECK(decoder.zero_samples_appended() == 0);
    CHECK(decoded == samples);
}

TEST_CASE("stream: FIFO timing honours the FBA peak rate and recovers its delay", "[mlp]") {
    // Sixteen channels of incompressible full-scale noise make access units
    // (~16,000 payload bits against a 15,000-bit-per-frame channel) whose
    // §2.7 delivery spacing exceeds one frame - the schedule must stretch
    // the input-timing gaps to keep size/spacing under 18 Mbit/s, then
    // glide back to the constant kFifoDelayFrames delay once silence
    // follows.
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));
    std::mt19937 rng(0xF1F0);
    ac3::mlp::StreamConfig config;
    config.channels = 16;
    config.wordlength = 24;
    ac3::mlp::StreamEncoder encoder(config);

    std::vector<std::vector<std::byte>> units;
    const std::vector<std::vector<std::int32_t>> quiet(
        16, std::vector<std::int32_t>(frame, 0));
    for (int i = 0; i < 40; ++i) {
        auto loud = random_channels(rng, 16, frame, 24);
        const auto& source = i < 20 ? loud : quiet;
        std::vector<std::span<const std::int32_t>> spans(source.begin(), source.end());
        units.push_back(encoder.encode_access_unit(
            std::span<const std::span<const std::int32_t>>{spans}));
    }
    CHECK(encoder.rate_violations() == 0);
    // The point of the fixture: at least one access unit genuinely needed
    // more than a frame of delivery time.
    CHECK(std::any_of(units.begin(), units.end(), [](const auto& u) {
        return u.size() * 8 > 15'000;  // 40 samples x 375 bits/sample
    }));

    // Unwrap each unit's input_timing (bits 4..15 are the length, 16..31
    // the timing) and check §2.7 directly.
    std::vector<std::int64_t> input(units.size());
    std::int64_t previous_wrapped = -1;
    std::int64_t base = 0;
    for (std::size_t n = 0; n < units.size(); ++n) {
        const auto wrapped =
            (std::to_integer<std::int64_t>(units[n][2]) << 8) |
            std::to_integer<std::int64_t>(units[n][3]);
        if (previous_wrapped >= 0 && wrapped < previous_wrapped) {
            base += 65536;
        }
        input[n] = base + wrapped;
        previous_wrapped = wrapped;
    }
    const auto start = input[0];  // = 65536 - the initial delay, unwrapped from 0
    for (std::size_t n = 0; n + 1 < units.size(); ++n) {
        const auto spacing = input[n + 1] - input[n];
        REQUIRE(spacing >= 1);
        const auto size_bits = static_cast<std::int64_t>(units[n].size()) * 8;
        // §2.7: size / spacing <= 18e6 / 48000 bits per sample.
        CHECK(size_bits * 48000 <= spacing * 18'000'000);
        // The delay never leaves [0, kFifoDelayFrames] frames. input[0]
        // sits exactly one full delay before sample 0.
        const auto output_time =
            static_cast<std::int64_t>(n) * static_cast<std::int64_t>(frame) +
            (start + static_cast<std::int64_t>(ac3::mlp::kFifoDelayFrames) *
                         static_cast<std::int64_t>(frame));
        const auto delay = output_time - input[n];
        CHECK(delay >= 0);
        CHECK(delay <= static_cast<std::int64_t>(ac3::mlp::kFifoDelayFrames) *
                           static_cast<std::int64_t>(frame));
    }
    // The quiet tail is fully recovered: constant maximum delay again.
    const auto last = units.size() - 1;
    const auto output_last =
        static_cast<std::int64_t>(last) * static_cast<std::int64_t>(frame) +
        (start + static_cast<std::int64_t>(ac3::mlp::kFifoDelayFrames) *
                     static_cast<std::int64_t>(frame));
    CHECK(output_last - input[last] ==
          static_cast<std::int64_t>(ac3::mlp::kFifoDelayFrames) *
              static_cast<std::int64_t>(frame));

    // And the measured peak respects the ceiling it was scheduled against.
    CHECK(encoder.measured_peak_data_rate_16ths() <=
          ac3::mlp::peak_data_rate_ceiling_16ths(ac3::mlp::SampleRate::k48000));
    CHECK(encoder.measured_peak_data_rate_16ths() > 0);
}

TEST_CASE("stream: a configured peak_data_rate leaves access-unit sizes untouched", "[mlp]") {
    // The two-pass contract: re-encoding with the measured peak written into
    // the major sync changes only that field (and its CRC), never a size.
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));
    std::mt19937 rng(0x2FA5);
    std::vector<std::vector<std::int32_t>> audio;
    for (int i = 0; i < 10; ++i) {
        audio.push_back(random_samples(rng, frame, 20));
    }

    ac3::mlp::StreamConfig config;
    ac3::mlp::StreamEncoder first(config);
    std::vector<std::size_t> sizes;
    for (const auto& samples : audio) {
        sizes.push_back(first.encode_access_unit(std::span<const std::int32_t>{samples}).size());
    }

    config.peak_data_rate_16ths = first.measured_peak_data_rate_16ths();
    ac3::mlp::StreamEncoder second(config);
    ac3::mlp::StreamDecoder decoder(24);
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const auto unit =
            second.encode_access_unit(std::span<const std::int32_t>{audio[i]});
        CHECK(unit.size() == sizes[i]);
        std::vector<std::int32_t> decoded;
        REQUIRE(decoder.decode_access_unit(unit, decoded));
        REQUIRE(decoded == audio[i]);
    }
}

TEST_CASE("block: a DC pedestal costs nothing - the offset rides the LSB word", "[mlp]") {
    // The same varying signal with and without a +2000 pedestal must code to
    // the SAME size: midrange centring folds the pedestal into the LSB
    // word's free DC slot, so the entropy layer sees identical residuals.
    // (This is the exact shape that once forced the whole block onto a wider
    // entropy table via the predictor's warm-up outlier.)
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));
    std::vector<std::int32_t> centred(frame);
    for (std::size_t i = 0; i < frame; ++i) {
        centred[i] = static_cast<std::int32_t>(i) - static_cast<std::int32_t>(frame / 2);
    }
    auto offset = centred;
    for (auto& v : offset) {
        v += 2000;
    }

    ac3::mlp::StreamConfig config;
    config.automatic = true;
    ac3::mlp::StreamEncoder encode_centred(config);
    ac3::mlp::StreamEncoder encode_offset(config);
    const auto unit_centred =
        encode_centred.encode_access_unit(std::span<const std::int32_t>{centred});
    const auto unit_offset =
        encode_offset.encode_access_unit(std::span<const std::int32_t>{offset});
    CHECK(unit_centred.size() == unit_offset.size());

    ac3::mlp::StreamDecoder decoder(24);
    std::vector<std::int32_t> decoded;
    REQUIRE(decoder.decode_access_unit(unit_offset, decoded));
    REQUIRE(decoded == offset);
}

// --- Atmos structural layer: 16ch_channel_meaning + EXTRA_DATA -------------

TEST_CASE("extra_data: framing round trips and rejects tampering", "[mlp]") {
    // Odd- and even-length payloads exercise both padding cases (§4.8.4
    // fills to a 16-bit boundary, so 0 or 1 zero bytes follow the payload).
    for (const std::size_t size : {1u, 2u, 5u, 32u, 33u}) {
        CAPTURE(size);
        std::vector<std::byte> payload(size);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::byte>(0x30 + i);
        }
        const auto framed = ac3::mlp::build_extra_data(payload);
        REQUIRE(framed.size() % 2 == 0);
        std::vector<std::byte> back;
        REQUIRE(ac3::mlp::parse_extra_data(framed, back));
        REQUIRE(back.size() >= size);
        REQUIRE(back.size() - size <= 1);
        CHECK(std::equal(payload.begin(), payload.end(), back.begin()));
        if (back.size() > size) {
            CHECK(back.back() == std::byte{0});
        }

        // §4.8.5's parity must catch a flipped payload byte.
        auto corrupted = framed;
        corrupted[2] ^= std::byte{0x01};
        CHECK_FALSE(ac3::mlp::parse_extra_data(corrupted, back));

        // And §4.8.2's nibble must catch a mangled length word.
        auto bad_length = framed;
        bad_length[1] ^= std::byte{0x01};
        CHECK_FALSE(ac3::mlp::parse_extra_data(bad_length, back));
    }

    // An empty payload IS no EXTRA_DATA (§4.8 makes the field optional).
    CHECK(ac3::mlp::build_extra_data({}).empty());

    // §4.8: a first word of zero is all padding - valid, and empty.
    const std::array<std::byte, 4> padding_only{std::byte{0}, std::byte{0}, std::byte{0},
                                                std::byte{0}};
    std::vector<std::byte> out{std::byte{0x55}};
    REQUIRE(ac3::mlp::parse_extra_data(padding_only, out));
    CHECK(out.empty());
}

TEST_CASE("sync: 16ch channel meaning round trips through the major sync", "[mlp]") {
    ac3::mlp::MajorSyncInfo info;
    info.sample_rate = ac3::mlp::SampleRate::k48000;
    info.substreams = 1;
    info.substream_info = 0x14;

    SECTION("dynamic objects only, LFE leading") {
        ac3::mlp::SixteenChannelMeaning m;
        m.dialogue_norm = 27;
        m.mix_level = 12;
        m.channel_count = 12;
        m.dyn_object_only = true;
        m.lfe_present = true;
        info.sixteen_channel = m;
    }
    SECTION("loudspeaker feeds, ISF bed, and dynamic objects") {
        // Table 17 code 0111b: feeds, then BH7.3.0.0's 10 ISF channels,
        // then objects. 2 feeds (L/R) + 10 ISF + 4 objects = 16.
        ac3::mlp::SixteenChannelMeaning m;
        m.channel_count = 16;
        m.dyn_object_only = false;
        m.content_description = 0b111;
        m.lfe_only = false;
        m.channel_assignment = 0x001;  // L/R
        m.intermediate_spatial_format = 0b010;
        m.dynamic_object_count = 4;
        info.sixteen_channel = m;
    }

    const auto bytes = ac3::mlp::build_major_sync_info(info);
    // The extension grows the structure past the fixed 28 bytes, and the
    // size prober must agree with what the builder produced.
    CHECK(bytes.size() > 28);
    CHECK(ac3::mlp::major_sync_info_size(bytes) == bytes.size());

    ac3::mlp::MajorSyncInfo parsed;
    REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));
    REQUIRE(parsed.sixteen_channel.has_value());
    CHECK((parsed.substream_info & 0x80) != 0);
    CHECK(parsed.sixteen_channel->dialogue_norm == info.sixteen_channel->dialogue_norm);
    CHECK(parsed.sixteen_channel->mix_level == info.sixteen_channel->mix_level);
    CHECK(parsed.sixteen_channel->channel_count == info.sixteen_channel->channel_count);
    CHECK(parsed.sixteen_channel->dyn_object_only == info.sixteen_channel->dyn_object_only);
    CHECK(parsed.sixteen_channel->lfe_present == info.sixteen_channel->lfe_present);
    CHECK(parsed.sixteen_channel->content_description ==
          info.sixteen_channel->content_description);
    CHECK(parsed.sixteen_channel->channel_assignment ==
          info.sixteen_channel->channel_assignment);
    CHECK(parsed.sixteen_channel->intermediate_spatial_format ==
          info.sixteen_channel->intermediate_spatial_format);
    CHECK(parsed.sixteen_channel->dynamic_object_count ==
          info.sixteen_channel->dynamic_object_count);
}

TEST_CASE("sync: a plain major sync still parses and sizes as 28 bytes", "[mlp]") {
    ac3::mlp::MajorSyncInfo info;
    info.substreams = 1;
    const auto bytes = ac3::mlp::build_major_sync_info(info);
    CHECK(bytes.size() == 28);
    CHECK(ac3::mlp::major_sync_info_size(bytes) == 28);
    ac3::mlp::MajorSyncInfo parsed;
    REQUIRE(ac3::mlp::parse_major_sync_info(bytes, parsed));
    CHECK_FALSE(parsed.sixteen_channel.has_value());
}

TEST_CASE("stream: an Atmos-shaped stream carries channel roles and object metadata",
          "[mlp]") {
    // The structural whole: a 12-channel presentation - a 5.1 bed as
    // loudspeaker feeds plus six dynamic objects, each a discrete lossless
    // channel (TrueHD's way, no JOC) - described by 16ch_channel_meaning()
    // in every major sync, with per-frame OAMD object positions riding an
    // EMDF container in EXTRA_DATA().
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));

    ac3::mlp::SixteenChannelMeaning meaning;
    meaning.channel_count = 12;
    meaning.dyn_object_only = false;
    meaning.content_description = 0b101;  // feeds + dynamic objects
    meaning.lfe_only = false;
    meaning.channel_assignment = 0x00F;  // L/R, C, LFE, Ls/Rs = 6 feeds
    meaning.dynamic_object_count = 6;

    ac3::mlp::StreamConfig config;
    config.channels = 12;
    config.automatic = true;
    config.sixteen_channel = meaning;
    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder;

    std::mt19937 rng(0xA7305);
    for (int au = 0; au < 3; ++au) {
        CAPTURE(au);
        auto channels = random_channels(rng, 12, frame, 20);
        std::vector<std::span<const std::int32_t>> spans(channels.begin(), channels.end());

        // Per-frame object metadata: six objects on the move.
        std::vector<ac3::oba::DynamicObject> objects(6);
        for (std::size_t i = 0; i < objects.size(); ++i) {
            objects[i].position = {0.1 * static_cast<double>(i + au), 0.5, 0.25};
        }
        const ac3::oba::Program program{
            .dynamic_only = false, .lfe = false, .bed = ac3::oba::bed::k51,
            .dynamic_objects = 6};
        const auto oamd = ac3::oba::build_payload(program, objects);
        const std::array<ac3::emdf::Payload, 1> payloads{
            ac3::emdf::Payload{ac3::emdf::kPayloadIdOamd, oamd}};
        const auto container = ac3::emdf::build_container(payloads);

        const auto unit = encoder.encode_access_unit(
            std::span<const std::span<const std::int32_t>>{spans}, container);

        std::vector<std::vector<std::int32_t>> decoded;
        REQUIRE(decoder.decode_access_unit(unit, decoded));
        REQUIRE(decoded == channels);

        // The metadata came through intact (the framing may append one
        // zero byte of §4.8.4 padding the payload formats self-delimit
        // past).
        const auto& extra = decoder.extra_data();
        REQUIRE(extra.size() >= container.size());
        REQUIRE(extra.size() - container.size() <= 1);
        CHECK(std::equal(container.begin(), container.end(), extra.begin()));
    }

    // And the stream announced its channel roles in-band.
    REQUIRE(decoder.sixteen_channel().has_value());
    CHECK(decoder.sixteen_channel()->channel_count == 12);
    CHECK(decoder.sixteen_channel()->content_description == 0b101);
    CHECK(decoder.sixteen_channel()->channel_assignment == 0x00F);
    CHECK(decoder.sixteen_channel()->dynamic_object_count == 6);
    CHECK(decoder.channels() == 12);
}

TEST_CASE("stream: an access unit without extra data reports none", "[mlp]") {
    const auto frame = static_cast<std::size_t>(
        ac3::mlp::samples_per_access_unit(ac3::mlp::SampleRate::k48000));
    std::mt19937 rng(0xE0DA);
    ac3::mlp::StreamConfig config;
    ac3::mlp::StreamEncoder encoder(config);
    ac3::mlp::StreamDecoder decoder;
    const auto samples = random_samples(rng, frame, 16);
    const auto unit = encoder.encode_access_unit(std::span<const std::int32_t>{samples});
    std::vector<std::int32_t> decoded;
    REQUIRE(decoder.decode_access_unit(unit, decoded));
    CHECK(decoder.extra_data().empty());
}
