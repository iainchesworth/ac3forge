#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "mpegts/export.hpp"

// A minimal MPEG-2 Transport Stream (TS) muxer, per ISO/IEC 13818-1 (MPEG-2
// Systems), with the AC-3/Enhanced AC-3 identification DVB defines in ETSI
// EN 300 468 Annex D.
//
// This is a container writer and nothing more: it lays out 188-byte TS
// packets and takes each access unit as opaque bytes. It has NO dependency
// on ac3::forge beyond the caller telling it AC-3 vs. E-AC-3 (AudioCodec,
// below) - which is the point of keeping it a separate library, the same
// shape as matroska::matroska (src/matroska/). A caller muxing E-AC-3 hands
// over whole access units; the module knows nothing about what is inside
// them.
//
// Scope, deliberately narrow for a first, mergeable implementation:
//   - single program: one PAT, one PMT, one elementary stream, all on PIDs
//     the caller can override but that default to values clear of the
//     0x0000-0x001F reserved range (ISO/IEC 13818-1 Table 2-3);
//   - PAT and PMT are re-sent periodically so a receiver tuning in mid-
//     stream does not have to wait for the very first packet - common
//     broadcast practice, not something ISO/IEC 13818-1 itself mandates for
//     a stream this simple;
//   - PCR runs on the audio PID (there is no other PID to carry it), stamped
//     once per access unit - AC-3/E-AC-3's largest possible frame duration
//     (1536 samples at the slowest Annex E fscod2 rate, 16 kHz) is 96 ms,
//     comfortably inside the 100 ms bound §2.7.2 sets for how far apart two
//     PCR values may be, so "once per access unit" is enough on its own
//     without a separate timer;
//   - no video, no other elementary streams, no PID remapping, no seek aids.
// A general-purpose multiplexer is out of scope; this is enough to produce a
// stream a player or `ffprobe` recognizes as one AC-3/E-AC-3 programme.
//
// Broadcast profile: DVB, not ATSC. Both standards register AC-3/E-AC-3 for
// MPEG-TS carriage, but with different, non-interoperable signalling (a
// different stream_type, and ATSC's own PSIP descriptor registry rather than
// DVB's SI one) - implementing a bit of each would produce a stream that
// satisfies neither's conformance requirements, which is worse than picking
// one and saying so. This module implements DVB: stream_type 0x06 (audio
// carried as PES private data - ISO/IEC 13818-1 Table 2-34) plus the
// AC3_descriptor (tag 0x6A) or Enhanced_AC3_descriptor (tag 0x7A) DVB
// defines in ETSI EN 300 468 Annex D.3/D.5, chosen because - per this
// project's clean-room sourcing rules - it is the more completely specified
// of the two registries, with every field's bit layout verified against the
// actual descriptor definition rather than inferred from a stream_type
// number alone.

namespace mpegts {

enum class AudioCodec : std::uint8_t {
    kAc3,   // ETSI EN 300 468 Annex D.3 AC3_descriptor, tag 0x6A
    kEac3,  // ETSI EN 300 468 Annex D.5 Enhanced_AC3_descriptor, tag 0x7A
};

enum class MuxError : std::uint8_t {
    kNoFrames,
    kInvalidTrack,    // zero/negative channels, zero sample rate or zero samples_per_frame
    kInvalidOptions,  // PMT PID and audio PID collide, or either collides with PID 0x0000 (PAT)
    kFrameTooLarge,   // a single access unit too large for one PES packet's 16-bit length field
};

[[nodiscard]] MPEGTS_EXPORT std::string_view describe(MuxError error);

struct AudioTrack {
    AudioCodec codec = AudioCodec::kEac3;
    std::uint32_t sample_rate = 48000;
    int channels = 2;
    // Samples one access unit represents, used to place PTS/PCR timestamps.
    // An AC-3 or E-AC-3 access unit is 1536.
    std::uint32_t samples_per_frame = 1536;
};

struct MuxOptions {
    std::uint16_t program_number = 1;
    std::uint16_t transport_stream_id = 1;
    // Chosen clear of the 0x0000-0x001F reserved PID range (ISO/IEC 13818-1
    // Table 2-3) and of each other; mux() reports MuxError::kInvalidOptions
    // if a caller manages to collide them.
    std::uint16_t pmt_pid = 0x1000;
    std::uint16_t audio_pid = 0x0100;
    // How often PAT+PMT repeat, in access units (always sent once before the
    // very first one regardless of this value). Not a conformance
    // requirement for a stream this simple - just how quickly a receiver
    // that tunes in mid-stream finds the programme.
    std::uint32_t psi_repeat_every_au = 20;
};

// Mux access units into a complete .ts, returned as bytes. No file I/O here,
// so this stays testable without touching a disk. Access units arrive as
// views (matroska::mux's own reasoning); the vector-list overload below
// forwards for owned lists.
[[nodiscard]] MPEGTS_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options = {});

[[nodiscard]] MPEGTS_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options = {});

}  // namespace mpegts
