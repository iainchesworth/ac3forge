#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"
#include "ac3/mlp/matrix.hpp"
#include "ac3/mlp/predictor.hpp"

// The per-block layer joining the MLP primitives into a working codec:
// WO 96/37048's Fig. 18a encoder / Fig. 18b decoder. Per block of L
// samples: detect and strip B1 constant least-significant bits (the "LSB
// word"), decorrelate the significant words through the lossless predictor,
// pick an entropy table from the block's peak residual level, and emit
// header + Huffman-coded payload. The decoder (Fig. 18b) runs it exactly
// backwards: "First the Huffman table number is read from the header and
// the relevant Huffman table loaded... The initialisation data and the
// filter coefficients from the block header are loaded into the lossless
// decoding filter... [then] insert B1 zero LSBs... add the LSB word."
//
// The header carries the WO's documented inventory - Huffman table number,
// B1, the N-bit LSB word (whose leading N-B1 bits may carry a DC offset),
// filter coefficients, and initialisation data (the first `order` input
// significant-words; the matching first `order` outputs are simply the
// payload's own first samples, per the WO's state-swap rule: "the first n
// input samples and the first n output samples of the encoding filter ...
// are used respectively as the first n output samples and the first n
// input samples of the decoding filter").
//
// PROVISIONAL FIELD LAYOUT: the WO specifies the inventory and several
// individual budgets (5-bit B1, N-bit LSB word, per-coefficient ranges) but
// not a complete normative field order - that lives in the MLP Reference
// Information (docs/concepts/truehd-mlp.md, layer 3). The layout here is a
// self-consistent packing of the WO inventory, documented field by field in
// block.cpp, and expected to be reconciled or replaced when layer 3/4
// sources land. Round trips through THIS layout are exact regardless.

namespace ac3::mlp {

// Which entropy mode the block's payload uses. The WO's "empty" table is
// digital black: no payload, no coefficients, no initialisation ("predictor
// filter coefficients and initialisation data need not be transmitted").
enum class BlockCoding : std::uint8_t {
    kEmpty = 0,        // all-zero block, nothing follows but B1/LSB word
    kPcm = 1,          // WO Table 7: n+1 raw digits per sample
    kSignificant = 2,  // WO Tables 2+3: top-4-varying-digits Huffman + raw remainder
};

struct BlockHeader {
    BlockCoding coding = BlockCoding::kEmpty;
    int n = 0;    // Table 3 / PCM range parameter (unused for kEmpty)
    int b1 = 0;   // stripped constant LSBs, 0..N-1
    std::uint32_t lsb_word = 0;  // N bits: [DC offset (N-b1) | constant LSB pattern (b1)]
    PredictorCoefficients coefficients{};  // empty vectors for kEmpty
    std::vector<std::int32_t> init;        // first max-order input significant words
};

// Header pack/parse alone, for callers assembling their own payloads.
// `wordlength` is N (the stream-level sample width, e.g. 20 or 24) - a
// stream property, not a per-block field, per the WO's worked example.
AC3FORGE_EXPORT void build_block_header(BitWriter& w, const BlockHeader& header, int wordlength);
[[nodiscard]] AC3FORGE_EXPORT bool parse_block_header(BitReader& r, int wordlength,
                                                      BlockHeader& out);

// The assembled block codec. encode_block runs the whole Fig. 18a chain on
// `samples` (each fitting `wordlength` signed bits, excluding the single
// most-negative code - the entropy ranges are asymmetric, see huffman.hpp)
// with the caller's coefficient choice; decode_block reverses it, needing
// only the block length and wordlength from stream-level context. Returns
// false on a malformed header.
AC3FORGE_EXPORT void encode_block(BitWriter& w, std::span<const std::int32_t> samples,
                                  int wordlength, const PredictorCoefficients& coefficients);
[[nodiscard]] AC3FORGE_EXPORT bool decode_block(BitReader& r, int wordlength,
                                                std::span<std::int32_t> samples);

// --- multichannel ----------------------------------------------------------
//
// WO Fig. 3's encoder-core order, per block: each channel is B1-stripped
// first ("each channel is shifted to recover unused capacity"), THEN the
// lossless matrix decorrelates across channels, then "the signal in each
// channel is de-correlated using a separate predictor for each channel"
// and entropy-coded per channel (Fig. 26a: "an initial n x n matrix
// quantizer followed by n separate 1-channel lossless encoding filter
// arrangements ... using a possibly different set of filter and noise
// shaping coefficients for each"). Matrix coefficients ride in the block
// header - the WO's "transmission to the decoder of only n - 1
// coefficients" per primitive stage, sent dense in channel order here.
// The payload interleaves codewords per sample across channels (a
// provisional choice like the field order itself - the WO doesn't pin the
// interleave; per-sample keeps decoder memory flat, which matches MLP's
// design pressure).
struct MultichannelBlockConfig {
    std::vector<matrix::Step> steps;  // PMQ cascade; may be empty
    std::vector<PredictorCoefficients> coefficients;  // exactly one per channel
};

inline constexpr int kMaxBlockChannels = 16;

AC3FORGE_EXPORT void encode_block_channels(BitWriter& w,
                                           std::span<const std::span<const std::int32_t>> channels,
                                           int wordlength, const MultichannelBlockConfig& config);
[[nodiscard]] AC3FORGE_EXPORT bool decode_block_channels(
    BitReader& r, int wordlength, std::span<const std::span<std::int32_t>> channels);

// --- hooks for encoder-side selection --------------------------------------

// The entropy decision the block codec itself makes for a residual signal,
// with its exact payload cost in bits - exported so a coefficient/matrix
// search can rank candidates by the same measure the encoder will actually
// pay, rather than by a proxy.
struct CodingChoice {
    BlockCoding coding = BlockCoding::kEmpty;
    int n = 0;
    long long payload_bits = 0;
};
[[nodiscard]] AC3FORGE_EXPORT CodingChoice choose_coding_cost(
    std::span<const std::int32_t> residual);

// The B1 detection the strip stage uses (WO: "how many B1 of the least
// significant bits have identical form throughout the block"), exported so
// selection can reproduce the significant-word domain the block codec will
// actually encode in.
[[nodiscard]] AC3FORGE_EXPORT int detect_constant_lsbs(std::span<const std::int32_t> samples,
                                                       int wordlength);

}  // namespace ac3::mlp
