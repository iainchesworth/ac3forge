#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"
#include "ac3/mlp/mlp_tables.hpp"

// restart_header(), "Dolby TrueHD (MLP) high-level bitstream description"
// §3.3.8 / §4.7.2 - the per-substream initialization block a restart point
// carries. Every field's bit position and width comes from §3.3.8's syntax
// table, which is complete; but §4.7.2's own prose only defines
// output_timing, min_chan, max_chan, error_protect and restart_header_CRC -
// it jumps straight from max_chan to error_protect, so dither_shift,
// dither_seed, max_shift, max_lsbs, the two max_bits fields, lossless_check
// and ch_assign[]'s per-entry meaning have no defining prose in either Dolby
// document. Their likely purpose is cross-referenced from US 7,193,538 B2
// (see docs/concepts/truehd-mlp.md's "What the two patents actually
// describe") rather than confirmed spec text - packed here at the position
// and width §3.3.8 gives them regardless, since that part IS fully
// specified, with the uncertainty called out per field below.

namespace ac3::mlp {

struct RestartHeader {
    // §4.7.2, Table 20: which substream this restart header belongs to -
    // constrains which restart_sync_word values build_restart_header()
    // accepts (is_restart_sync_word_valid()), not itself a packed field.
    int substream_index = 0;
    std::uint16_t restart_sync_word = kRestartSyncWordSubstream0;

    std::uint16_t output_timing = 0;  // u(16); §4.7.2: sample number (mod 65536)
                                        // of this block's first sample

    // u(4) each. §4.7.2 defines both as "one less than the minimum/maximum
    // channel number carried by the substream" - these fields hold that
    // already-decremented value, not the channel number itself, since
    // nothing here needs the distinction and re-adding one only to
    // subtract it back at pack time would just be a chance to get it wrong.
    std::uint8_t min_chan = 0;
    std::uint8_t max_chan = 0;
    // u(4): loop bound for channel_assignment below - ch_assign[ch] for
    // ch = 0..max_matrix_chan inclusive, per §3.3.8's own for-loop, so
    // channel_assignment.size() must equal max_matrix_chan + 1.
    std::uint8_t max_matrix_chan = 0;

    // The following six fields (u(4)/u(23)/s(4)/u(5)/u(5)/u(5)) are packed
    // at §3.3.8's positions with no confirmed semantics beyond the patent
    // cross-reference in the header comment above - treat every name here
    // as provisional.
    std::uint8_t dither_shift = 0;      // u(4)
    std::uint32_t dither_seed = 0;      // u(23); likely the patent's diamond-dither seed
    std::int8_t max_shift = 0;          // s(4); likely the patent's output_shift (overload headroom)
    std::uint8_t max_lsbs = 0;          // u(5); likely the patent's LSB-bypass extra-precision count
    std::uint8_t max_bits_a = 0;        // u(5); first of §3.3.8's two identically-named max_bits fields
    std::uint8_t max_bits_b = 0;        // u(5); second of the two - which (if either) is matrix vs.
                                          // prediction coefficient bit depth is not confirmed

    bool error_protect = false;  // §4.7.2: enables block_data_bits/block_header_CRC in block()
    std::uint8_t lossless_check = 0;  // u(8); likely the patent's 8-bit output-verification parity

    // ch_assign[0..max_matrix_chan], u(6) each. size() must equal
    // max_matrix_chan + 1; build_restart_header() asserts this.
    std::vector<std::uint8_t> channel_assignment;
};

// Appends restart_header() - including its own trailing restart_header_CRC -
// to an ongoing access unit's BitWriter via put_bits(), since the CRC's
// covered span is generally not a whole number of bytes (see crc.hpp's
// restart_header_crc). Returns the number of bits appended, so a caller
// composing a larger structure knows the exact extent without having to
// recompute it.
[[nodiscard]] AC3FORGE_EXPORT std::size_t build_restart_header(BitWriter& out,
                                                                const RestartHeader& header);

// The independent read side, transcribed from §3.3.8's field list rather
// than as the inverse of build_restart_header(). Returns false (leaving
// `out` partially written) if the restart_sync_word isn't valid for
// `expected_substream_index` or the CRC doesn't check out.
//
// The BitReader form consumes the header from the reader's CURRENT bit
// position - which inside block() is two flag bits past a 16-bit boundary,
// never byte-aligned, so the CRC is verified by re-serialising the parsed
// fields rather than over the raw input (exact as long as the reserved
// v(16) is zero, which the spec reserves and our writer guarantees). The
// span form is a convenience wrapper for a header starting at byte offset
// zero.
[[nodiscard]] AC3FORGE_EXPORT bool parse_restart_header(BitReader& r,
                                                         int expected_substream_index,
                                                         RestartHeader& out);
[[nodiscard]] AC3FORGE_EXPORT bool parse_restart_header(std::span<const std::byte> data,
                                                         int expected_substream_index,
                                                         RestartHeader& out);

}  // namespace ac3::mlp
