#include "ac3/mlp/restart_header.hpp"

#include <cassert>

#include "ac3/mlp/crc.hpp"

namespace ac3::mlp {

namespace {

// Everything before restart_header_CRC, in §3.3.8's field order. Shared by
// the builder and by the BitReader-based parser's CRC verification (which
// re-serialises the parsed fields and checks the CRC against that - exact
// as long as every field round-trips, which requires the reserved v(16) to
// be zero; our writer always writes zero and the spec reserves it).
std::size_t put_restart_header_body(BitWriter& body, const RestartHeader& header) {
    body.put(header.restart_sync_word, 14);
    body.put(header.output_timing, 16);
    body.put(header.min_chan, 4);
    body.put(header.max_chan, 4);
    body.put(header.max_matrix_chan, 4);
    body.put(header.dither_shift, 4);
    body.put(header.dither_seed & 0x7FFFFF, 23);
    body.put(static_cast<std::uint32_t>(header.max_shift) & 0xF, 4);  // s(4), 2's complement
    body.put(header.max_lsbs & 0x1F, 5);
    body.put(header.max_bits_a & 0x1F, 5);
    body.put(header.max_bits_b & 0x1F, 5);
    body.put(header.error_protect ? 1u : 0u, 1);
    body.put(header.lossless_check, 8);
    body.put(0, 16);  // reserved
    for (const auto assignment : header.channel_assignment) {
        body.put(assignment & 0x3F, 6);
    }
    return body.bit_count();
}

}  // namespace

std::size_t build_restart_header(BitWriter& out, const RestartHeader& header) {
    assert(is_restart_sync_word_valid(header.substream_index, header.restart_sync_word));
    assert(header.channel_assignment.size() == static_cast<std::size_t>(header.max_matrix_chan) + 1);

    BitWriter body;
    const auto bit_count = put_restart_header_body(body, header);
    const std::vector<std::byte> bytes = body.take();
    const std::uint8_t crc = restart_header_crc(bytes, bit_count);

    out.put_bits(bytes, bit_count);
    out.put(crc, 8);
    return bit_count + 8;
}

bool parse_restart_header(BitReader& r, int expected_substream_index, RestartHeader& out) {
    const auto word = static_cast<std::uint16_t>(r.read(14));
    if (!is_restart_sync_word_valid(expected_substream_index, word)) {
        return false;
    }
    out.substream_index = expected_substream_index;
    out.restart_sync_word = word;

    out.output_timing = static_cast<std::uint16_t>(r.read(16));
    out.min_chan = static_cast<std::uint8_t>(r.read(4));
    out.max_chan = static_cast<std::uint8_t>(r.read(4));
    out.max_matrix_chan = static_cast<std::uint8_t>(r.read(4));
    out.dither_shift = static_cast<std::uint8_t>(r.read(4));
    out.dither_seed = r.read(23);
    {
        // s(4), 2's complement: sign-extend from bit 3.
        const auto raw = r.read(4);
        out.max_shift = static_cast<std::int8_t>((raw & 0x8) != 0 ? static_cast<int>(raw) - 0x10
                                                                    : static_cast<int>(raw));
    }
    out.max_lsbs = static_cast<std::uint8_t>(r.read(5));
    out.max_bits_a = static_cast<std::uint8_t>(r.read(5));
    out.max_bits_b = static_cast<std::uint8_t>(r.read(5));
    out.error_protect = r.read(1) != 0;
    out.lossless_check = static_cast<std::uint8_t>(r.read(8));
    r.skip(16);  // reserved (must be zero for the CRC re-serialisation below)

    out.channel_assignment.assign(static_cast<std::size_t>(out.max_matrix_chan) + 1, 0);
    for (auto& assignment : out.channel_assignment) {
        assignment = static_cast<std::uint8_t>(r.read(6));
    }

    const auto received_crc = static_cast<std::uint8_t>(r.read(8));
    if (r.overflowed()) {
        return false;
    }

    // The header may start at any bit offset (inside block() it follows two
    // flag bits), so the covered span is not addressable as bytes of the
    // input - verify the CRC by re-serialising the parsed fields instead.
    BitWriter body;
    const auto bit_count = put_restart_header_body(body, out);
    const std::vector<std::byte> bytes = body.take();
    return restart_header_crc(bytes, bit_count) == received_crc;
}

bool parse_restart_header(std::span<const std::byte> data, int expected_substream_index,
                          RestartHeader& out) {
    BitReader r(data);
    return parse_restart_header(r, expected_substream_index, out);
}

}  // namespace ac3::mlp
