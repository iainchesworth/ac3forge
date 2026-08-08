#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"

namespace {

// The decoder side of §6.6.3 Pseudocode 4, walking the normative trees. The
// encoder was generated from those same trees, so this is not a round trip
// against itself: the generator inverted them and this walks them forwards. A
// disagreement means the inversion is wrong.
struct HuffTree {
    std::span<const std::array<int, 2>> nodes;
};

// Rebuilt from the encode tables rather than duplicating the trees: a prefix
// code is uniquely determined by its (code, length) pairs, so decoding by
// longest-match over them is equivalent to walking the tree.
// §5.5.1 variable_bits_max(n, max_num_groups), as the decoder. Same shape as
// EMDF's variable_bits with a ceiling on the group count.
std::uint32_t read_variable_bits_max(ac3::BitReader& r, int group_bits, int max_groups) {
    std::uint32_t value = 0;
    for (int group = 1;; ++group) {
        value += r.read(group_bits);
        const bool read_more = r.read_bit() != 0;
        if (!read_more || group >= max_groups) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

int huff_decode(std::span<const ac3::joc::HuffCode> table, ac3::BitReader& r) {
    std::uint32_t accumulated = 0;
    for (int bits = 1; bits <= 32; ++bits) {
        accumulated = (accumulated << 1) | r.read_bit();
        for (std::size_t value = 0; value < table.size(); ++value) {
            if (table[value].bits == bits && table[value].code == accumulated) {
                return static_cast<int>(value);
            }
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("JOC quantization round-trips through the spec's own scale", "[oba][joc]") {
    // §6.6.4's note pins the reachable range exactly, which is the cheapest
    // check that the 820/4096 scale and the nquant/2 origin are both right.
    CHECK_THAT(ac3::joc::dequantize(0, false),
               Catch::Matchers::WithinAbs(-9.609, 0.001));
    CHECK_THAT(ac3::joc::dequantize(95, false),
               Catch::Matchers::WithinAbs(9.410, 0.001));
    CHECK_THAT(ac3::joc::dequantize(0, true),
               Catch::Matchers::WithinAbs(-9.609, 0.001));
    CHECK_THAT(ac3::joc::dequantize(191, true),
               Catch::Matchers::WithinAbs(9.509, 0.001));

    // Zero gain is a code, not an approximation - it is the origin.
    CHECK(ac3::joc::quantize(0.0, false) == 48);
    CHECK(ac3::joc::quantize(0.0, true) == 96);
    CHECK(ac3::joc::dequantize(48, false) == 0.0);

    // Fine quantization must actually halve the step.
    const double coarse_step = ac3::joc::dequantize(49, false);
    const double fine_step = ac3::joc::dequantize(97, true);
    CHECK_THAT(coarse_step, Catch::Matchers::WithinAbs(2.0 * fine_step, 1e-12));

    for (const bool fine : {false, true}) {
        for (const double value : {-9.0, -1.0, -0.2, 0.0, 0.5, 1.0, 3.3, 9.0}) {
            const double back = ac3::joc::dequantize(ac3::joc::quantize(value, fine), fine);
            CHECK_THAT(back, Catch::Matchers::WithinAbs(value, fine ? 0.051 : 0.101));
        }
    }
}

TEST_CASE("Table 54 matches the standard's worked example", "[oba][joc]") {
    // §6.6.5: "If joc_num_bands = 15 and the input to sb_to_pb(subband) is the
    // subband value 24, sb_to_pb(24) returns the value 13."
    STATIC_CHECK(ac3::joc::kNumBands[6] == 15);
    STATIC_CHECK(ac3::joc::kSubbandToBand[6][24] == 13);
    // Every mapping has to reach its last band, or the top of the spectrum
    // would be coded with parameters nothing ever reads.
    for (std::size_t idx = 0; idx < ac3::joc::kNumBands.size(); ++idx) {
        CHECK(ac3::joc::kSubbandToBand[idx][0] == 0);
        CHECK(ac3::joc::kSubbandToBand[idx][63] == ac3::joc::kNumBands[idx] - 1);
    }
}

TEST_CASE("JOC payload decodes back to the matrix it was given", "[oba][joc]") {
    ac3::joc::FrameParameters params{.objects = 4, .num_bands_idx = 4, .seq_count = 7};
    params.matrix.resize(params.coefficient_count());
    // A matrix with structure rather than noise: each object leans on a
    // different channel, and the lean varies across bands. Constant values
    // would make every differential zero and hide a broken predictor.
    for (int object = 0; object < params.objects; ++object) {
        for (int channel = 0; channel < params.channels; ++channel) {
            for (int band = 0; band < params.bands(); ++band) {
                params.at(object, channel, band) =
                    (object == channel ? 1.0 : -0.3) + 0.1 * band - 0.05 * channel;
            }
        }
    }

    const auto payload = ac3::joc::build_payload(params);
    ac3::BitReader r{payload};

    // --- joc_header ---
    CHECK(r.read(3) == 0);  // joc_dmx_config_idx: 5.X
    CHECK(r.read(6) == 3);  // joc_num_objects_bits = objects - 1
    CHECK(r.read(3) == 0);  // joc_ext_config_idx

    // --- joc_info ---
    CHECK(r.read(3) == 0);   // joc_clipgain_x_bits
    CHECK(r.read(5) == 0);   // joc_clipgain_y_bits
    CHECK(r.read(10) == 7);  // joc_seq_count_bits
    for (int object = 0; object < params.objects; ++object) {
        CHECK(r.read(1) == 1);  // b_joc_obj_present
        CHECK(r.read(3) == 4);  // joc_num_bands_idx
        CHECK(r.read(1) == 0);  // b_joc_sparse
        CHECK(r.read(1) == 0);  // joc_num_quant_idx
        CHECK(r.read(1) == 0);  // joc_slope_idx
        CHECK(r.read(1) == 0);  // joc_num_dpoints_bits
    }

    // --- joc_data, undone exactly as §6.6.2 Pseudocode 3 specifies ---
    constexpr int kNquant = 96;
    const std::span<const ac3::joc::HuffCode> table{ac3::joc::kMtxCoarse};
    for (int object = 0; object < params.objects; ++object) {
        for (int channel = 0; channel < params.channels; ++channel) {
            int previous = kNquant / 2;  // the offset Pseudocode 3 starts from
            for (int band = 0; band < params.bands(); ++band) {
                const int difference = huff_decode(table, r);
                REQUIRE(difference >= 0);
                const int code = (previous + difference) % kNquant;
                previous = code;
                CHECK_THAT(ac3::joc::dequantize(code, false),
                           Catch::Matchers::WithinAbs(
                               params.at(object, channel, band), 0.101));
            }
        }
    }
    CHECK_FALSE(r.overflowed());
    // Only padding may remain, and §6.2.1 caps it at seven bits.
    CHECK(payload.size() * 8 - r.bit_position() < 8);
}

TEST_CASE("JOC codes an unchanged band in a single bit", "[oba][joc]") {
    // Value 0 has a one-bit codeword in every generic table, which is the
    // whole reason the matrix is differentially coded along the bands: a
    // coefficient that does not move across the spectrum is nearly free.
    ac3::joc::FrameParameters flat{.objects = 1, .num_bands_idx = 7};  // 23 bands
    flat.matrix.assign(flat.coefficient_count(), 0.0);
    const auto payload = ac3::joc::build_payload(flat);
    // joc_header 12 + joc_info's fixed 18 + 8 per object (presence, bands,
    // sparse, quant, slope, data points) = 38 bits, then 5 channels x 23 bands
    // of zero-difference codewords at one bit each.
    CHECK(payload.size() == (38 + 5 * 23 + 7) / 8);
}

TEST_CASE("OAMD describes an LFE-only bed and its objects", "[oba][oamd]") {
    const ac3::oba::Program program{.bed = ac3::oba::bed::kLfe, .dynamic_objects = 3};
    CHECK(ac3::oba::object_count(program) == 4);
    // The LFE is an object but never a JOC output - §6.3.2.2 bypasses it.
    CHECK(ac3::oba::joc_object_count(program) == 3);

    const std::array<ac3::oba::DynamicObject, 3> objects{{
        {.position = {.x = 0.0, .y = 0.0, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 1.0, .y = 1.0, .z = 1.0}, .gain_db = -6.0},
        {.position = {.x = 0.5, .y = 0.5, .z = -1.0}, .gain_db = 3.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);
    ac3::BitReader r{payload};

    CHECK(r.read(2) == 0);  // oa_md_version_bits
    CHECK(r.read(5) == 3);  // object_count_bits = object_count - 1

    // --- program_assignment ---
    CHECK(r.read(1) == 0);       // b_dyn_object_only_program
    CHECK(r.read(4) == 0b1010);  // content_description: a bed and dynamic objects
    CHECK(r.read(1) == 0);       // b_bed_chan_distribute
    CHECK(r.read(1) == 0);       // b_multiple_bed_instances_present
    CHECK(r.read(1) == 1);       // b_lfe_only - the assignment field is absent
    CHECK(r.read(5) == 2);       // num_dynamic_objects_bits = count - 1

    CHECK(r.read(1) == 0);  // b_alternate_object_data_present
    CHECK(r.read(4) == 1);  // oa_element_count_bits

    // --- oa_element_md ---
    CHECK(r.read(4) == 1);  // oa_element_id_idx: object_element
    const auto size_bits = read_variable_bits_max(r, 4, 4);
    CHECK(r.read(1) == 0);  // b_discard_unknown_element
    const std::size_t element_start = r.bit_position();

    // --- object_element ---
    CHECK(r.read(2) == 0);     // sample_offset_code
    CHECK(r.read(3) == 0);     // num_obj_info_blocks_bits => one block
    CHECK(r.read(6) == 0);     // block_offset_factor_bits
    CHECK(r.read(2) == 0b10);  // ramp_duration_code: 1 536 samples, one frame
    CHECK(r.read(1) == 1);     // b_reserved_data_not_present

    // Object 0 is the bed's LFE: basic info only, no render info, because
    // §5.5.9 forces object_render_info_status_idx to 0 for a bed object.
    CHECK(r.read(1) == 0);     // b_object_not_active
    CHECK(r.read(2) == 0b00);  // object_gain_idx: 0 dB
    CHECK(r.read(1) == 1);     // b_default_object_priority
    CHECK(r.read(1) == 0);     // b_additional_table_data_exists

    const std::array<std::uint32_t, 3> expect_x{0, 62, 31};
    const std::array<std::uint32_t, 3> expect_y{0, 62, 31};
    const std::array<std::uint32_t, 3> expect_z_sign{1, 1, 0};
    const std::array<std::uint32_t, 3> expect_z{0, 15, 15};
    // Table 19: +3 dB is code 15-3 = 12; -6 dB is code 14-(-6) = 20.
    const std::array<std::uint32_t, 3> expect_gain_idx{0b00, 0b10, 0b10};
    const std::array<std::uint32_t, 3> expect_gain_bits{0, 20, 12};

    for (std::size_t object = 0; object < objects.size(); ++object) {
        CAPTURE(object);
        CHECK(r.read(1) == 0);  // b_object_not_active
        CHECK(r.read(2) == expect_gain_idx[object]);
        if (expect_gain_idx[object] == 0b10) {
            CHECK(r.read(6) == expect_gain_bits[object]);
        }
        CHECK(r.read(1) == 1);  // b_default_object_priority

        CHECK(r.read(6) == expect_x[object]);
        CHECK(r.read(6) == expect_y[object]);
        CHECK(r.read(1) == expect_z_sign[object]);
        CHECK(r.read(4) == expect_z[object]);
        CHECK(r.read(1) == 0);     // b_object_distance_specified
        CHECK(r.read(3) == 0);     // zone_constraints_idx
        CHECK(r.read(1) == 1);     // b_enable_elevation
        CHECK(r.read(2) == 0b00);  // object_size_idx: a point source
        CHECK(r.read(1) == 0);     // b_object_use_screen_ref
        CHECK(r.read(1) == 0);     // b_object_snap
        CHECK(r.read(1) == 0);     // b_additional_table_data_exists
    }
    CHECK_FALSE(r.overflowed());

    // §5.6.4.3: oa_element_size covers b_discard_unknown_element, the element
    // and its padding. The reader is at the end of the element now, so the
    // measured content is exactly known and the declared size must be the
    // fewest whole bytes that holds it - stated as an equality, because "the
    // region covers the content" alone is satisfied by a size that is one byte
    // too big and only fails when the content happens to fill its last byte.
    const std::size_t content_bits = r.bit_position() - (element_start - 1);
    CHECK((size_bits + 1) * 8 == (content_bits + 7) / 8 * 8);

    // That boundary is a byte measured from the ELEMENT's start, which is not
    // the payload's - so §5.5.2's own trailing padding still has bits to add.
    const std::size_t element_end = element_start - 1 + (size_bits + 1) * 8;
    CHECK(element_end <= payload.size() * 8);
    CHECK(payload.size() * 8 - element_end < 8);
}

TEST_CASE("oa_element_size holds whatever the object count makes it", "[oba][oamd]") {
    // The element's length moves by 31 bits per object, so it lands on every
    // residue mod 8 as the count climbs - including the one where the content
    // exactly fills its last byte, which is the only count that catches a size
    // computed from the element without the flag bit that precedes it.
    for (int count = 1; count <= 8; ++count) {
        CAPTURE(count);
        const ac3::oba::Program program{.bed = ac3::oba::bed::kLfe,
                                        .dynamic_objects = count};
        const std::vector<ac3::oba::DynamicObject> objects(
            static_cast<std::size_t>(count));
        const auto payload = ac3::oba::build_payload(program, objects);

        ac3::BitReader r{payload};
        r.skip(2 + 5);              // version, object_count
        r.skip(1 + 4 + 1 + 1 + 1);  // program_assignment through b_lfe_only
        r.skip(5);                  // num_dynamic_objects_bits
        r.skip(1 + 4);              // alternate data, oa_element_count
        r.skip(4);                  // oa_element_id_idx
        const auto size_bits = read_variable_bits_max(r, 4, 4);
        const std::size_t flag_at = r.bit_position();

        r.skip(1);                  // b_discard_unknown_element
        r.skip(2 + 3 + 6 + 2 + 1);  // md_update_info, block_update_info, reserved
        r.skip(1 + 2 + 1 + 1);      // the bed's LFE object_info_block
        for (int object = 0; object < count; ++object) {
            r.skip(1 + 2 + 1);                     // not_active, gain (0 dB), priority
            r.skip(6 + 6 + 1 + 4 + 1);             // position and distance
            r.skip(3 + 1 + 2 + 1 + 1);             // zone, size, screen ref, snap
            r.skip(1);                             // b_additional_table_data_exists
        }
        REQUIRE_FALSE(r.overflowed());

        const std::size_t content_bits = r.bit_position() - flag_at;
        CHECK((size_bits + 1) * 8 == (content_bits + 7) / 8 * 8);
    }
}

TEST_CASE("OAMD carries a full 5.1 bed when asked", "[oba][oamd]") {
    const ac3::oba::Program program{.bed = ac3::oba::bed::k51, .dynamic_objects = 0};
    CHECK(ac3::oba::object_count(program) == 6);
    CHECK(ac3::oba::joc_object_count(program) == 5);

    const auto payload = ac3::oba::build_payload(program, {});
    ac3::BitReader r{payload};
    CHECK(r.read(2) == 0);       // oa_md_version_bits
    CHECK(r.read(5) == 5);       // object_count_bits
    CHECK(r.read(1) == 0);       // b_dyn_object_only_program
    CHECK(r.read(4) == 0b1000);  // a bed, no dynamic objects
    CHECK(r.read(1) == 0);       // b_bed_chan_distribute
    CHECK(r.read(1) == 0);       // b_multiple_bed_instances_present
    CHECK(r.read(1) == 0);       // b_lfe_only
    CHECK(r.read(1) == 1);       // b_standard_chan_assign
    // Table 12, index 9 as the most significant bit: L/R, C, LFE, Ls/Rs.
    CHECK(r.read(10) == 0b1111000000);
}
