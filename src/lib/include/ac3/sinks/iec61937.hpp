#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "ac3/export.hpp"

// IEC 61937 ("S/PDIF burst") packing: an AC-3 or E-AC-3 access unit disguised
// as 16-bit stereo PCM so AV receivers accept it over S/PDIF or HDMI.
//
// AC-3: each burst is exactly 6144 bytes (1536 stereo 16-bit sample frames —
// one AC-3 frame duration at any AC-3 sample rate): the four preamble words
// Pa 0xF872, Pb 0x4E1F, Pc (data type 1 = AC-3, with bsmod in bits 8..10), Pd
// (payload length in BITS), then the frame bytes packed big-endian into
// words, zero-padded to the burst length. Words are emitted little-endian,
// ready for a PCM16 container; byte-exact against FFmpeg's spdif muxer as the
// oracle.
//
// E-AC-3: verified against two independent primary sources (FFmpeg's
// libavformat/spdifenc.c spdif_header_eac3, and Microsoft's own "Representing
// Formats for IEC 61937 Transmissions" — the two agree). The burst is fixed
// at 24576 bytes (4x AC-3's, matching WASAPI's requirement that the carrier
// clock run at 4x the content sample rate for Dolby Digital Plus), Pc is data
// type 0x15 with no extra bits, and Pd is the payload length in BYTES rather
// than bits — unlike AC-3's Pd, the detail most likely to be copied wrong
// from the AC-3 shape. Annex E lets one syncframe cover as few as one of the
// six blocks a burst period spans (numblkscod, Table E2.4), so
// Eac3BurstPacker accumulates consecutive access units until their block
// counts reach six before emitting a burst.

namespace ac3::iec61937 {

inline constexpr std::size_t kBurstBytes = 6144;
inline constexpr std::size_t kEac3BurstBytes = 24576;

enum class WrapError : std::uint8_t {
    kNotAFrame,      // missing sync word or truncated header
    kFrameTooLarge,  // cannot happen for legal AC-3 sizes; guarded anyway
};

// Wrap exactly one AC-3 syncframe into one 6144-byte burst.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, WrapError> wrap_frame(
    std::span<const std::byte> frame);

// Accumulates E-AC-3 access units into IEC 61937 bursts. Feed it whole access
// units (ac3::split_access_units's granularity — the independent substream's
// syncframe plus every dependent's, concatenated exactly as split_access_units
// returns them) rather than lone syncframes: a dependent's channels only
// reach the burst if its bytes are included, and a decoder finds them by the
// same concatenation the elementary stream already uses.
class AC3FORGE_EXPORT Eac3BurstPacker {
   public:
    // Returns a completed burst once enough access units have accumulated to
    // cover six blocks, or std::nullopt if more are still needed. bsid, fscod
    // and numblkscod are read from the leading (independent) substream's
    // header, which every substream of an access unit shares.
    [[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, WrapError> push(
        std::span<const std::byte> access_unit);

   private:
    std::vector<std::byte> pending_;
    int blocks_pending_ = 0;
};

// Wrap a whole stream's worth of ALREADY-SPLIT units into one concatenated
// IEC 61937 payload - one AC-3 frame per unit (ac3::split_frames's
// granularity), or one whole E-AC-3 access unit per unit
// (ac3::split_access_units's granularity), matching `eac3`. For a caller
// that already has its frames/access units in hand - e.g. a GUI's freshly
// encoded output - rather than a raw elementary-stream buffer it would
// otherwise have to split itself first. ac3cli's own `spdif`/`play` commands
// split a raw buffer and wrap frame-by-frame instead (see main.cpp); both
// paths bottom out in wrap_frame/Eac3BurstPacker above, so they cannot
// disagree about how a unit becomes a burst.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, WrapError> wrap_stream(
    std::span<const std::span<const std::byte>> units, bool eac3);

}  // namespace ac3::iec61937
