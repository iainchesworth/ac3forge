#include "mpegts/mpegts.hpp"

#include <algorithm>
#include <array>
#include <cassert>

namespace mpegts {

namespace {

using Bytes = std::vector<std::byte>;

constexpr std::size_t kTsPacketSize = 188;
constexpr std::uint8_t kSyncByte = 0x47;
constexpr std::uint16_t kPatPid = 0x0000;

// ISO/IEC 13818-1 Table 2-34: 0x06 is "ITU-T Rec. H.222.0 | ISO/IEC 13818-1
// PES packets containing private data" - not an MPEG audio stream_type at
// all. This is deliberate: DVB does not register a stream_type of its own
// for AC-3/E-AC-3 (unlike ATSC's 0x81), so a DVB-conformant demuxer is
// expected to find 0x06 plus one of the descriptors below in the PMT and
// treat the PID as AC-3/E-AC-3 on that basis alone. See mpegts.hpp's header
// comment for why this project implements the DVB profile rather than
// ATSC's.
constexpr std::uint8_t kStreamTypePrivateData = 0x06;

// ISO/IEC 13818-1 Table 2-22's audio stream_id range ('110x xxxx') does not
// apply here - stream_type 0x06 payloads are carried as PES private data,
// which Table 2-19 puts under stream_id 0xBD (private_stream_1). Unlike a
// DVD-Video private_stream_1, DVB's use of it for AC-3 carries no extra
// sub-stream-id byte in front of the payload (that convention is specific to
// DVD-Video's own multiplexing rules, not ISO/IEC 13818-1 or DVB's use of
// it) - the PES payload here is the raw AC-3/E-AC-3 access unit, nothing
// else.
constexpr std::uint8_t kPesStreamIdPrivateStream1 = 0xBD;

// ETSI EN 300 468 Annex D.2, Table D.1.
constexpr std::uint8_t kTagAc3Descriptor = 0x6A;
// ETSI EN 300 468 Annex D.4, Table D.3.
constexpr std::uint8_t kTagEnhancedAc3Descriptor = 0x7A;

void put_byte(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_be16(Bytes& out, std::uint16_t value) {
    put_byte(out, static_cast<std::uint8_t>(value >> 8));
    put_byte(out, static_cast<std::uint8_t>(value & 0xFF));
}

void put_be32(Bytes& out, std::uint32_t value) {
    put_byte(out, static_cast<std::uint8_t>(value >> 24));
    put_byte(out, static_cast<std::uint8_t>(value >> 16));
    put_byte(out, static_cast<std::uint8_t>(value >> 8));
    put_byte(out, static_cast<std::uint8_t>(value & 0xFF));
}

// ISO/IEC 13818-1 Annex B: the CRC_32 field every PSI section ends with is
// the non-reflected CRC-32/MPEG-2 variant - generator polynomial
// x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1 (0x04C11DB7),
// initial value all-ones, no output XOR, most-significant-bit-first. This is
// NOT the same algorithm as the reflected CRC-32 (poly 0xEDB88320, e.g.
// zlib/PNG's) that "CRC-32" alone often means - transcribing that instead is
// a real, easy-to-make mistake, so this is self-checked below against the
// standard "123456789" test vector rather than merely trusted.
constexpr std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFF'FFFFu;
    for (const auto b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000'0000u) ? (crc << 1) ^ 0x04C1'1DB7u : (crc << 1);
        }
    }
    return crc;
}

constexpr std::array<std::byte, 9> kCrc32CheckVector = {
    std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
    std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
static_assert(crc32_mpeg2(kCrc32CheckVector) == 0x0376'E6E7u,
              "CRC-32/MPEG-2 self-check against the standard \"123456789\" test vector failed - "
              "a wrong polynomial or bit order here corrupts every PAT/PMT section silently, "
              "since nothing but a real demuxer's CRC check would ever notice.");

// --- PSI section builders ---------------------------------------------------
// Table/section field names and widths throughout follow ISO/IEC 13818-1
// §2.4.4 (program_association_section, Table 2-30) and §2.4.4.8
// (TS_program_map_section, Table 2-33).

Bytes build_pat_section(std::uint16_t transport_stream_id, std::uint16_t program_number,
                        std::uint16_t pmt_pid) {
    Bytes body;
    put_be16(body, transport_stream_id);
    // reserved(2)='11', version_number(5)=0, current_next_indicator(1)=1 -
    // version_number never changes here because this module never rewrites
    // a PAT/PMT mid-stream, so 0 for the life of the file is correct, not a
    // placeholder.
    put_byte(body, 0xC1);
    put_byte(body, 0x00);  // section_number
    put_byte(body, 0x00);  // last_section_number
    put_be16(body, program_number);
    // reserved(3)='111', program_map_PID(13).
    put_be16(body, static_cast<std::uint16_t>(0xE000u | (pmt_pid & 0x1FFFu)));

    Bytes section;
    put_byte(section, 0x00);  // table_id: program_association_section
    // section_syntax_indicator(1)=1, '0'(1), reserved(2)='11', then the
    // 12-bit section_length counting everything from here to the end of
    // CRC_32 inclusive.
    const auto section_length = static_cast<std::uint16_t>(body.size() + 4);
    put_be16(section, static_cast<std::uint16_t>(0xB000u | (section_length & 0x0FFFu)));
    section.insert(section.end(), body.begin(), body.end());
    put_be32(section, crc32_mpeg2(section));
    return section;
}

// Both AC3_descriptor and Enhanced_AC3_descriptor are left with every
// optional identification field (component_type/bsid/mainid/asvc and, for
// the enhanced form, substream1-3) unset - all of them are genuinely
// optional per their own descriptor syntax, gated by a presence flag each.
// ac3::io::scan (the only source this module's caller has) reports format,
// sample rate and rendered channel count, not the finer bsmod/full-service/
// associated-service semantics those fields carry - and a guessed or
// zero-filled value there is actively misleading to a receiver that reads
// it, unlike an absent optional field, which a receiver already has to
// handle. A decoder still has everything it needs to actually play the
// stream from the AC-3/E-AC-3 bitstream's own bsmod/acmod, which are present
// unconditionally regardless of this descriptor.
Bytes build_ac3_descriptor() {
    Bytes d;
    put_byte(d, kTagAc3Descriptor);
    put_byte(d, 1);  // descriptor_length: the flags byte alone
    // component_type_flag/bsid_flag/mainid_flag/asvc_flag = 0, reserved(4)=0.
    put_byte(d, 0x00);
    return d;
}

Bytes build_enhanced_ac3_descriptor() {
    Bytes d;
    put_byte(d, kTagEnhancedAc3Descriptor);
    put_byte(d, 1);  // descriptor_length: the flags byte alone
    // component_type_flag/bsid_flag/mainid_flag/asvc_flag/mixinfoexists/
    // substream1_flag/substream2_flag/substream3_flag = 0 (mixinfoexists is
    // a plain bit here, not gated by its own flag - ETSI EN 300 468 D.5
    // defines it that way, unlike every other field in the byte).
    put_byte(d, 0x00);
    return d;
}

Bytes build_pmt_section(std::uint16_t program_number, std::uint16_t audio_pid,
                        const Bytes& descriptor) {
    Bytes es_loop;
    put_byte(es_loop, kStreamTypePrivateData);
    // reserved(3)='111', elementary_PID(13).
    put_be16(es_loop, static_cast<std::uint16_t>(0xE000u | (audio_pid & 0x1FFFu)));
    // reserved(4)='1111', ES_info_length(12).
    put_be16(es_loop, static_cast<std::uint16_t>(0xF000u | (descriptor.size() & 0x0FFFu)));
    es_loop.insert(es_loop.end(), descriptor.begin(), descriptor.end());

    Bytes body;
    put_be16(body, program_number);
    put_byte(body, 0xC1);  // reserved(2)='11', version_number(5)=0, current_next_indicator(1)=1
    put_byte(body, 0x00);  // section_number
    put_byte(body, 0x00);  // last_section_number
    // reserved(3)='111', PCR_PID(13) - the audio PID, since it is the only
    // PID this program has and mux() always stamps a PCR on it (see
    // mpegts.hpp's header comment on why once per access unit is enough).
    put_be16(body, static_cast<std::uint16_t>(0xE000u | (audio_pid & 0x1FFFu)));
    put_be16(body, 0xF000u);  // reserved(4)='1111', program_info_length(12)=0: no top-level descriptors
    body.insert(body.end(), es_loop.begin(), es_loop.end());

    Bytes section;
    put_byte(section, 0x02);  // table_id: TS_program_map_section
    const auto section_length = static_cast<std::uint16_t>(body.size() + 4);
    put_be16(section, static_cast<std::uint16_t>(0xB000u | (section_length & 0x0FFFu)));
    section.insert(section.end(), body.begin(), body.end());
    put_be32(section, crc32_mpeg2(section));
    return section;
}

// --- TS packet assembly -----------------------------------------------------

// A PSI section (PAT or PMT) as this module ever builds one is always small
// enough - one program, one stream - to fit in a single TS packet with the
// mandatory pointer_field, so unlike a PES access unit this never needs to
// span packets. If a future change ever grows a section past that, this is
// a programming error (a caller/build-time bug, not a malformed input this
// module was asked to mux), so it asserts rather than returning a
// std::expected error nothing else in this file could plausibly trigger.
void write_psi_packet(Bytes& out, std::uint16_t pid, std::uint8_t& cc, const Bytes& section) {
    assert(section.size() + 1 <= kTsPacketSize - 4);

    Bytes pkt;
    pkt.reserve(kTsPacketSize);
    put_byte(pkt, kSyncByte);
    // transport_error_indicator(1)=0, payload_unit_start_indicator(1)=1 (a
    // PSI section always starts its own packet here), transport_priority(1)=0,
    // PID(13).
    put_be16(pkt, static_cast<std::uint16_t>(0x4000u | (pid & 0x1FFFu)));
    // transport_scrambling_control(2)='00', adaptation_field_control(2)='01'
    // (payload only), continuity_counter(4).
    put_byte(pkt, static_cast<std::uint8_t>(0x10u | (cc & 0x0Fu)));
    cc = static_cast<std::uint8_t>((cc + 1) & 0x0Fu);
    put_byte(pkt, 0x00);  // pointer_field: the section starts immediately after it
    pkt.insert(pkt.end(), section.begin(), section.end());
    // ISO/IEC 13818-1 §2.4.4.3's note: a stuffing byte of 0xFF immediately
    // following the last section in a packet's payload is not itself a
    // valid table_id, so a demuxer that keeps reading past the section it
    // wanted stops there rather than misinterpreting stuffing as another
    // section header.
    while (pkt.size() < kTsPacketSize) {
        put_byte(pkt, 0xFF);
    }
    assert(pkt.size() == kTsPacketSize);
    out.insert(out.end(), pkt.begin(), pkt.end());
}

// program_clock_reference: 33-bit base (90 kHz) + 6 reserved bits (all 1) +
// 9-bit extension (27 MHz remainder, 0-299) - ISO/IEC 13818-1 §2.4.2.2,
// Table 2-6. This module's whole timing model runs off the same 90 kHz
// count PTS uses (see mux()'s stamp_90k), so the extension is always 0:
// there is no finer-grained clock anywhere in this module to put there, and
// 0 is exactly as valid as any other value in that field.
void write_pcr(Bytes& pkt, std::uint64_t pcr_base_90k) {
    const std::uint64_t base = pcr_base_90k & 0x1'FFFF'FFFFull;  // 33 bits
    const std::uint64_t word = (base << 15) | (0x3Full << 9);    // reserved(6)=111111, extension(9)=0
    for (int i = 5; i >= 0; --i) {
        put_byte(pkt, static_cast<std::uint8_t>(word >> (8 * i)));
    }
}

// PTS/DTS field encoding, ISO/IEC 13818-1 §2.4.3.7, Table 2-21 - the 5-byte
// '0010 PTS[32..30] marker PTS[29..15] marker PTS[14..0] marker' layout used
// for a PTS-only PES header (prefix 0b0010; DTS-also would be 0b0011/0b0001
// and is never needed here, since audio has no reordering to signal).
void write_pts(Bytes& pkt, std::uint64_t pts_90k) {
    const std::uint64_t v = pts_90k & 0x1'FFFF'FFFFull;  // 33 bits
    put_byte(pkt, static_cast<std::uint8_t>(0x20u | (((v >> 30) & 0x7u) << 1) | 0x1u));
    put_byte(pkt, static_cast<std::uint8_t>((v >> 22) & 0xFFu));
    put_byte(pkt, static_cast<std::uint8_t>((((v >> 15) & 0x7Fu) << 1) | 0x1u));
    put_byte(pkt, static_cast<std::uint8_t>((v >> 7) & 0xFFu));
    put_byte(pkt, static_cast<std::uint8_t>(((v & 0x7Fu) << 1) | 0x1u));
}

// Writes an adaptation field of exactly `total_len` bytes (the value that
// goes in adaptation_field_length itself, i.e. NOT counting that length byte
// - ISO/IEC 13818-1 §2.4.3.4, Table 2-6). total_len==0 is that table's own
// special case: "the value 0 is for inserting a single stuffing byte in the
// adaptation field of a Transport Stream packet" - just the length byte, no
// flags byte at all - used when a packet needs exactly one pad byte to reach
// 188 and nothing else (no PCR, no random_access marking).
void append_adaptation_field(Bytes& pkt, std::size_t total_len, bool pcr_present,
                             std::uint64_t pcr_base_90k, bool random_access) {
    put_byte(pkt, static_cast<std::uint8_t>(total_len));
    if (total_len == 0) {
        return;
    }
    std::uint8_t flags = 0;
    if (random_access) {
        flags |= 0x40u;  // random_access_indicator
    }
    if (pcr_present) {
        flags |= 0x10u;  // PCR_flag
    }
    put_byte(pkt, flags);
    std::size_t used = 1;
    if (pcr_present) {
        write_pcr(pkt, pcr_base_90k);
        used += 6;
    }
    // Remaining budget, if any, is pure stuffing - ISO/IEC 13818-1 §2.4.3.5:
    // "stuffing_byte - This is a fixed 8-bit value equal to '1111 1111'".
    while (used < total_len) {
        put_byte(pkt, 0xFF);
        ++used;
    }
}

// Builds one complete PES packet (header + the raw access unit as payload) -
// ISO/IEC 13818-1 §2.4.3.6/§2.4.3.7. Returns MuxError::kFrameTooLarge if the
// access unit would overflow PES_packet_length's 16 bits; every legal AC-3/
// E-AC-3 access unit is far smaller than that ceiling, so this is a defensive
// check rather than something real material is expected to hit.
std::expected<Bytes, MuxError> build_pes_packet(std::span<const std::byte> access_unit,
                                                std::uint64_t pts_90k) {
    constexpr std::size_t kHeaderAfterLengthField = 3 + 5;  // flags(3) + PTS(5)
    // start_code_prefix(3) + stream_id(1) + PES_packet_length field(2), then
    // kHeaderAfterLengthField.
    constexpr std::size_t kFixedHeaderBytes = 6 + kHeaderAfterLengthField;
    const std::size_t pes_packet_length = kHeaderAfterLengthField + access_unit.size();
    if (pes_packet_length > 0xFFFFu) {
        return std::unexpected(MuxError::kFrameTooLarge);
    }

    Bytes pes;
    pes.reserve(kFixedHeaderBytes + access_unit.size());
    put_byte(pes, 0x00);
    put_byte(pes, 0x00);
    put_byte(pes, 0x01);  // packet_start_code_prefix
    put_byte(pes, kPesStreamIdPrivateStream1);
    put_be16(pes, static_cast<std::uint16_t>(pes_packet_length));
    // '10'(2, fixed marker) + PES_scrambling_control(2)='00' +
    // PES_priority(1)=0 + data_alignment_indicator(1)=1 (the payload is
    // exactly one complete access unit, starting right here) +
    // copyright(1)=0 + original_or_copy(1)=0.
    put_byte(pes, 0x84);
    // PTS_DTS_flags(2)='10' (PTS only - audio never reorders, so no DTS) +
    // ESCR/ES_rate/DSM_trick_mode/additional_copy_info/PES_CRC/
    // PES_extension flags, all 0.
    put_byte(pes, 0x80);
    put_byte(pes, 0x05);  // PES_header_data_length: just the 5-byte PTS
    write_pts(pes, pts_90k);
    pes.insert(pes.end(), access_unit.begin(), access_unit.end());
    return pes;
}

// Splits one PES packet across as many 188-byte TS packets as it needs.
// ISO/IEC 13818-1 §2.4.3.3: continuity_counter increments once per packet
// that carries payload on this PID, first packet or not. The first packet
// carries PUSI, and an adaptation field stamping PCR and
// random_access_indicator (every AC-3/E-AC-3 access unit here is
// independently decodable, dependent substreams and all, so this is always
// true, not an approximation). Whichever packet turns out to be the last one
// gets whatever adaptation-field stuffing it needs to land on exactly 188
// bytes - a Transport Stream packet has no other way to be short.
void emit_pes_packets(Bytes& out, std::uint16_t pid, std::uint8_t& cc,
                      std::span<const std::byte> pes, std::uint64_t pcr_base_90k) {
    std::size_t offset = 0;
    bool first = true;
    while (offset < pes.size()) {
        Bytes pkt;
        pkt.reserve(kTsPacketSize);
        put_byte(pkt, kSyncByte);
        put_be16(pkt, static_cast<std::uint16_t>((first ? 0x4000u : 0x0000u) | (pid & 0x1FFFu)));

        const std::size_t remaining = pes.size() - offset;
        std::size_t take = 0;
        bool has_adaptation = false;
        std::size_t adapt_len = 0;

        if (first) {
            // Reserve 1 (adaptation_field_length byte) + 1 (flags) + 6 (PCR)
            // = 8 of the 184-byte payload budget; 183 - take always lands
            // exactly on 7 (no stuffing) when take is the full 176, and
            // above 7 (with stuffing) whenever this is also the last packet.
            take = std::min<std::size_t>(remaining, 176);
            has_adaptation = true;
            adapt_len = 183 - take;
        } else if (remaining >= 184) {
            take = 184;
            has_adaptation = false;
        } else {
            take = remaining;
            has_adaptation = true;
            adapt_len = 183 - take;
        }

        // transport_scrambling_control(2)='00',
        // adaptation_field_control(2)='11' (both) or '01' (payload only),
        // continuity_counter(4).
        put_byte(pkt, static_cast<std::uint8_t>((has_adaptation ? 0x30u : 0x10u) | (cc & 0x0Fu)));
        cc = static_cast<std::uint8_t>((cc + 1) & 0x0Fu);

        if (has_adaptation) {
            append_adaptation_field(pkt, adapt_len, /*pcr_present=*/first, pcr_base_90k,
                                    /*random_access=*/first);
        }
        pkt.insert(pkt.end(), pes.begin() + static_cast<std::ptrdiff_t>(offset),
                  pes.begin() + static_cast<std::ptrdiff_t>(offset + take));
        assert(pkt.size() == kTsPacketSize);
        out.insert(out.end(), pkt.begin(), pkt.end());

        offset += take;
        first = false;
    }
}

}  // namespace

std::string_view describe(MuxError error) {
    switch (error) {
        case MuxError::kNoFrames:
            return "no frames to mux";
        case MuxError::kInvalidTrack:
            return "invalid track: channels, sample rate and samples_per_frame are required";
        case MuxError::kInvalidOptions:
            return "invalid options: pmt_pid and audio_pid must differ and neither may be 0x0000";
        case MuxError::kFrameTooLarge:
            return "access unit too large for one PES packet";
    }
    return "unknown error";
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (track.channels <= 0 || track.sample_rate == 0 || track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    if (options.pmt_pid == options.audio_pid || options.pmt_pid == kPatPid ||
        options.audio_pid == kPatPid) {
        return std::unexpected(MuxError::kInvalidOptions);
    }

    const Bytes descriptor = track.codec == AudioCodec::kAc3 ? build_ac3_descriptor()
                                                              : build_enhanced_ac3_descriptor();
    const Bytes pat_section =
        build_pat_section(options.transport_stream_id, options.program_number, options.pmt_pid);
    const Bytes pmt_section = build_pmt_section(options.program_number, options.audio_pid, descriptor);

    // PTS and PCR share one 90 kHz clock derived from the cumulative sample
    // count, the same way matroska::mux derives its millisecond timestamps -
    // see that module's own comment on why the cumulative count is used
    // rather than a per-frame increment (a frame duration that is not a
    // whole number of clock ticks - 1536 samples at 44.1 kHz, for one -
    // rounds without the error ever accumulating).
    const auto stamp_90k = [&](std::size_t index) {
        return static_cast<std::uint64_t>(index) * track.samples_per_frame * 90'000ull /
               track.sample_rate;
    };

    Bytes out;
    std::uint8_t pat_cc = 0;
    std::uint8_t pmt_cc = 0;
    std::uint8_t audio_cc = 0;
    const std::uint32_t psi_period = std::max<std::uint32_t>(options.psi_repeat_every_au, 1);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i % psi_period == 0) {
            write_psi_packet(out, kPatPid, pat_cc, pat_section);
            write_psi_packet(out, options.pmt_pid, pmt_cc, pmt_section);
        }

        const auto pts = stamp_90k(i);
        auto pes = build_pes_packet(frames[i], pts);
        if (!pes) {
            return std::unexpected(pes.error());
        }
        emit_pes_packets(out, options.audio_pid, audio_cc, *pes, pts);
    }

    return out;
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options) {
    const std::vector<std::span<const std::byte>> views(frames.begin(), frames.end());
    return mux(track, views, options);
}

}  // namespace mpegts
