#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "ac3/export.hpp"

// EXTRA_DATA() - "Dolby TrueHD (MLP) high-level bitstream description"
// §3.3.9 / §4.8: the access unit's one extension point ("allows for
// extensions to Dolby TrueHD"), sitting between the last substream_end and
// unit_end. Framing: a 4-bit length_check_nibble (the XOR of it and the
// three EXTRA_DATA_length nibbles is 0xF), a 12-bit length in 16-bit words
// (one less than the total including these fields), the opaque data(),
// padding to a 16-bit boundary, and an 8-bit parity - the XOR of the
// data+padding bytes with §4.8.5's 0xA9 constant.
//
// This is the carrier Atmos-in-TrueHD's per-frame dynamic object metadata
// rides in. What goes INSIDE data() for that purpose is the initiative's
// one remaining metadata gap (docs/concepts/truehd-mlp.md) - this module
// deliberately treats the payload as opaque bytes, so an EMDF container
// (ac3::emdf, TS 102 366 Annex H) carrying OAMD is one caller away without
// this layer guessing at the wrapper details.

namespace ac3::mlp {

// The complete EXTRA_DATA() expansion for `payload`, always a whole number
// of 16-bit words. An empty payload returns an empty vector - §4.8 makes
// EXTRA_DATA itself optional ("if substream_end_ptr[substreams-1] equals
// unit_end - start there is no EXTRA_DATA"), so nothing is the right
// encoding of nothing.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_extra_data(
    std::span<const std::byte> payload);

// Parses the EXTRA_DATA() occupying exactly `data`. Returns false on a
// length-check, size, or parity failure. On success `payload` holds the
// data() area INCLUDING its 0-1 bytes of zero padding (the framing cannot
// distinguish the two; every payload format that rides here is
// self-delimiting). §4.8's all-padding form (first word zero) succeeds
// with an empty payload.
[[nodiscard]] AC3FORGE_EXPORT bool parse_extra_data(std::span<const std::byte> data,
                                                    std::vector<std::byte>& payload);

}  // namespace ac3::mlp
