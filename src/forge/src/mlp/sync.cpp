#include "ac3/mlp/sync.hpp"

#include <cassert>

#include "ac3/mlp/crc.hpp"

namespace ac3::mlp {

namespace {

// §4.2.2: format_info's 13-bit 8ch_presentation_channel_assignment field,
// packed one of two ways depending on flags bit 11 - Table 10's own bit
// layout when clear, or Table 11's 5-bit layout (zero-extended into the same
// 13-bit field, bits 5-12 reserved) when set.
[[nodiscard]] std::uint16_t eight_channel_assignment_bits(const MajorSyncInfo& info) {
    if (info.eight_channel_use_alternate_table) {
        return info.eight_channel_alternate.bits();
    }
    return info.eight_channel_assignment & ((Assignment{1} << kEightChannelAssignmentBits) - 1);
}

// §3.3.5's exact bit length of 16ch_channel_meaning() for these field
// values - needed up front because §3.3.4's reserved-fill formula sizes the
// extension around it.
[[nodiscard]] int sixteen_channel_meaning_bits(const SixteenChannelMeaning& m) {
    int bits = 5 + 6 + 5 + 1;  // dialogue_norm, mix_level, channel_count, dyn_object_only
    if (m.dyn_object_only) {
        return bits + 1;  // lfe_present
    }
    bits += 4;  // 16ch_content_description
    if ((m.content_description & 0x1) != 0) {
        bits += 3;  // chan_distribute, reserved, lfe_only
        if (!m.lfe_only) {
            bits += 1 + 10;  // reserved ('1'), 16ch_channel_assignment
        }
    }
    if ((m.content_description & 0x2) != 0) {
        bits += 3;  // 16ch_intermediate_spatial_format
    }
    if ((m.content_description & 0x4) != 0) {
        bits += 5;  // 16ch_dynamic_object_count
    }
    return bits;
}

// The channel-count consistency §4.4.10 states ("the total number of
// loudspeaker feed, intermediate spatial format and dynamic object channels
// is equal to ... 16ch_channel_count"), shared by the builder's assert and
// the parser's rejection.
[[nodiscard]] bool sixteen_channel_meaning_consistent(const SixteenChannelMeaning& m) {
    if (m.channel_count < 1 || m.channel_count > 16) {
        return false;
    }
    if (m.dyn_object_only) {
        return true;  // count covers the LFE (if any) plus objects
    }
    if (m.content_description == 0 || m.content_description > 0b111) {
        return false;  // Table 17: 0000 illegal, 1000-1111 reserved
    }
    if (m.content_description == 0b100) {
        return false;  // Table 17: objects-only says dyn_object_only instead
    }
    int total = 0;
    if ((m.content_description & 0x1) != 0) {
        total += m.lfe_only ? 1 : sixteen_channel_assignment_count(m.channel_assignment);
    }
    if ((m.content_description & 0x2) != 0) {
        const auto isf = intermediate_spatial_format_channels(m.intermediate_spatial_format);
        if (isf == 0) {
            return false;  // Table 19 reserved code
        }
        total += isf;
    }
    if ((m.content_description & 0x4) != 0) {
        if (m.dynamic_object_count < 1 || m.dynamic_object_count > 16) {
            return false;
        }
        total += m.dynamic_object_count;
    }
    return total == m.channel_count;
}

void put_sixteen_channel_meaning(BitWriter& w, const SixteenChannelMeaning& m) {
    w.put(m.dialogue_norm & 0x1F, 5);
    w.put(m.mix_level & 0x3F, 6);
    w.put(static_cast<std::uint32_t>(m.channel_count - 1), 5);
    w.put(m.dyn_object_only ? 1u : 0u, 1);
    if (m.dyn_object_only) {
        w.put(m.lfe_present ? 1u : 0u, 1);
        return;
    }
    w.put(m.content_description & 0xF, 4);
    if ((m.content_description & 0x1) != 0) {
        w.put(m.chan_distribute ? 1u : 0u, 1);
        w.put(0, 1);  // reserved
        w.put(m.lfe_only ? 1u : 0u, 1);
        if (!m.lfe_only) {
            w.put(1, 1);  // reserved, "set to '1'" (§3.3.5)
            w.put(m.channel_assignment & 0x3FF, 10);
        }
    }
    if ((m.content_description & 0x2) != 0) {
        w.put(m.intermediate_spatial_format & 0x7, 3);
    }
    if ((m.content_description & 0x4) != 0) {
        w.put(static_cast<std::uint32_t>(m.dynamic_object_count - 1), 5);
    }
}

[[nodiscard]] bool read_sixteen_channel_meaning(BitReader& r, SixteenChannelMeaning& m) {
    m.dialogue_norm = static_cast<std::uint8_t>(r.read(5));
    m.mix_level = static_cast<std::uint8_t>(r.read(6));
    m.channel_count = static_cast<int>(r.read(5)) + 1;
    m.dyn_object_only = r.read(1) != 0;
    if (m.dyn_object_only) {
        m.lfe_present = r.read(1) != 0;
    } else {
        m.content_description = static_cast<std::uint8_t>(r.read(4));
        if ((m.content_description & 0x1) != 0) {
            m.chan_distribute = r.read(1) != 0;
            r.skip(1);  // reserved
            m.lfe_only = r.read(1) != 0;
            if (!m.lfe_only) {
                if (r.read(1) != 1) {
                    return false;  // reserved, "set to '1'"
                }
                m.channel_assignment = static_cast<std::uint16_t>(r.read(10));
            }
        }
        if ((m.content_description & 0x2) != 0) {
            m.intermediate_spatial_format = static_cast<std::uint8_t>(r.read(3));
        }
        if ((m.content_description & 0x4) != 0) {
            m.dynamic_object_count = static_cast<int>(r.read(5)) + 1;
        }
    }
    return !r.overflowed() && sixteen_channel_meaning_consistent(m);
}

}  // namespace

std::uint8_t compute_check_nibble(std::uint16_t access_unit_length, std::uint16_t input_timing) {
    // §4.1.1: the XOR of every 4-bit nibble in mlp_sync - check_nibble
    // itself plus access_unit_length's 3 nibbles and input_timing's 4 -
    // equals 0xF. Accumulate the 7 known nibbles, then solve for the one
    // that makes the total 0xF.
    std::uint8_t known = 0;
    for (int shift = 8; shift >= 0; shift -= 4) {
        known ^= static_cast<std::uint8_t>((access_unit_length >> shift) & 0xF);
    }
    for (int shift = 12; shift >= 0; shift -= 4) {
        known ^= static_cast<std::uint8_t>((input_timing >> shift) & 0xF);
    }
    return static_cast<std::uint8_t>(0xF ^ known);
}

std::vector<std::byte> build_major_sync_info(const MajorSyncInfo& info) {
    BitWriter w;

    // format_sync - v(32)
    w.put(kFormatSync, 32);

    // format_info - v(32), Table 2
    w.put(static_cast<std::uint32_t>(info.sample_rate), 4);  // audio_sampling_frequency
    w.put(0, 1);                                              // 6ch_multichannel_type: standard layout
    w.put(0, 1);                                              // 8ch_multichannel_type: standard layout
    w.put(0, 2);                                              // reserved
    w.put(two_channel_modifier(info.two_channel_content), 2);  // 2ch_presentation_channel_modifier
    w.put(info.six_channel_modifier & 0b11, 2);                // 6ch_presentation_channel_modifier
    w.put(info.six_channel_assignment & ((Assignment{1} << kSixChannelAssignmentBits) - 1),
          kSixChannelAssignmentBits);                          // 6ch_presentation_channel_assignment
    w.put(info.eight_channel_modifier & 0b11, 2);               // 8ch_presentation_channel_modifier
    w.put(eight_channel_assignment_bits(info), kEightChannelAssignmentBits);

    // signature - v(16)
    w.put(kSignature, 16);

    // flags - v(16), Table 12
    w.put(info.fifo_delay_constant ? 1u : 0u, 1);                       // bit 15 (MSB)
    w.put(0, 3);                                                        // bits 14-12, reserved
    w.put(info.eight_channel_use_alternate_table ? 1u : 0u, 1);         // bit 11
    w.put(0, 11);                                                       // bits 10-0, reserved

    w.put(0, 16);  // reserved

    w.put(info.variable_rate ? 1u : 0u, 1);        // variable_rate
    w.put(info.peak_data_rate & 0x7FFF, 15);       // peak_data_rate
    assert(info.substreams >= 1 && info.substreams <= 15);
    w.put(static_cast<std::uint32_t>(info.substreams), 4);  // substreams
    w.put(0, 2);                                             // reserved
    w.put(info.extended_substream_info & 0b11, 2);           // extended_substream_info
    // substream_info: bit 7 (the 16-channel-presentation flag) is derived
    // from whether the 16ch tier is engaged - §3.3.4 gates
    // 16ch_channel_meaning() on exactly this bit, so letting a caller set
    // them independently could only create self-contradictions.
    const std::uint32_t substream_info =
        (info.substream_info & 0x7F) | (info.sixteen_channel ? 0x80u : 0u);
    w.put(substream_info, 8);

    // channel_meaning() - §3.3.3 / §4.3
    w.put(0, 6);  // reserved
    w.put(info.meaning.ch2_control_enabled ? 1u : 0u, 1);
    w.put(info.meaning.ch6_control_enabled ? 1u : 0u, 1);
    w.put(info.meaning.ch8_control_enabled ? 1u : 0u, 1);
    w.put(0, 1);  // reserved
    w.put(static_cast<std::uint32_t>(info.meaning.drc_start_up_gain) & 0x7F, 7);  // s(7), 2's complement
    w.put(info.meaning.ch2_dialogue_norm & 0x3F, 6);
    w.put(info.meaning.ch2_mix_level & 0x3F, 6);
    w.put(info.meaning.ch6_dialogue_norm & 0x1F, 5);
    w.put(info.meaning.ch6_mix_level & 0x3F, 6);
    w.put(info.meaning.ch6_source_format & 0x1F, 5);
    w.put(info.meaning.ch8_dialogue_norm & 0x1F, 5);
    w.put(info.meaning.ch8_mix_level & 0x3F, 6);
    w.put(info.meaning.ch8_source_format & 0x3F, 6);
    w.put(0, 1);  // reserved
    w.put(info.sixteen_channel ? 1u : 0u, 1);  // extra_channel_meaning_present

    if (info.sixteen_channel) {
        assert(sixteen_channel_meaning_consistent(*info.sixteen_channel));
        // §4.3.10: the extension's expansion (the 4-bit length field plus
        // extra_channel_meaning_data()) is a whole number of 16-bit words,
        // the field holding one less than that count; §3.3.4's reserved
        // fill makes up the difference after 16ch_channel_meaning().
        const int meaning_bits = sixteen_channel_meaning_bits(*info.sixteen_channel);
        const int words = (4 + meaning_bits + 15) / 16;
        w.put(static_cast<std::uint32_t>(words - 1), 4);  // extra_channel_meaning_length
        put_sixteen_channel_meaning(w, *info.sixteen_channel);
        const int reserved = words * 16 - 4 - meaning_bits;
        if (reserved > 0) {
            w.put(0, reserved);
        }
    }

    // major_sync_info_CRC - v(16), over everything above.
    const std::vector<std::byte> body = w.take();
    const std::uint16_t crc = major_sync_crc(body);

    std::vector<std::byte> out = body;
    out.push_back(static_cast<std::byte>(crc >> 8));
    out.push_back(static_cast<std::byte>(crc & 0xFF));
    return out;
}

bool parse_major_sync_info(std::span<const std::byte> data, MajorSyncInfo& out) {
    if (data.size() < 2) {
        return false;
    }
    const std::uint16_t received_crc =
        (std::to_integer<std::uint16_t>(data[data.size() - 2]) << 8) |
        std::to_integer<std::uint16_t>(data[data.size() - 1]);
    if (major_sync_crc(data.first(data.size() - 2)) != received_crc) {
        return false;
    }

    BitReader r(data);

    if (r.read(32) != kFormatSync) {
        return false;
    }

    const auto sample_rate_code = r.read(4);
    switch (sample_rate_code) {
        case static_cast<std::uint32_t>(SampleRate::k48000):
        case static_cast<std::uint32_t>(SampleRate::k96000):
        case static_cast<std::uint32_t>(SampleRate::k192000):
        case static_cast<std::uint32_t>(SampleRate::k44100):
        case static_cast<std::uint32_t>(SampleRate::k88200):
        case static_cast<std::uint32_t>(SampleRate::k176400):
            out.sample_rate = static_cast<SampleRate>(sample_rate_code);
            break;
        default: return false;  // reserved code
    }
    r.skip(1);  // 6ch_multichannel_type
    r.skip(1);  // 8ch_multichannel_type
    r.skip(2);  // reserved

    out.two_channel_content = static_cast<TwoChannelContent>(r.read(2));
    out.six_channel_modifier = static_cast<std::uint8_t>(r.read(2));
    out.six_channel_assignment = static_cast<Assignment>(r.read(kSixChannelAssignmentBits));
    out.eight_channel_modifier = static_cast<std::uint8_t>(r.read(2));
    const auto eight_channel_bits = r.read(kEightChannelAssignmentBits);

    if (r.read(16) != kSignature) {
        return false;
    }

    const auto flags = r.read(16);
    out.fifo_delay_constant = ((flags >> 15) & 1) != 0;
    out.eight_channel_use_alternate_table = ((flags >> 11) & 1) != 0;

    if (out.eight_channel_use_alternate_table) {
        out.eight_channel_alternate.main = ((eight_channel_bits >> 0) & 1) != 0;
        out.eight_channel_alternate.centre = ((eight_channel_bits >> 1) & 1) != 0;
        out.eight_channel_alternate.lfe = ((eight_channel_bits >> 2) & 1) != 0;
        out.eight_channel_alternate.surround = ((eight_channel_bits >> 3) & 1) != 0;
        out.eight_channel_alternate.top_side = ((eight_channel_bits >> 4) & 1) != 0;
        out.eight_channel_assignment = 0;
    } else {
        out.eight_channel_assignment = static_cast<Assignment>(eight_channel_bits);
        out.eight_channel_alternate = AlternateEightChannelAssignment{};
    }

    r.skip(16);  // reserved

    out.variable_rate = r.read(1) != 0;
    out.peak_data_rate = r.read(15);
    out.substreams = static_cast<int>(r.read(4));
    r.skip(2);  // reserved
    out.extended_substream_info = static_cast<std::uint8_t>(r.read(2));
    out.substream_info = static_cast<std::uint8_t>(r.read(8));

    r.skip(6);  // reserved
    out.meaning.ch2_control_enabled = r.read(1) != 0;
    out.meaning.ch6_control_enabled = r.read(1) != 0;
    out.meaning.ch8_control_enabled = r.read(1) != 0;
    r.skip(1);  // reserved
    {
        // s(7), 2's complement: sign-extend from bit 6.
        const auto raw = r.read(7);
        out.meaning.drc_start_up_gain =
            static_cast<std::int8_t>((raw & 0x40) != 0 ? static_cast<int>(raw) - 0x80
                                                        : static_cast<int>(raw));
    }
    out.meaning.ch2_dialogue_norm = static_cast<std::uint8_t>(r.read(6));
    out.meaning.ch2_mix_level = static_cast<std::uint8_t>(r.read(6));
    out.meaning.ch6_dialogue_norm = static_cast<std::uint8_t>(r.read(5));
    out.meaning.ch6_mix_level = static_cast<std::uint8_t>(r.read(6));
    out.meaning.ch6_source_format = static_cast<std::uint8_t>(r.read(5));
    out.meaning.ch8_dialogue_norm = static_cast<std::uint8_t>(r.read(5));
    out.meaning.ch8_mix_level = static_cast<std::uint8_t>(r.read(6));
    out.meaning.ch8_source_format = static_cast<std::uint8_t>(r.read(6));
    r.skip(1);                     // reserved
    const auto extra = r.read(1);  // extra_channel_meaning_present

    // §3.3.4 gates 16ch_channel_meaning() on substream_info bit 7 - the
    // flag and the extension must agree in both directions.
    out.sixteen_channel.reset();
    const bool sixteen_flag = (out.substream_info & 0x80) != 0;
    if (extra != 0) {
        const auto length = r.read(4);  // extra_channel_meaning_length
        const auto data_bits = (length + 1) * 16 - 4;
        const auto start = r.bit_position();
        if (sixteen_flag) {
            SixteenChannelMeaning meaning;
            if (!read_sixteen_channel_meaning(r, meaning)) {
                return false;
            }
            const auto used = r.bit_position() - start;
            if (used > data_bits) {
                return false;  // meaning overran its declared extension
            }
            r.skip(static_cast<int>(data_bits - used));  // reserved fill
            out.sixteen_channel = meaning;
        } else {
            // Extension data with no 16-channel presentation: §3.3.4 calls
            // the whole area reserved - skip it, don't reject it.
            r.skip(static_cast<int>(data_bits));
        }
    } else if (sixteen_flag) {
        return false;  // bit 7 promises channel meaning data that isn't here
    }

    if (r.overflowed()) {
        return false;
    }
    // Everything before the CRC must have been consumed - a mismatch means
    // the caller sliced a different length than the fields describe.
    return r.bit_position() == (data.size() - 2) * 8;
}

std::size_t major_sync_info_size(std::span<const std::byte> data) {
    // Fixed shape: 144 bits through substream_info, then channel_meaning's
    // 64-bit fixed part = 26 bytes, then the CRC. The extension flag is the
    // 64-bit part's final bit (byte 25, LSB) and the 4-bit word count
    // follows immediately (byte 26, high nibble).
    constexpr std::size_t kFixedTotal = 28;
    if (data.size() < kFixedTotal) {
        return 0;
    }
    if ((std::to_integer<std::uint8_t>(data[25]) & 0x01) == 0) {
        return kFixedTotal;
    }
    const auto length = std::to_integer<std::uint8_t>(data[26]) >> 4;
    const std::size_t total = 26 + (static_cast<std::size_t>(length) + 1) * 2 + 2;
    return data.size() >= total ? total : 0;
}

}  // namespace ac3::mlp
