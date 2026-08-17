#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "ac3/decoder/decoder.hpp"
#include "ac3/meta/drc.hpp"

// The §7.7 gain math both decoders apply, shared so a future correction to
// the partial-compression exponent or the compr-fallback rule only has one
// place to land. Internal to src/lib/src/decoder/ on purpose - this is
// plumbing between the two decoder translation units (decoder.cpp and
// eac3_decoder.cpp), not library surface - the same convention
// src/lib/src/encoder/snr_search.hpp uses for its own cross-TU helper.

namespace ac3::internal {

// The §7.7 gain for one block, resolving which of the two control signals
// applies. §7.7.2.1: a decoder told to use compr falls back on dynrng for any
// syncframe with no compr word, so heavy compression is a preference and not a
// mode switch. A dependent E-AC-3 substream's compr is always std::nullopt
// (see DecodedSubstream::compr's own comment), so this composes correctly
// there too without any substream-type check.
[[nodiscard]] inline double block_gain(const DecoderConfig& config, std::uint8_t dynrng_word,
                                       std::optional<std::uint8_t> compr) {
    if (config.heavy_compression && compr) {
        // §7.7.2 states no partial-compression scaling: compr's whole purpose
        // is a hard ceiling, and a decoder that applied a fraction of it would
        // be promising a ceiling it does not deliver.
        return meta::compr_gain(*compr);
    }
    if (config.drc_scale == 0.0 || dynrng_word == meta::kDynrngUnity) {
        return 1.0;
    }
    const double gain = meta::dynrng_gain(dynrng_word);
    // §7.7.1's "Partial Compression" scales the word as a signed fraction of
    // dB, which in the linear domain is exactly raising the gain to that
    // power. Doing it here rather than on the bits avoids re-quantising.
    return config.drc_scale == 1.0 ? gain : std::pow(gain, config.drc_scale);
}

}  // namespace ac3::internal
