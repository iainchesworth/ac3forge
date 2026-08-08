#include "ac3/oba/oamd.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ac3/core/bitwriter.hpp"

namespace ac3::oba {

namespace {

// §5.5.1 variable_bits_max(n, max_num_groups). Same construction as EMDF's
// variable_bits - groups of n bits, each followed by read_more, with the
// group offset folded in - but the group count is capped, so the last group
// carries no read_more of any consequence and the field cannot run away.
void put_variable_bits_max(BitWriter& w, std::uint32_t value, int group_bits,
                           int max_groups) {
    int groups = 1;
    std::uint64_t offset = 0;
    while (groups < max_groups) {
        const std::uint64_t capacity = std::uint64_t{1} << (groups * group_bits);
        if (value < offset + capacity) {
            break;
        }
        offset += capacity;
        ++groups;
    }
    const auto encoded = static_cast<std::uint64_t>(value) - offset;
    for (int group = groups - 1; group >= 0; --group) {
        w.put(static_cast<std::uint32_t>((encoded >> (group * group_bits)) &
                                         ((std::uint64_t{1} << group_bits) - 1)),
              group_bits);
        w.put(group == 0 ? 0u : 1u, 1);  // read_more
    }
}

// The flag arrays in §5.6 are transmitted with array index 0 FIRST, so element
// n lands at bit (width - 1 - n) of the field, not at bit n.
//
// The tables read the other way round - Table 11a numbers the bed flag 3 and
// Table 12 numbers L/R 9 - which is what made the opposite reading look
// natural. Dolby's own encoder settles it: for a 7.1.4 input it writes
// bed_channel_assignment 0b0010111111, and only index-0-first turns that into
// L/R, C, LFE, Ls/Rs, Lb/Rb, Tfl/Tfr, Tbl/Tbr - twelve channels, which is
// exactly the object_count of 12 it declares in the same payload. Under the
// other reading the same bits name a bed with no left, right or centre.
[[nodiscard]] std::uint32_t flags_msb_first(std::uint32_t flags, int width) {
    std::uint32_t out = 0;
    for (int index = 0; index < width; ++index) {
        if (flags & (1u << index)) {
            out |= 1u << (width - 1 - index);
        }
    }
    return out;
}

// §5.6.1.1.8 and §5.6.1.1.9: x and y are the 6-bit field over 62, so 62 - not
// 63 - is the far wall and the two codes above it also mean "at the wall".
// Sending 63 would be legal and would still decode to 1 through the min(), but
// it wastes the only two codes a future extension could use.
[[nodiscard]] std::uint32_t quantize_xy(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return static_cast<std::uint32_t>(std::lround(clamped * 62.0));
}

// §5.6.1.1.10 / §5.6.1.1.11: z is a sign bit and a 4-bit magnitude over 15.
// pos3D_Z_sign_bits == 0 means negative, so the floor is the zero code.
void put_z(BitWriter& w, double value) {
    const double clamped = std::clamp(value, -1.0, 1.0);
    w.put(clamped < 0.0 ? 0u : 1u, 1);
    w.put(static_cast<std::uint32_t>(std::lround(std::abs(clamped) * 15.0)), 4);
}

// §5.6.1.4, Table 18 and Table 19. The two ranges are not contiguous and the
// break is not where it looks: codes 0..14 mean 15 - code dB (so +15 down to
// +1) and codes 15..63 mean 14 - code dB (so -1 down to -49). Exactly 0 dB
// falls in neither, which is what object_gain_idx 0 is for.
void put_gain(BitWriter& w, double gain_db) {
    const long rounded = std::lround(gain_db);
    if (rounded == 0) {
        w.put(0b00, 2);  // object_gain_idx: 0 dB
        return;
    }
    w.put(0b10, 2);  // object_gain_idx: an explicit object_gain_bits follows
    const long clamped = std::clamp(rounded, -49L, 15L);
    const long code = clamped > 0 ? 15 - clamped : 14 - clamped;
    w.put(static_cast<std::uint32_t>(std::clamp(code, 0L, 63L)), 6);
}

// §5.5.9 object_info_block for the single update block this encoder sends.
// Everything that block 0 implies is implied here too, so nothing that the
// status indices would have carried is written: §5.5.9 fixes
// object_basic_info_status_idx to 0b01 for blk == 0, and §5.5.10 turns that
// into "both fields present" without a bit on the wire.
void put_object_info_block(BitWriter& w, const DynamicObject* dynamic) {
    w.put(0, 1);  // b_object_not_active: every object carries essence

    // object_basic_info(), status 0b01 => object_basic_info[] = {true, true}.
    put_gain(w, dynamic ? dynamic->gain_db : 0.0);
    w.put(1, 1);  // b_default_object_priority: 1,0

    // §5.5.9 again: an object in a bed has its position from its speaker
    // label, so object_render_info_status_idx is forced to 0b00 and the whole
    // block is absent. Only dynamic objects describe themselves.
    if (dynamic != nullptr) {
        // object_render_info(), status 0b01 => obj_render_info[] all true.
        // blk == 0 forces b_differential_position_specified to FALSE, and it
        // is implied rather than transmitted.
        w.put(quantize_xy(dynamic->position.x), 6);
        w.put(quantize_xy(dynamic->position.y), 6);
        put_z(w, dynamic->position.z);
        w.put(0, 1);  // b_object_distance_specified: inside the room

        w.put(0, 3);  // zone_constraints_idx: no constraints
        w.put(1, 1);  // b_enable_elevation: the Top-Bottom zone is in play

        w.put(0b00, 2);  // object_size_idx: a point source

        w.put(0, 1);  // b_object_use_screen_ref: room-anchored, not screen
        w.put(0, 1);  // b_object_snap: no channel lock
    }

    w.put(0, 1);  // b_additional_table_data_exists
}

// §5.5.5 object_element. Written into whatever writer it is handed, so it can
// be emitted once into a probe to measure it and once into the payload for
// real - the element's bits do not start on a byte boundary, so they cannot be
// copied through a byte buffer without picking up padding that is not theirs.
void put_object_element(BitWriter& w, const Program& program,
                        std::span<const DynamicObject> objects) {
    // Objects that are anchored rather than placed: the LFE in the
    // dynamic-only branch, or the whole bed instance otherwise. They come
    // first (§5.6.4.8) and carry no render info.
    const int anchored = object_count(program) - program.dynamic_objects;
    const int total = object_count(program);

    // md_update_info (§5.5.6). One update per frame, aligned to its first
    // sample: sample_offset_code 0 is sample_offset 0, and
    // num_obj_info_blocks_bits 0 is a single block.
    w.put(0b00, 2);  // sample_offset_code
    w.put(0, 3);     // num_obj_info_blocks_bits => 1 block
    // block_update_info (§5.5.7).
    w.put(0, 6);     // block_offset_factor_bits: the update starts the frame
    // Table 24 code 2 is a 1 536-sample ramp - exactly one frame - so object
    // properties interpolate across the frame instead of stepping at its edge.
    // Code 0 would be a jump, and a moving object would zipper.
    w.put(0b10, 2);  // ramp_duration_code

    w.put(1, 1);  // b_reserved_data_not_present

    for (int object = 0; object < total; ++object) {
        const bool is_anchored = object < anchored;
        put_object_info_block(
            w, is_anchored ? nullptr
                           : &objects[static_cast<std::size_t>(object - anchored)]);
    }
}

}  // namespace

std::vector<std::byte> build_payload(const Program& program,
                                     std::span<const DynamicObject> objects) {
    assert(static_cast<int>(objects.size()) == program.dynamic_objects);
    const int total = object_count(program);

    // §5.6.4.3: oa_element_size counts b_discard_unknown_element, the element
    // and its padding, so the flag bit is inside the measurement and the
    // padding is whatever rounds the three of them up to whole bytes.
    const std::size_t element_bits = [&] {
        BitWriter probe;
        put_object_element(probe, program, objects);
        return probe.bit_count() + 1;
    }();
    const auto element_bytes = static_cast<std::uint32_t>((element_bits + 7) / 8);
    const std::size_t element_padding = element_bytes * 8 - element_bits;

    BitWriter w;
    // --- object_audio_metadata_payload (§5.5.2) ---
    w.put(0, 2);  // oa_md_version_bits
    assert(total >= 1 && total <= 31);
    w.put(static_cast<std::uint32_t>(total - 1), 5);  // object_count_bits

    // --- program_assignment (§5.5.3) ---
    if (program.dynamic_only) {
        // The whole branch is two bits. The object count is object_count
        // above; the number of dynamic objects is what is left after the LFE.
        w.put(1, 1);  // b_dyn_object_only_program
        w.put(program.lfe ? 1 : 0, 1);  // b_lfe_present
    } else {
        w.put(0, 1);
        // content_description[] (Table 11a): element 3 a bed instance, 2 ISF,
        // 1 dynamic objects, 0 reserved - written index 0 first, so the bed
        // flag is the LAST bit of the four.
        const std::uint32_t content = (program.bed != 0 ? 1u << 3 : 0u) |
                                      (program.dynamic_objects > 0 ? 1u << 1 : 0u);
        w.put(flags_msb_first(content, 4), 4);
        if (program.bed != 0) {
            w.put(0, 1);  // b_bed_chan_distribute
            w.put(0, 1);  // b_multiple_bed_instances_present => one instance
            // §5.6.1.1.6: b_lfe_only is not a shorthand for the assignment, it
            // REPLACES it - the 10-bit field is absent entirely.
            const bool lfe_only = program.bed == bed::kLfe;
            w.put(lfe_only ? 1 : 0, 1);  // b_lfe_only
            if (!lfe_only) {
                w.put(1, 1);  // b_standard_chan_assign
                w.put(flags_msb_first(program.bed, 10), 10);
            }
        }
        if (program.dynamic_objects > 0) {
            w.put(static_cast<std::uint32_t>(program.dynamic_objects - 1), 5);
        }
    }

    w.put(0, 1);  // b_alternate_object_data_present
    w.put(1, 4);  // oa_element_count_bits: one element, the object_element

    // --- oa_element_md (§5.5.4) ---
    w.put(1, 4);  // oa_element_id_idx: object_element (Table 26)
    put_variable_bits_max(w, element_bytes - 1, 4, 4);  // oa_element_size_bits
    // A decoder that does not know this element can skip it by its size, so
    // there is no reason to make it throw the payload away.
    w.put(0, 1);  // b_discard_unknown_element
    put_object_element(w, program, objects);
    for (std::size_t bit = 0; bit < element_padding; ++bit) {
        w.put(0, 1);  // §5.6.4.14 padding: zero bits, counted by the size
    }

    // §5.5.2's own trailing padding. It is NOT the same as the element's: the
    // element is a whole number of bytes measured from a bit offset that is
    // not itself byte-aligned, so the payload still has a few bits to go.
    return w.take();
}

}  // namespace ac3::oba
