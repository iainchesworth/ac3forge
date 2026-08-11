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
    // substream_info: bit 7 (the 16-channel-presentation flag) is always 0 -
    // extra_channel_meaning() below is never emitted in v1 scope, so nothing
    // could make bit 7 true and still be self-consistent.
    w.put(info.substream_info & 0x7F, 8);

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
    w.put(0, 1);  // extra_channel_meaning_present: always 0 - v1 scope has no 16ch tier

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

    if (r.overflowed()) {
        return false;
    }
    // v1 scope never emits the 16ch tier; a stream that sets this flag needs
    // extra_channel_meaning_data()/16ch_channel_meaning() support this parser
    // doesn't have yet.
    return extra == 0;
}

}  // namespace ac3::mlp
