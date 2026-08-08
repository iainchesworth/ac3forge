#include "ac3/core/mantissas.hpp"

#include <algorithm>
#include <cassert>

namespace ac3 {

std::uint32_t quantize_mantissa(std::int32_t mantissa, int bap) {
    assert(bap >= 1 && bap <= 15);
    if (bap <= 5) {
        // Symmetric: nearest of L levels at (2k - (L-1))/L. In 25-bit fixed
        // point: code = floor((m*L + (L-1)*2^24 + 2^24) / 2^25), clamped.
        const auto levels = static_cast<std::int64_t>(kSymmetricLevels[static_cast<std::size_t>(bap)]);
        const std::int64_t numerator = static_cast<std::int64_t>(mantissa) * levels +
                                       ((levels - 1) << 24) + (std::int64_t{1} << 24);
        const auto code = std::clamp<std::int64_t>(numerator >> 25, 0, levels - 1);
        return static_cast<std::uint32_t>(code);
    }
    // Asymmetric: round to a qntztab-bit two's complement fraction.
    const int bits = kBapBits[static_cast<std::size_t>(bap)];
    const int shift = 25 - bits;
    std::int64_t code = (static_cast<std::int64_t>(mantissa) + (std::int64_t{1} << (shift - 1))) >>
                        shift;
    code = std::clamp<std::int64_t>(code, -(std::int64_t{1} << (bits - 1)),
                                    (std::int64_t{1} << (bits - 1)) - 1);
    return static_cast<std::uint32_t>(code) & ((1u << bits) - 1);
}

double dequantize_mantissa(std::uint32_t code, int bap) {
    assert(bap >= 1 && bap <= 15);
    if (bap <= 5) {
        const int levels = kSymmetricLevels[static_cast<std::size_t>(bap)];
        return (2.0 * static_cast<int>(code) - (levels - 1)) / levels;
    }
    const int bits = kBapBits[static_cast<std::size_t>(bap)];
    auto value = static_cast<std::int32_t>(code << (32 - bits)) >> (32 - bits);  // sign extend
    return static_cast<double>(value) / static_cast<double>(1u << (bits - 1));
}

std::uint32_t MantissaBlockWriter::pack_group(const PendingGroup& group, int members) {
    if (members == 3) {
        // bap 1: 9a+3b+c; bap 2: 25a+5b+c — radix = levels.
        return group.codes[0] * group.radix * group.radix +
               group.codes[1] * group.radix + group.codes[2];
    }
    return group.codes[0] * 11 + group.codes[1];  // bap 4 pairs
}

void MantissaBlockWriter::add_grouped(PendingGroup& group, std::uint32_t code, int members,
                                      int bits) {
    if (group.count == 0) {
        group.token_index = static_cast<int>(tokens_.size());
        tokens_.push_back({static_cast<std::uint8_t>(bits), 0});
        bit_count_ += static_cast<std::size_t>(bits);
    }
    group.codes[static_cast<std::size_t>(group.count)] = code;
    ++group.count;
    if (group.count == members) {
        tokens_[static_cast<std::size_t>(group.token_index)].value = pack_group(group, members);
        group.count = 0;
    }
}

void MantissaBlockWriter::add(std::int32_t mantissa, int bap) {
    if (bap == 0) {
        return;  // no bits; decoder substitutes silence (dithflag = 0) or dither
    }
    const std::uint32_t code = quantize_mantissa(mantissa, bap);
    switch (bap) {
        case 1:
            bap1_.radix = 3;
            add_grouped(bap1_, code, 3, 5);
            break;
        case 2:
            bap2_.radix = 5;
            add_grouped(bap2_, code, 3, 7);
            break;
        case 4:
            add_grouped(bap4_, code, 2, 7);
            break;
        default:
            tokens_.push_back({static_cast<std::uint8_t>(kBapBits[static_cast<std::size_t>(bap)]),
                               code});
            bit_count_ += static_cast<std::size_t>(kBapBits[static_cast<std::size_t>(bap)]);
            break;
    }
}

void MantissaBlockWriter::add_raw(std::uint32_t value, int bits) {
    if (bits <= 0) {
        return;
    }
    tokens_.push_back({static_cast<std::uint8_t>(bits), value});
    bit_count_ += static_cast<std::size_t>(bits);
}

void MantissaBlockWriter::finish_block() {
    // §7.3.5: partial groups at block end are completed with dummy mantissas
    // (code 0 is always legal); dummies are ignored by the decoder.
    const auto flush = [this](PendingGroup& group, int members) {
        if (group.count > 0) {
            while (group.count < members) {
                group.codes[static_cast<std::size_t>(group.count)] = 0;
                ++group.count;
            }
            tokens_[static_cast<std::size_t>(group.token_index)].value =
                pack_group(group, members);
            group.count = 0;
        }
    };
    flush(bap1_, 3);
    flush(bap2_, 3);
    flush(bap4_, 2);
}

std::size_t mantissa_bits_per_block(
    std::span<const std::span<const std::uint8_t>> channel_baps) {
    std::size_t direct = 0;
    std::size_t count1 = 0;
    std::size_t count2 = 0;
    std::size_t count4 = 0;
    for (const auto& baps : channel_baps) {
        for (const auto bap : baps) {
            switch (bap) {
                case 0: break;
                case 1: ++count1; break;
                case 2: ++count2; break;
                case 4: ++count4; break;
                default: direct += static_cast<std::size_t>(kBapBits[bap]); break;
            }
        }
    }
    return direct + 5 * ((count1 + 2) / 3) + 7 * ((count2 + 2) / 3) + 7 * ((count4 + 1) / 2);
}

std::uint32_t MantissaBlockReader::read_group(Cache& cache, BitReader& reader, int bits,
                                              std::uint32_t radix, int members) {
    if (cache.remaining == 0) {
        const std::uint32_t group = reader.read(bits);
        if (members == 3) {
            cache.values = {(group % (radix * radix)) / radix, group % radix};
            cache.remaining = 2;
            return group / (radix * radix);
        }
        cache.values = {group % radix, 0};
        cache.remaining = 1;
        return group / radix;
    }
    // values holds the members after the first, oldest first.
    const std::uint32_t next = cache.values[static_cast<std::size_t>(
        members == 3 ? 2 - cache.remaining : 1 - cache.remaining)];
    --cache.remaining;
    return next;
}

std::uint32_t MantissaBlockReader::read(BitReader& reader, int bap) {
    // §7.3.5: 3-level codes pack three to a 5-bit word, 5-level three to a
    // 7-bit word, 11-level two to a 7-bit word. Everything else is direct.
    switch (bap) {
        case 1: return read_group(bap1_, reader, 5, 3, 3);
        case 2: return read_group(bap2_, reader, 7, 5, 3);
        case 4: return read_group(bap4_, reader, 7, 11, 2);
        default: return reader.read(kBapBits[static_cast<std::size_t>(bap)]);
    }
}

}  // namespace ac3
