#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"

// Mantissa quantization and grouping (A/52 §7.3).
//
// Symmetric quantizers for bap 1-5 (3/5/7/11/15 levels, reconstruction
// values (2k - (L-1))/L per Tables 7.19-7.23); asymmetric fractional two's
// complement for bap 6-15 (qntztab bits, Table 7.18). Grouping (§7.3.5):
// 3-level and 5-level codes pack three-to-a-word (5 and 7 bits), 11-level
// codes two-to-a-word (7 bits); the group codeword sits at the position of
// its FIRST member; grouping state is shared ACROSS exponent sets (channels)
// within one audio block and flushed with dummy mantissas at block end.
// The SNR-search bit counter and the packer must agree exactly, so both are
// built on the same machinery here.

namespace ac3 {

// Bits per directly-coded mantissa (0 for the grouped baps 1, 2, 4 and for
// bap 0) — Table 7.18 qntztab.
inline constexpr std::array<int, 16> kBapBits = {0, 0, 0, 3, 0, 4, 5, 6,
                                                 7, 8, 9, 10, 11, 12, 14, 16};

// Levels of the symmetric quantizers (bap 1-5).
inline constexpr std::array<int, 6> kSymmetricLevels = {0, 3, 5, 7, 11, 15};

// Quantize one normalized mantissa (25-bit fixed point: fixed << decoded
// exponent, representing [-1, 1)) to its bap's code. Symmetric baps return
// the level index; asymmetric baps return the qntztab-bit two's-complement
// pattern.
[[nodiscard]] std::uint32_t quantize_mantissa(std::int32_t mantissa, int bap);

// Reconstruction value in [-1, 1) for a code (test/decoder use).
[[nodiscard]] double dequantize_mantissa(std::uint32_t code, int bap);

// One bitstream write: `bits` bits of `value`.
struct MantissaToken {
    std::uint8_t bits;
    std::uint32_t value;
};

// Builds the mantissa bitstream for ONE audio block across all channels.
// add() mantissas in bitstream order (channel 0 all bins, then channel 1,
// ...); finish_block() pads partial groups with dummy zero codes and
// backfills group codewords. tokens() then yields the exact writes.
class MantissaBlockWriter {
public:
    void add(std::int32_t mantissa, int bap);
    // A pre-formed codeword of a known width, placed in sequence with the
    // rest. The adaptive hybrid transform needs this: its codewords are VQ
    // indices and gain-adaptive mantissas rather than bap-quantized values,
    // and they carry no grouping - but they sit in the same block's mantissa
    // stream as ordinary channels' grouped ones, so they have to go through
    // the same writer to keep the ordering and the group backfill straight.
    void add_raw(std::uint32_t value, int bits);
    void finish_block();
    [[nodiscard]] std::size_t bit_count() const { return bit_count_; }
    [[nodiscard]] const std::vector<MantissaToken>& tokens() const { return tokens_; }

private:
    struct PendingGroup {
        int token_index = -1;
        int count = 0;
        std::uint32_t radix = 0;  // 3 / 5 for triplet groups; unused for pairs
        std::array<std::uint32_t, 3> codes{};
    };

    void add_grouped(PendingGroup& group, std::uint32_t code, int members, int bits);
    static std::uint32_t pack_group(const PendingGroup& group, int members);

    PendingGroup bap1_;
    PendingGroup bap2_;
    PendingGroup bap4_;
    std::vector<MantissaToken> tokens_;
    std::size_t bit_count_ = 0;
};

// Fast bit count for one block given per-channel bap arrays — must equal
// what MantissaBlockWriter emits (property-tested). Grouped baps cost
// ceil(count/members) codewords per block.
[[nodiscard]] std::size_t mantissa_bits_per_block(
    std::span<const std::span<const std::uint8_t>> channel_baps);

// The mirror of MantissaBlockWriter: reads ONE audio block's mantissas in
// bitstream order. A group's codeword arrives at the position of its FIRST
// member and later members consume nothing, so the reader has to carry the
// unpacked remainder forward. State is shared across exponent sets within a
// block and discarded at block end, where the writer's dummy padding sits.
// AC-3 and E-AC-3 group mantissas identically, so both decoders use this.
class MantissaBlockReader {
public:
    [[nodiscard]] std::uint32_t read(BitReader& reader, int bap);

private:
    struct Cache {
        int remaining = 0;
        std::array<std::uint32_t, 2> values{};
    };

    [[nodiscard]] static std::uint32_t read_group(Cache& cache, BitReader& reader, int bits,
                                                  std::uint32_t radix, int members);

    Cache bap1_;
    Cache bap2_;
    Cache bap4_;
};

}  // namespace ac3
