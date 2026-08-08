#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitwriter.hpp"

// The Extensible Metadata Delivery Format - ETSI TS 102 366 Annex H. A/52:2018
// Annex H defers to it wholesale rather than restating it, so this is the one
// piece of the bitstream whose authority is the ETSI text.
//
// EMDF is the sanctioned way to put data an AC-3 decoder has never heard of
// into an AC-3 or E-AC-3 frame. §H.1 places the container in a reserved space -
// it names auxdata and the per-block skip fields - so a decoder that knows
// nothing about it reads the frame exactly as it did before. That is precisely
// what makes Dolby Atmos in DD+ backward compatible: the object metadata rides
// here, not as extra coded channels, and a plain 5.1 decoder is untouched.
//
// Nothing in the container is codec state. It is a length-prefixed bag of
// identified payloads, so this module knows only how to pack bits, and the
// OAMD and JOC payload writers know nothing about where they end up.

namespace ac3::emdf {

// §H.2.2.1.1. The container is found by scanning for this word rather than by
// a fixed offset, because nauxbits is only known after the audio is decoded.
inline constexpr std::uint16_t kSyncWord = 0x5838;

// §H.2.1.2.1 / §H.2.2.2.1. A value is sent as groups of `group_bits`, most
// significant group first, each followed by a read_more bit. A k-group field
// carries the value MINUS the offset (2^n + 2^2n + ... + 2^(k-1)n), which is
// what stops the encodings overlapping: every value has exactly one shortest
// form, and the decoder recovers the offset from the group count alone.
void put_variable_bits(BitWriter& w, std::uint32_t value, int group_bits);
[[nodiscard]] int variable_bits_size(std::uint32_t value, int group_bits);

// Table H.2.3 assigns 0x1..0x7 and reserves the rest; TS 103 420 Table 55
// claims two of the reserved values for object audio.
inline constexpr int kPayloadIdOamd = 11;
inline constexpr int kPayloadIdJoc = 14;

struct Payload {
    int id = 0;
    // Whole bytes: emdf_payload_size (§H.2.2.2.5) counts bytes, so a payload
    // that does not fill its last byte has to pad itself.
    std::span<const std::byte> bytes;
};

// emdf_sync() followed by emdf_container(), zero-padded to a whole number of
// bytes as §H.2.2.1.2 requires.
//
// Every payload is written with the configuration TS 103 420 Table 56 makes
// mandatory for object audio - frame-aligned, no sample offset, no duration,
// grouped, highest priority, no processing allowed - because that is the only
// configuration this encoder ever needs and the only one a JOC decoder will
// accept. `groupid` ties the OAMD and JOC payloads together and must be equal
// for both.
[[nodiscard]] std::vector<std::byte> build_container(std::span<const Payload> payloads,
                                                     int groupid = 0);

}  // namespace ac3::emdf
