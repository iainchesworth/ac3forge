#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"
#include "ac3/mlp/mlp_tables.hpp"

// mlp_sync and major_sync_info(), "Dolby TrueHD (MLP) high-level bitstream
// description" §3.3.1-3.3.5 / §4.1-4.4 - including the 16-channel
// presentation's channel_meaning() extension (16ch_channel_meaning(),
// §3.3.5/§4.4), which is the static/structural half of Atmos-in-TrueHD:
// it declares which channels are loudspeaker feeds, Intermediate Spatial
// Format bed channels, and dynamic audio objects.
//
// Deliberately NOT covered here: substream_directory, substream_segment(),
// block(), restart_header(), EXTRA_DATA() (§3.3.6-3.3.9 / §4.5-4.8) - those
// carry the actual audio, and block_data() inside them is the one piece
// neither Dolby document specifies (see docs/concepts/truehd-mlp.md). This
// file covers exactly what's fully specified and buildable without it: the
// header that announces a TrueHD access unit and its channel presentations.

namespace ac3::mlp {

// §4.4: 16ch_channel_meaning(), the 16-channel presentation's description.
// Channel order on the wire is loudspeaker feeds, then Intermediate Spatial
// Format channels, then dynamic objects (§4.4.10's worked example), and the
// parts must sum to channel_count. Despite the name, §4.4.3 restricts the
// presentation to 16 channels (the count field's MSB "shall be set to 0").
struct SixteenChannelMeaning {
    std::uint8_t dialogue_norm = 31;  // u(5); 0 and 31 both mean -31 LKFS
    std::uint8_t mix_level = 0;       // u(6); peak mixing level = 70+value dB
    int channel_count = 1;            // 1..16; stored one less

    // §4.4.4: every full-bandwidth channel is a dynamic object. When set,
    // the only further question is whether an LFE channel leads (§4.4.5);
    // the content-description machinery below is skipped entirely.
    bool dyn_object_only = true;
    bool lfe_present = false;  // §4.4.5; meaningful only when dyn_object_only

    // §4.4.6, Table 17: bit 0 = loudspeaker feeds, bit 1 = Intermediate
    // Spatial Format audio, bit 2 = dynamic objects - present in that
    // order. Meaningful only when !dyn_object_only; 0 is illegal then.
    std::uint8_t content_description = 0;  // v(4)
    // §3.3.5 carries chan_distribute in the bit-0 branch but the document
    // defines no semantics for it (no §4.4.x entry) - packed structurally,
    // left false, same policy as ChannelMeaning's control-enable flags.
    bool chan_distribute = false;
    bool lfe_only = false;                 // §4.4.7: the ONLY feed is an LFE
    std::uint16_t channel_assignment = 0;  // §4.4.8 v(10), Table 18; when
                                           // feeds beyond a lone LFE exist
    std::uint8_t intermediate_spatial_format = 0;  // §4.4.9 v(3), Table 19:
                                                   // 010b/011b/100b only
    int dynamic_object_count = 0;  // §4.4.10; 1..16 when bit 2 set, stored
                                   // one less
};

// Table 18's per-bit channel counts, for consistency checks: how many
// loudspeaker-feed channels an assignment mask names.
[[nodiscard]] constexpr int sixteen_channel_assignment_count(std::uint16_t assignment) {
    constexpr int kCounts[10] = {2, 1, 1, 2, 2, 2, 2, 2, 2, 1};
    int total = 0;
    for (int bit = 0; bit < 10; ++bit) {
        if ((assignment >> bit) & 1) {
            total += kCounts[bit];
        }
    }
    return total;
}

// Table 19: the channel count of each legal Intermediate Spatial Format
// (0 for reserved codes).
[[nodiscard]] constexpr int intermediate_spatial_format_channels(std::uint8_t format) {
    switch (format) {
        case 0b010: return 10;  // BH7.3.0.0
        case 0b011: return 14;  // BH9.5.0.0
        case 0b100: return 15;  // BH7.5.3.0
        default: return 0;
    }
}

// §4.3: channel_meaning(), transcribed field-by-field. *_dialogue_norm and
// *_mix_level are per Table columns 4.3.1-4.3.8; the two enable flags this
// struct omits (2ch/6ch/8ch_control_enabled) are the write side's problem
// (build_major_sync_info sets them from which presentations are actually
// present), not something a caller states redundantly.
struct ChannelMeaning {
    // §3.3.3 lists these three flags in channel_meaning()'s syntax, but the
    // available document text does not go on to define their semantics the
    // way it does for every other field here (no §4.3.x section for any of
    // the three). Packed structurally at the bit positions the syntax gives
    // them; left false by default rather than guessed at, since this
    // codebase cites what a field does before relying on it (CONTRIBUTING.md's
    // clean-room rule) and these can't be cited yet.
    bool ch2_control_enabled = false;
    bool ch6_control_enabled = false;
    bool ch8_control_enabled = false;

    std::int8_t drc_start_up_gain = 0;  // s(7)

    std::uint8_t ch2_dialogue_norm = 31;  // u(6); 0 and 31 both mean -31 LKFS
    std::uint8_t ch2_mix_level = 0;       // u(6); peak mixing level = 70+value dB

    std::uint8_t ch6_dialogue_norm = 31;  // u(5)
    std::uint8_t ch6_mix_level = 0;       // u(6)
    std::uint8_t ch6_source_format = 0;   // v(5); 0 == not hierarchical

    std::uint8_t ch8_dialogue_norm = 31;  // u(5)
    std::uint8_t ch8_mix_level = 0;       // u(6)
    std::uint8_t ch8_source_format = 0;   // v(6); 0 == not hierarchical
};

// §4.2: major_sync_info(), transcribed field-by-field. The six/eight-channel
// presentation's *_modifier fields are raw 2-bit values rather than
// TwoChannelContent/SurroundModifier, because which enum applies depends on
// the assignment bits alongside them (§4.2.2's own "Table 5 shall only apply
// when..." / "has no meaning" language) - callers pick the right helper
// (two_channel_modifier()/surround_modifier() below) when setting one.
struct MajorSyncInfo {
    SampleRate sample_rate = SampleRate::k48000;

    TwoChannelContent two_channel_content = TwoChannelContent::kStereo;

    std::uint8_t six_channel_modifier = 0;  // 2 bits; see two_channel_modifier()/
                                              // surround_modifier() below
    Assignment six_channel_assignment = 0;   // Table 7, bits 0-4 of Location

    std::uint8_t eight_channel_modifier = 0;         // same story, Tables 8/9
    bool eight_channel_use_alternate_table = false;  // flags bit 11 (Table 11
                                                       // instead of Table 10)
    Assignment eight_channel_assignment = 0;                    // Table 10
    AlternateEightChannelAssignment eight_channel_alternate{};  // Table 11

    bool fifo_delay_constant = false;  // flags bit 15
    bool variable_rate = false;
    std::uint32_t peak_data_rate = 0;  // u(15); units of 1/16 bit per sample period
    int substreams = 1;                // u(4); substreams actually present
    std::uint8_t extended_substream_info = 0;  // u(2), Table 13
    std::uint8_t substream_info = 0;           // v(8), Tables 14-16; bit 7 (the
                                                 // 16ch-presentation flag) is
                                                 // derived: the builder sets it
                                                 // exactly when sixteen_channel
                                                 // is engaged

    ChannelMeaning meaning{};

    // When engaged, channel_meaning() grows its §4.3.9-4.3.10 extension
    // (extra_channel_meaning_present, the 4-bit word length, and
    // extra_channel_meaning_data() carrying 16ch_channel_meaning() plus
    // reserved fill) and substream_info bit 7 is set - §3.3.4's presence
    // rule couples the two, so the builder and parser enforce it.
    std::optional<SixteenChannelMeaning> sixteen_channel{};
};

// §4.2.2, Table 4: a 2-bit modifier value meaningful as content type - used
// for the 2-channel presentation always, and for the 6-/8-channel
// presentations when their assignment carries only Main L/R (Table 5/8).
[[nodiscard]] constexpr std::uint8_t two_channel_modifier(TwoChannelContent content) {
    return static_cast<std::uint8_t>(content);
}

// §4.2.2, Table 6/9: a 2-bit modifier value meaningful as a Surround EX/Pro
// Logic indicator - used for the 6-/8-channel presentations when Ls/Rs is
// the only surround pair present.
[[nodiscard]] constexpr std::uint8_t surround_modifier(SurroundModifier modifier) {
    return static_cast<std::uint8_t>(modifier);
}

// §4.1.1: the 4-bit check_nibble that makes the exclusive-OR of every 4-bit
// nibble in mlp_sync (check_nibble itself, access_unit_length's three
// nibbles, input_timing's four) equal 0xF - "in a major sync the calculation
// shall include only the items that also appear in a minor sync", so this is
// unaffected by whether major_sync_info() follows.
[[nodiscard]] AC3FORGE_EXPORT std::uint8_t compute_check_nibble(
    std::uint16_t access_unit_length, std::uint16_t input_timing);

// major_sync_info(), including its own leading format_sync and trailing
// major_sync_info_CRC (§4.2.10, ac3::mlp::major_sync_crc over everything
// before it). Byte-aligned in and out, as §3.2.1 requires of every MLP Sync
// structure.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_major_sync_info(
    const MajorSyncInfo& info);

// The independent read side, transcribed from §4.2's field list rather than
// as the inverse of build_major_sync_info() - the two are meant to disagree
// if either one gets a field wrong, not to validate each other's assumptions.
// `data` must be exactly the structure (use major_sync_info_size to find its
// end inside an access unit - the extension makes it variable-length).
// Returns false (leaving `out` partially written) if format_sync, signature
// or the CRC don't check out.
[[nodiscard]] AC3FORGE_EXPORT bool parse_major_sync_info(std::span<const std::byte> data,
                                                          MajorSyncInfo& out);

// The total byte size of the major_sync_info() starting at data[0],
// discovered from the §4.3.9-4.3.10 extension fields alone (26 fixed bytes,
// the optional (extra_channel_meaning_length + 1) 16-bit words, the CRC).
// Returns 0 when `data` is too short to hold the structure it declares.
// This is how an access-unit parser finds where the substream directory
// starts; nothing before the CRC states the length explicitly.
[[nodiscard]] AC3FORGE_EXPORT std::size_t major_sync_info_size(std::span<const std::byte> data);

}  // namespace ac3::mlp
