#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "mpegts/mpegts.hpp"

// These tests read the muxer's output back with an independent TS/PSI walker
// rather than comparing against bytes this same code produced. A muxer
// checked only against itself proves nothing about whether a player can
// open the stream - the same reasoning test_matroska.cpp documents for its
// own EBML walker.

namespace {

using Bytes = std::vector<std::byte>;

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

constexpr std::size_t kTsPacketSize = 188;

// ISO/IEC 13818-1 Annex B's CRC_32, transcribed independently of
// src/mpegts/src/mpegts.cpp's own copy (same reasoning as that file's own
// comment on why this is worth self-checking rather than trusting by
// construction): non-reflected CRC-32/MPEG-2, poly 0x04C11DB7, init
// all-ones, no output XOR.
std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFF'FFFFu;
    for (const auto b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000'0000u) ? (crc << 1) ^ 0x04C1'1DB7u : (crc << 1);
        }
    }
    return crc;
}

struct TsPacket {
    std::uint16_t pid = 0;
    bool pusi = false;
    std::uint8_t cc = 0;
    bool has_payload = false;
    bool pcr_present = false;
    bool random_access = false;
    std::uint64_t pcr_base = 0;
    std::span<const std::byte> payload;
};

std::vector<TsPacket> parse_packets(std::span<const std::byte> ts) {
    REQUIRE(ts.size() % kTsPacketSize == 0);
    std::vector<TsPacket> out;
    for (std::size_t off = 0; off < ts.size(); off += kTsPacketSize) {
        const auto pkt = ts.subspan(off, kTsPacketSize);
        REQUIRE(std::to_integer<std::uint8_t>(pkt[0]) == 0x47);
        const auto b1 = std::to_integer<std::uint8_t>(pkt[1]);
        const auto b2 = std::to_integer<std::uint8_t>(pkt[2]);
        const auto b3 = std::to_integer<std::uint8_t>(pkt[3]);

        TsPacket p;
        p.pusi = (b1 & 0x40u) != 0;
        p.pid = static_cast<std::uint16_t>(((b1 & 0x1Fu) << 8) | b2);
        const auto afc = static_cast<std::uint8_t>((b3 >> 4) & 0x3u);
        p.cc = static_cast<std::uint8_t>(b3 & 0x0Fu);
        const bool has_adaptation = (afc & 0x2u) != 0;
        p.has_payload = (afc & 0x1u) != 0;

        std::size_t offset = 4;
        if (has_adaptation) {
            const auto adapt_len = std::to_integer<std::uint8_t>(pkt[4]);
            if (adapt_len > 0) {
                const auto flags = std::to_integer<std::uint8_t>(pkt[5]);
                p.random_access = (flags & 0x40u) != 0;
                p.pcr_present = (flags & 0x10u) != 0;
                if (p.pcr_present) {
                    std::uint64_t word = 0;
                    for (int i = 0; i < 6; ++i) {
                        word = (word << 8) | std::to_integer<std::uint8_t>(pkt[6 + i]);
                    }
                    p.pcr_base = (word >> 15) & 0x1'FFFF'FFFFull;
                }
            }
            offset += 1 + adapt_len;
        }
        if (p.has_payload) {
            REQUIRE(offset <= pkt.size());
            p.payload = pkt.subspan(offset);
        }
        out.push_back(p);
    }
    return out;
}

// Extracts one PSI section from a PUSI packet's payload (pointer_field, then
// the section) and validates its CRC_32 covers the section correctly.
Bytes section_from_psi_packet(const TsPacket& pkt) {
    REQUIRE(pkt.pusi);
    REQUIRE(pkt.has_payload);
    const auto pointer_field = std::to_integer<std::uint8_t>(pkt.payload[0]);
    const auto section_start = pkt.payload.subspan(1 + pointer_field);
    // section_length is the low 12 bits of bytes[1..2]; the section runs
    // from table_id through CRC_32 inclusive, i.e. 3 + section_length bytes.
    const auto len_hi = std::to_integer<std::uint8_t>(section_start[1]);
    const auto len_lo = std::to_integer<std::uint8_t>(section_start[2]);
    const std::size_t section_length = ((len_hi & 0x0Fu) << 8) | len_lo;
    const Bytes section(section_start.begin(), section_start.begin() + 3 +
                                                     static_cast<std::ptrdiff_t>(section_length));
    REQUIRE(crc32_mpeg2(section) == 0);
    return section;
}

std::vector<const TsPacket*> packets_on_pid(const std::vector<TsPacket>& packets,
                                            std::uint16_t pid) {
    std::vector<const TsPacket*> out;
    for (const auto& p : packets) {
        if (p.pid == pid) {
            out.push_back(&p);
        }
    }
    return out;
}

// Reassembles every PES-wrapped access unit carried on `pid` and returns
// each one's raw payload (the bytes after the full PES header), in order.
// The PES header this module always writes is a fixed 14 bytes: start code
// (3) + stream_id (1) + PES_packet_length (2) + flags (3) + PTS (5).
std::vector<Bytes> reassemble_pes_payloads(const std::vector<const TsPacket*>& on_pid) {
    constexpr std::size_t kPesHeaderBytes = 14;
    std::vector<Bytes> out;
    Bytes current;
    std::size_t want = 0;  // access-unit bytes expected for the PES packet in progress
    bool in_progress = false;

    for (const auto* p : on_pid) {
        if (!p->has_payload) {
            continue;
        }
        if (p->pusi) {
            if (in_progress) {
                // A well-formed stream never starts a new PES before the
                // previous one's declared length is satisfied.
                REQUIRE(current.size() == want);
                out.push_back(current);
            }
            REQUIRE(p->payload.size() >= kPesHeaderBytes);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[0]) == 0x00);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[1]) == 0x00);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[2]) == 0x01);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[3]) == 0xBD);  // private_stream_1
            const auto len_hi = std::to_integer<std::uint8_t>(p->payload[4]);
            const auto len_lo = std::to_integer<std::uint8_t>(p->payload[5]);
            const std::size_t pes_packet_length =
                (static_cast<std::size_t>(len_hi) << 8) | len_lo;
            REQUIRE(pes_packet_length >= 8);  // flags(3) + PTS(5), the fixed part it always covers
            want = pes_packet_length - 8;
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[8]) == 0x05);  // PES_header_data_length
            const auto access_unit_part = p->payload.subspan(kPesHeaderBytes);
            current.assign(access_unit_part.begin(), access_unit_part.end());
            in_progress = true;
        } else {
            REQUIRE(in_progress);
            current.insert(current.end(), p->payload.begin(), p->payload.end());
        }
    }
    if (in_progress) {
        REQUIRE(current.size() == want);
        out.push_back(current);
    }
    return out;
}

}  // namespace

TEST_CASE("MPEG-TS output parses as well-formed TS packets with a valid PAT/PMT", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(1792, 0x11), frame_of(1792, 0x22),
                                    frame_of(1792, 0x33), frame_of(1792, 0x44),
                                    frame_of(1792, 0x55)};
    const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kEac3, .channels = 6}, frames);
    REQUIRE(file.has_value());

    const auto packets = parse_packets(*file);
    REQUIRE_FALSE(packets.empty());

    const auto pat_packets = packets_on_pid(packets, 0x0000);
    REQUIRE_FALSE(pat_packets.empty());
    const auto pat = section_from_psi_packet(*pat_packets.front());
    CHECK(std::to_integer<std::uint8_t>(pat[0]) == 0x00);  // table_id: PAT
    // program_number at bytes[8..9], program_map_PID at bytes[10..11] low 13 bits.
    const std::uint16_t program_number = static_cast<std::uint16_t>(
        (std::to_integer<std::uint8_t>(pat[8]) << 8) | std::to_integer<std::uint8_t>(pat[9]));
    const std::uint16_t pmt_pid = static_cast<std::uint16_t>(
        ((std::to_integer<std::uint8_t>(pat[10]) & 0x1Fu) << 8) |
        std::to_integer<std::uint8_t>(pat[11]));
    CHECK(program_number == 1);
    CHECK(pmt_pid == 0x1000);

    const auto pmt_packets = packets_on_pid(packets, pmt_pid);
    REQUIRE_FALSE(pmt_packets.empty());
    const auto pmt = section_from_psi_packet(*pmt_packets.front());
    CHECK(std::to_integer<std::uint8_t>(pmt[0]) == 0x02);  // table_id: PMT
    const std::uint16_t pcr_pid = static_cast<std::uint16_t>(
        ((std::to_integer<std::uint8_t>(pmt[8]) & 0x1Fu) << 8) |
        std::to_integer<std::uint8_t>(pmt[9]));
    CHECK(pcr_pid == 0x0100);  // default audio_pid
    // ES loop starts right after program_info_length (bytes[10..11], 0 here).
    const std::uint8_t stream_type = std::to_integer<std::uint8_t>(pmt[12]);
    const std::uint16_t elementary_pid = static_cast<std::uint16_t>(
        ((std::to_integer<std::uint8_t>(pmt[13]) & 0x1Fu) << 8) |
        std::to_integer<std::uint8_t>(pmt[14]));
    CHECK(stream_type == 0x06);  // DVB private data - see mpegts.hpp
    CHECK(elementary_pid == 0x0100);
    const std::uint8_t descriptor_tag = std::to_integer<std::uint8_t>(pmt[17]);
    CHECK(descriptor_tag == 0x7A);  // Enhanced_AC3_descriptor for E-AC-3
}

TEST_CASE("MPEG-TS access units round-trip byte-for-byte through PES", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(1792, 0x11), frame_of(1792, 0x22),
                                    frame_of(1792, 0x33), frame_of(896, 0x44)};
    const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2}, frames);
    REQUIRE(file.has_value());

    const auto packets = parse_packets(*file);
    const auto audio_ptrs = packets_on_pid(packets, 0x0100);
    const auto access_units = reassemble_pes_payloads(audio_ptrs);

    REQUIRE(access_units.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(access_units[i] == frames[i]);
    }

    // Every access unit's first TS packet stamps PCR and marks a random
    // access point (mpegts.hpp's rationale: every AC-3/E-AC-3 access unit
    // here is independently decodable).
    std::size_t pusi_count = 0;
    for (const auto* p : audio_ptrs) {
        if (p->pusi) {
            CHECK(p->pcr_present);
            CHECK(p->random_access);
            ++pusi_count;
        }
    }
    CHECK(pusi_count == frames.size());
}

TEST_CASE("MPEG-TS descriptor identifies AC-3 vs Enhanced AC-3", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(64, 0xAB), frame_of(64, 0xCD), frame_of(64, 0xEF)};

    const auto ac3_file = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2}, frames);
    REQUIRE(ac3_file.has_value());
    const auto ac3_packets = parse_packets(*ac3_file);
    const auto ac3_pmt = section_from_psi_packet(*packets_on_pid(ac3_packets, 0x1000).front());
    CHECK(std::to_integer<std::uint8_t>(ac3_pmt[17]) == 0x6A);
    CHECK(std::to_integer<std::uint8_t>(ac3_pmt[18]) == 1);  // descriptor_length: flags byte only

    const auto eac3_file =
        mpegts::mux({.codec = mpegts::AudioCodec::kEac3, .channels = 6}, frames);
    REQUIRE(eac3_file.has_value());
    const auto eac3_packets = parse_packets(*eac3_file);
    const auto eac3_pmt = section_from_psi_packet(*packets_on_pid(eac3_packets, 0x1000).front());
    CHECK(std::to_integer<std::uint8_t>(eac3_pmt[17]) == 0x7A);
    CHECK(std::to_integer<std::uint8_t>(eac3_pmt[18]) == 1);
}

TEST_CASE("MPEG-TS continuity counters increment per PID and wrap at 16", "[mpegts]") {
    // Small access units (well under 167 bytes) so each one is exactly one
    // TS packet, and psi_repeat_every_au=1 so PAT/PMT repeat every access
    // unit too - with 20 access units, every PID's continuity counter wraps
    // past 15 back to 0 at least once, which is the case a stuck or
    // never-incremented counter would fail on but a short run would not
    // exercise at all.
    std::vector<Bytes> frames;
    for (int i = 0; i < 20; ++i) {
        frames.push_back(frame_of(32, static_cast<std::uint8_t>(i)));
    }
    const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2}, frames,
                                  {.psi_repeat_every_au = 1});
    REQUIRE(file.has_value());
    const auto packets = parse_packets(*file);

    for (const std::uint16_t pid : {std::uint16_t{0x0000}, std::uint16_t{0x1000},
                                    std::uint16_t{0x0100}}) {
        const auto on_pid = packets_on_pid(packets, pid);
        REQUIRE(on_pid.size() >= 17);  // enough to prove a wrap actually happened
        bool saw_wrap = false;
        for (std::size_t i = 1; i < on_pid.size(); ++i) {
            const auto expected = static_cast<std::uint8_t>((on_pid[i - 1]->cc + 1) & 0x0F);
            CHECK(on_pid[i]->cc == expected);
            if (on_pid[i]->cc == 0 && on_pid[i - 1]->cc == 15) {
                saw_wrap = true;
            }
        }
        CHECK(saw_wrap);
    }
}

TEST_CASE("MPEG-TS muxer rejects what it cannot describe", "[mpegts]") {
    const std::vector<Bytes> one{frame_of(16, 0)};
    CHECK(mpegts::mux({.channels = 2}, {}).error() == mpegts::MuxError::kNoFrames);
    CHECK(mpegts::mux({.channels = 0}, one).error() == mpegts::MuxError::kInvalidTrack);
    CHECK(mpegts::mux({.sample_rate = 0, .channels = 2}, one).error() ==
          mpegts::MuxError::kInvalidTrack);
    CHECK(mpegts::mux({.channels = 2, .samples_per_frame = 0}, one).error() ==
          mpegts::MuxError::kInvalidTrack);
    CHECK(mpegts::mux({.channels = 2}, one, {.pmt_pid = 0x0100, .audio_pid = 0x0100}).error() ==
          mpegts::MuxError::kInvalidOptions);
    CHECK(mpegts::mux({.channels = 2}, one, {.pmt_pid = 0x0000}).error() ==
          mpegts::MuxError::kInvalidOptions);

    const std::vector<Bytes> huge{frame_of(70'000, 0)};
    CHECK(mpegts::mux({.channels = 2}, huge).error() == mpegts::MuxError::kFrameTooLarge);
}

TEST_CASE("MPEG-TS mux of real encoded AC-3 round-trips through ac3::io::scan", "[mpegts]") {
    // Real, non-silent, multi-frame material - a silent or single-frame
    // stream would exercise almost none of the encoder and none of the
    // frame-2-onward MDCT overlap state (CONTRIBUTING.md's validation
    // discipline). Different tones per channel so a channel-order mistake
    // would show up as wrong content, not just a wrong byte count.
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};

    Bytes elementary;
    constexpr int kFrames = 6;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const auto nf = static_cast<float>(n);
            pcm[0][static_cast<std::size_t>(n)] = 0.3F * std::sin(nf * 0.05F);
            pcm[1][static_cast<std::size_t>(n)] = 0.3F * std::sin(nf * 0.13F);
        }
        const auto encoded = encoder->encode_frame(views);
        REQUIRE(encoded.has_value());
        elementary.insert(elementary.end(), encoded->begin(), encoded->end());
    }

    const auto scanned = ac3::io::scan(elementary);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_units.size() == static_cast<std::size_t>(kFrames));
    REQUIRE(scanned->kind == ac3::io::StreamKind::kAc3);

    std::vector<Bytes> frames;
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const mpegts::AudioTrack track{.codec = mpegts::AudioCodec::kAc3,
                                   .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                                   .channels = scanned->channels,
                                   .samples_per_frame = ac3::kSamplesPerFrame};
    const auto file = mpegts::mux(track, frames);
    REQUIRE(file.has_value());

    const auto packets = parse_packets(*file);
    const auto pmt = section_from_psi_packet(*packets_on_pid(packets, 0x1000).front());
    CHECK(std::to_integer<std::uint8_t>(pmt[17]) == 0x6A);  // AC-3, not Enhanced AC-3

    const auto audio_ptrs = packets_on_pid(packets, 0x0100);
    const auto access_units = reassemble_pes_payloads(audio_ptrs);
    REQUIRE(access_units.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(access_units[i] == frames[i]);
    }
}
