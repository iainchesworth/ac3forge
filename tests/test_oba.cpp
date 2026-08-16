#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/tables.hpp"
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

TEST_CASE("reconstruct is a 256-sample-delayed identity when the matrix is a pure passthrough",
         "[oba][joc]") {
    // A degenerate but exact check on the transform pair itself, decoupled
    // from any panning/mixing math: M[0][0][*] = 1, every other entry 0,
    // should hand channel 0 straight back through - modulo the 256-sample
    // algorithmic delay every MDCT-based encode+decode round trip in this
    // codebase carries (see tests/test_eac3_decoder.cpp's own snr_db helper
    // and its "256-sample encode+decode delay" comment; reconstruct() runs
    // its own independent forward+inverse MDCT pass, so it carries that same
    // one block of delay relative to whatever bed it was handed).
    ac3::joc::FrameParameters params{.objects = 1, .num_bands_idx = 4};
    params.matrix.assign(params.coefficient_count(), 0.0);
    for (int band = 0; band < params.bands(); ++band) {
        params.at(0, 0, band) = 1.0;
    }

    std::vector<std::vector<float>> bed(5, std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
    for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
        bed[0][static_cast<std::size_t>(n)] = static_cast<float>(
            0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / 48000.0));
    }

    ac3::joc::ReconstructionState state;
    std::vector<std::vector<float>> out;
    for (int frame = 0; frame < 3; ++frame) {
        out = ac3::joc::reconstruct(bed, params, state);
    }
    REQUIRE(out.size() == 1);

    constexpr int kDelay = 256;
    double signal = 0.0;
    double error = 0.0;
    for (int n = kDelay; n < ac3::kSamplesPerFrame; ++n) {
        const double s = bed[0][static_cast<std::size_t>(n - kDelay)];
        const double r = out[0][static_cast<std::size_t>(n)];
        signal += s * s;
        error += (s - r) * (s - r);
    }
    CHECK(10.0 * std::log10(signal / std::max(error, 1e-30)) > 100.0);
}

TEST_CASE("JOC parse_payload decodes back to the matrix it was given", "[oba][joc]") {
    for (const bool fine : {false, true}) {
        CAPTURE(fine);
        ac3::joc::FrameParameters params{
            .objects = 4, .num_bands_idx = 4, .fine_quant = fine, .seq_count = 7};
        params.matrix.resize(params.coefficient_count());
        for (int object = 0; object < params.objects; ++object) {
            for (int channel = 0; channel < params.channels; ++channel) {
                for (int band = 0; band < params.bands(); ++band) {
                    params.at(object, channel, band) =
                        (object == channel ? 1.0 : -0.3) + 0.1 * band - 0.05 * channel;
                }
            }
        }

        const auto payload = ac3::joc::build_payload(params);
        const auto decoded = ac3::joc::parse_payload(payload);
        REQUIRE(decoded.has_value());
        CHECK(decoded->objects == params.objects);
        CHECK(decoded->channels == params.channels);
        CHECK(decoded->num_bands_idx == params.num_bands_idx);
        CHECK(decoded->fine_quant == params.fine_quant);
        CHECK(decoded->seq_count == params.seq_count);
        REQUIRE(decoded->matrix.size() == params.matrix.size());
        for (int object = 0; object < params.objects; ++object) {
            for (int channel = 0; channel < params.channels; ++channel) {
                for (int band = 0; band < params.bands(); ++band) {
                    CAPTURE(object, channel, band);
                    CHECK_THAT(decoded->at(object, channel, band),
                              Catch::Matchers::WithinAbs(params.at(object, channel, band),
                                                          fine ? 0.051 : 0.101));
                }
            }
        }
    }
}

TEST_CASE("JOC parse_payload covers every band count and the object-count boundary", "[oba][joc]") {
    for (const int num_bands_idx : {0, 1, 2, 3, 4, 5, 6, 7}) {
        CAPTURE(num_bands_idx);
        for (const int objects : {1, ac3::joc::kMaxObjects}) {
            CAPTURE(objects);
            ac3::joc::FrameParameters params{.objects = objects, .num_bands_idx = num_bands_idx};
            params.matrix.assign(params.coefficient_count(), 0.0);
            for (int object = 0; object < objects; ++object) {
                for (int channel = 0; channel < params.channels; ++channel) {
                    for (int band = 0; band < params.bands(); ++band) {
                        params.at(object, channel, band) = 0.2 * static_cast<double>(band + 1);
                    }
                }
            }
            const auto payload = ac3::joc::build_payload(params);
            const auto decoded = ac3::joc::parse_payload(payload);
            REQUIRE(decoded.has_value());
            CHECK(decoded->objects == objects);
            CHECK(decoded->bands() == params.bands());
        }
    }
}

TEST_CASE("JOC parse_payload rejects what it cannot cleanly interpret", "[oba][joc]") {
    ac3::joc::FrameParameters params{.objects = 2, .num_bands_idx = 3};
    params.matrix.assign(params.coefficient_count(), 0.5);
    const auto payload = ac3::joc::build_payload(params);
    REQUIRE(ac3::joc::parse_payload(payload).has_value());

    SECTION("truncated payload") {
        for (const std::size_t cut : {std::size_t{1}, payload.size() / 2, payload.size() - 1}) {
            CAPTURE(cut);
            const std::vector<std::byte> truncated(payload.begin(),
                                                   payload.begin() + static_cast<std::ptrdiff_t>(cut));
            CHECK_FALSE(ac3::joc::parse_payload(truncated).has_value());
        }
    }

    SECTION("a non-5.X downmix config") {
        // joc_dmx_config_idx occupies the payload's top 3 bits; forcing them
        // to a nonzero value is the cheapest way to name a 7.X config this
        // parser does not implement.
        auto corrupt = payload;
        corrupt[0] |= std::byte{0b001'00000};
        CHECK_FALSE(ac3::joc::parse_payload(corrupt).has_value());
    }

    SECTION("an empty payload") {
        CHECK_FALSE(ac3::joc::parse_payload({}).has_value());
    }
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

TEST_CASE("OAMD describes a dynamic-object program and its LFE", "[oba][oamd]") {
    // The shape Dolby's own DD+ JOC reference streams use: object_count 16,
    // b_dyn_object_only_program 1, b_lfe_present 1, and joc_num_objects 15 -
    // one fewer, because the LFE is bypassed rather than matrixed.
    const ac3::oba::Program program{
        .dynamic_only = true, .lfe = true, .dynamic_objects = 3};
    CHECK(ac3::oba::object_count(program) == 4);
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
    // The whole branch is two bits: object_count above already covers the
    // program, so the dynamic-object count is what is left after the LFE.
    CHECK(r.read(1) == 1);  // b_dyn_object_only_program
    CHECK(r.read(1) == 1);  // b_lfe_present

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
        const ac3::oba::Program program{
            .dynamic_only = true, .lfe = true, .dynamic_objects = count};
        const std::vector<ac3::oba::DynamicObject> objects(
            static_cast<std::size_t>(count));
        const auto payload = ac3::oba::build_payload(program, objects);

        ac3::BitReader r{payload};
        r.skip(2 + 5);  // version, object_count
        r.skip(1 + 1);  // b_dyn_object_only_program, b_lfe_present
        r.skip(1 + 4);  // alternate data, oa_element_count
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
    const ac3::oba::Program program{
        .dynamic_only = false, .bed = ac3::oba::bed::k51, .dynamic_objects = 0};
    CHECK(ac3::oba::object_count(program) == 6);
    CHECK(ac3::oba::joc_object_count(program) == 5);

    const auto payload = ac3::oba::build_payload(program, {});
    ac3::BitReader r{payload};
    CHECK(r.read(2) == 0);       // oa_md_version_bits
    CHECK(r.read(5) == 5);       // object_count_bits
    CHECK(r.read(1) == 0);  // b_dyn_object_only_program
    // content_description written index 0 first, so the bed flag (element 3)
    // is the LAST of the four bits, not the first.
    CHECK(r.read(4) == 0b0001);  // a bed, no dynamic objects
    CHECK(r.read(1) == 0);       // b_bed_chan_distribute
    CHECK(r.read(1) == 0);       // b_multiple_bed_instances_present
    CHECK(r.read(1) == 0);       // b_lfe_only
    CHECK(r.read(1) == 1);       // b_standard_chan_assign
    // Table 12 with index 0 first: L/R (9), C (8), LFE (7), Ls/Rs (6) become
    // the four LEAST significant bits of the 10. Same order Dolby's own
    // encoder writes for a 7.1.4 bed.
    CHECK(r.read(10) == 0b0000001111);
}

TEST_CASE("OAMD payload decodes back to the program and objects it described", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = true, .lfe = true, .dynamic_objects = 3};
    const std::array<ac3::oba::DynamicObject, 3> objects{{
        {.position = {.x = 0.0, .y = 0.0, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 1.0, .y = 1.0, .z = 1.0}, .gain_db = -6.0},
        {.position = {.x = 0.5, .y = 0.5, .z = -1.0}, .gain_db = 3.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);

    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    CHECK(decoded->program.dynamic_only == program.dynamic_only);
    CHECK(decoded->program.lfe == program.lfe);
    CHECK(decoded->program.dynamic_objects == program.dynamic_objects);
    REQUIRE(decoded->objects.size() == objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        CAPTURE(i);
        // Positions/z round-trip exactly here because every input value sits
        // exactly on the quantizer's grid (0, 0.5, 1 over 62nds; -1, 0, 1
        // over 15ths) - the same reason test_oba's encode-side test above can
        // assert exact codes rather than tolerances.
        CHECK(decoded->objects[i].position.x == objects[i].position.x);
        CHECK(decoded->objects[i].position.y == objects[i].position.y);
        CHECK(decoded->objects[i].position.z == objects[i].position.z);
        CHECK(decoded->objects[i].gain_db == objects[i].gain_db);
    }
}

TEST_CASE("OAMD payload decodes a full 5.1 bed with no dynamic objects", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = false, .bed = ac3::oba::bed::k51, .dynamic_objects = 0};
    const auto payload = ac3::oba::build_payload(program, {});

    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->program.dynamic_only);
    CHECK(decoded->program.bed == ac3::oba::bed::k51);
    CHECK(decoded->program.dynamic_objects == 0);
    CHECK(decoded->objects.empty());
}

TEST_CASE("OAMD payload decodes a bed plus dynamic objects together", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = false, .bed = ac3::oba::bed::kLfe, .dynamic_objects = 2};
    const std::array<ac3::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.25, .y = 0.75, .z = 0.5}, .gain_db = -20.0},
        {.position = {.x = 1.0, .y = 0.0, .z = -0.5}, .gain_db = 10.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);

    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->program.dynamic_only);
    CHECK(decoded->program.bed == ac3::oba::bed::kLfe);
    REQUIRE(decoded->objects.size() == 2);
    CHECK(decoded->objects[0].gain_db == -20.0);
    CHECK(decoded->objects[1].gain_db == 10.0);
}

TEST_CASE("OAMD gain decodes at its boundary and mid-range values", "[oba][oamd]") {
    // Table 19's two ends (+15, -49) plus a value from each range, and the
    // unreachable-through-object_gain_bits 0 dB that forces the other index.
    for (const double gain : {0.0, 15.0, -49.0, 7.0, -12.0, 1.0, -1.0}) {
        CAPTURE(gain);
        const ac3::oba::Program program{
            .dynamic_only = true, .lfe = false, .dynamic_objects = 1};
        const std::array<ac3::oba::DynamicObject, 1> objects{
            {{.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain_db = gain}}};
        const auto payload = ac3::oba::build_payload(program, objects);
        const auto decoded = ac3::oba::parse_payload(payload);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->objects.size() == 1);
        CHECK(decoded->objects[0].gain_db == gain);
    }
}

TEST_CASE("OAMD parse_payload rejects what it cannot cleanly interpret", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = true, .lfe = true, .dynamic_objects = 2};
    const std::array<ac3::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.2, .y = 0.3, .z = 0.1}, .gain_db = 0.0},
        {.position = {.x = 0.8, .y = 0.7, .z = -0.2}, .gain_db = -4.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);
    REQUIRE(ac3::oba::parse_payload(payload).has_value());  // the payload under test genuinely parses

    SECTION("truncated payload") {
        for (std::size_t cut : {std::size_t{1}, payload.size() / 2, payload.size() - 1}) {
            CAPTURE(cut);
            const std::vector<std::byte> truncated(payload.begin(),
                                                   payload.begin() + static_cast<std::ptrdiff_t>(cut));
            CHECK_FALSE(ac3::oba::parse_payload(truncated).has_value());
        }
    }

    SECTION("object_count contradicts the program it describes") {
        // Flip one bit of object_count_bits (bits 2..6 of byte 0): a
        // consistency check this parser makes that a byte-for-byte inverse
        // of build_payload alone would never exercise.
        auto corrupt = payload;
        corrupt[0] ^= std::byte{0b0000'0100};
        CHECK_FALSE(ac3::oba::parse_payload(corrupt).has_value());
    }

    SECTION("an empty payload") {
        CHECK_FALSE(ac3::oba::parse_payload({}).has_value());
    }
}
