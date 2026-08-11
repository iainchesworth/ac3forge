#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"
#include "ac3/mlp/mlp_tables.hpp"

// mlp_sync and major_sync_info(), "Dolby TrueHD (MLP) high-level bitstream
// description" §3.3.1-3.3.3 / §4.1-4.3.
//
// Deliberately NOT covered here: substream_directory, substream_segment(),
// block(), restart_header(), EXTRA_DATA() (§3.3.6-3.3.9 / §4.5-4.8) - those
// carry the actual audio, and block_data() inside them is the one piece
// neither Dolby document specifies (see docs/concepts/truehd-mlp.md). This
// file covers exactly what's fully specified and buildable without it: the
// header that announces a TrueHD access unit and its channel presentations.
//
// Also deliberately NOT covered: the 16-channel presentation
// (16ch_channel_meaning(), §3.3.5/§4.4) and therefore Atmos-in-TrueHD's
// channel-assignment metadata - v1 scope per the design doc. Every
// MajorSyncInfo this module builds packs extra_channel_meaning_present as 0.

namespace ac3::mlp {

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
                                                 // always packed as 0 - v1 scope

    ChannelMeaning meaning{};
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
// Returns false (leaving `out` partially written) if format_sync, signature
// or the CRC don't check out.
[[nodiscard]] AC3FORGE_EXPORT bool parse_major_sync_info(std::span<const std::byte> data,
                                                          MajorSyncInfo& out);

}  // namespace ac3::mlp
