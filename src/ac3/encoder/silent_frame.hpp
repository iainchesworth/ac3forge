#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ac3/core/bitwriter.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/tables.hpp"

// Milestone 1-2: build complete, spec-conformant AC-3 syncframes encoding
// digital silence in 2/0 (stereo) mode. Every field below is written in the
// exact order and width of ATSC A/52:2018 Tables 5.1-5.5 (syncinfo, bsi,
// audblk, auxdata, errorcheck), with the block-0 "shall" constraints of
// §5.4.3 respected. There is no DSP here yet: silence lets the whole frame
// be constructed from syntax knowledge alone. The decoder allocates zero
// mantissa bits (bap = 0 in every bin) on two independent grounds: all SNR
// offsets are transmitted as zero, which §7.2.2.1.1 defines as "the bit
// allocation pointers are all zero"; and the transmitted exponents (a ramp
// from the absolute field's maximum of 15 up to the ceiling of 24, flat
// after) keep psd below the masking curve in every bin under the full §7.2
// integer model as well. Either way, no mantissa data exists to write.

namespace ac3 {

struct SilentFrameConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    // A/52 §5.4.2.8: 1..31 meaning -1..-31 dB relative to digital 100%; 0 is
    // reserved. 31 (no attenuation) keeps decoded PCM comparisons clean.
    int dialnorm = 31;
    // 44.1 kHz only: select the odd frmsizecod, one word longer (Table 5.18).
    bool pad441 = false;
};

enum class FrameError {
    kInvalidBitrate,
    kInvalidDialnorm,
};

namespace detail {

// Differential exponents for one D15-coded silent channel: climb from the
// 4-bit absolute exponent field's maximum of 15 (§7.1.2: "exponent values
// larger than 15 are limited to a value of 15") to the overall exponent
// ceiling of 24 in +2/+1 steps, then stay flat. Mapped value = diff + 2
// (Table 7.1); three mapped values pack into one 7-bit group as
// 25*M1 + 5*M2 + M3 (§7.1.2).
//
// chbwcod = 0 gives endmant = (0 + 12)*3 + 37 = 73 mantissas (§7.1.3), so
// 72 differentials -> nchgrps = (73-1)/3 = 24 groups in D15 (§7.1.3).
inline constexpr int kSilentChbwcod = 0;
inline constexpr int kSilentEndmant = ((kSilentChbwcod + 12) * 3) + 37;
inline constexpr int kSilentNchgrps = (kSilentEndmant - 1) / 3;
static_assert(kSilentEndmant == 73 && kSilentNchgrps == 24);

consteval std::array<std::uint8_t, kSilentNchgrps> silent_d15_groups() {
    std::array<int, kSilentEndmant - 1> mapped{};
    int exponent = 15;
    for (auto& m : mapped) {
        const int diff = exponent + 2 <= 24 ? 2 : 24 - exponent;
        exponent += diff;
        m = diff + 2;
    }
    std::array<std::uint8_t, kSilentNchgrps> groups{};
    for (int g = 0; g < kSilentNchgrps; ++g) {
        groups[static_cast<std::size_t>(g)] =
            static_cast<std::uint8_t>(25 * mapped[3 * g] + 5 * mapped[3 * g + 1] + mapped[3 * g + 2]);
    }
    return groups;
}

inline constexpr auto kSilentExpGroups = silent_d15_groups();
// First groups encode the climb (+2,+2,+2 | +2,+1,0), the rest are flat.
static_assert(kSilentExpGroups[0] == 124 && kSilentExpGroups[1] == 117 &&
              kSilentExpGroups[2] == 62 && kSilentExpGroups[kSilentNchgrps - 1] == 62);

// Exact bit counts of the fixed silent-frame content (everything except skip
// payloads, aux fill, and the 18-bit auxdatae/crcrsv/crc2 tail), used to plan
// padding before packing. Block 0: 28 flag/strategy bits + 2*(4 + 24*7 + 2)
// exponent bits + 12 bit-allocation bits + 21 SNR-offset bits + deltbaie +
// skiple. Blocks 1-5 are the 15-bit full-reuse form.
inline constexpr std::uint32_t kSyncinfoBsiBits = 40 + 27;
inline constexpr std::uint32_t kBlock0Bits = 411;
inline constexpr std::uint32_t kReuseBlockBits = 15;
inline constexpr std::uint32_t kContentBits =
    kSyncinfoBsiBits + kBlock0Bits + (kBlocksPerFrame - 1) * kReuseBlockBits;  // 553
inline constexpr std::uint32_t kTailBits = 18;  // auxdatae + crcrsv + crc2

// A/52 §5.5 imposes two "shall" constraints on where frame slack may live:
// the aux + errorcheck fields (plus block-5 mantissa data) must fit in the
// final 3/8 of the syncframe, and syncinfo + bsi + blocks 0-1 must fit in the
// first 5/8. So CBR padding cannot simply be dumped into auxbits; the
// sanctioned sink is the in-block skip field (§5.3.3: skipl up to 511 bytes
// of dummy data per block, counted as used bits per §5.5). Fill skips from
// the LAST block backward: early blocks stay lean for the 5/8 bullet, and at
// most a byte-remainder (< 17 bits) is left for auxbits unless every block's
// skip capacity is exhausted (only possible in the largest 32 kHz frames,
// where the residue still fits the final-3/8 budget with room to spare).
struct SkipPlan {
    std::array<std::uint16_t, kBlocksPerFrame> skip_bytes{};
    std::uint32_t aux_bits = 0;
};

[[nodiscard]] constexpr SkipPlan plan_padding(std::uint32_t pad_bits) {
    SkipPlan plan{};
    for (int block = kBlocksPerFrame - 1; block >= 0; --block) {
        if (pad_bits < 9 + 8) {
            break;
        }
        const std::uint32_t bytes = std::min<std::uint32_t>(511, (pad_bits - 9) / 8);
        plan.skip_bytes[static_cast<std::size_t>(block)] = static_cast<std::uint16_t>(bytes);
        pad_bits -= 9 + 8 * bytes;
    }
    plan.aux_bits = pad_bits;
    return plan;
}

}  // namespace detail

// One complete 2/0 silent syncframe. The two CRC words are solved after
// packing: crc1 so the register reads zero at the 5/8 point (§7.10.1), crc2
// so it reads zero over the whole frame (sync word excluded from both).
[[nodiscard]] inline std::expected<std::vector<std::byte>, FrameError> build_silent_stereo_frame(
    const SilentFrameConfig& config) {
    const auto index = bitrate_index(config.bitrate_kbps);
    if (!index) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    if (config.dialnorm < 1 || config.dialnorm > 31) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }

    const std::uint32_t words = *frame_size_words(config.sample_rate, config.bitrate_kbps,
                                                  config.pad441);
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;
    const std::uint32_t words58 = frame_size_58_words(words);
    const auto plan =
        detail::plan_padding(total_bits - detail::kContentBits - detail::kTailBits);

    BitWriter w;

    // --- syncinfo (Table 5.1) ---
    w.put(kSyncWord, 16);
    w.put(0, 16);  // crc1: patched below once the frame is packed
    w.put(static_cast<std::uint32_t>(config.sample_rate), 2);            // fscod
    const bool pad = config.sample_rate == SampleRate::k44100 && config.pad441;
    w.put(static_cast<std::uint32_t>(*index) * 2 + (pad ? 1u : 0u), 6);  // frmsizecod

    // --- bsi (Table 5.2), 2/0 mode, no optional info blocks ---
    w.put(8, 5);                                        // bsid (§5.4.2.1)
    w.put(0, 3);                                        // bsmod: complete main
    w.put(static_cast<std::uint32_t>(Acmod::k2_0), 3);  // acmod
    w.put(0, 2);                                        // dsurmod: not indicated
    w.put(0, 1);                                        // lfeon
    w.put(static_cast<std::uint32_t>(config.dialnorm), 5);
    w.put(0, 1);  // compre
    w.put(0, 1);  // langcode
    w.put(0, 1);  // audprodie
    w.put(0, 1);  // copyrightb
    w.put(1, 1);  // origbs: this is an original bit stream
    w.put(0, 1);  // timecod1e
    w.put(0, 1);  // timecod2e
    w.put(0, 1);  // addbsie

    // --- 6 audio blocks (Table 5.3) ---
    constexpr int kNfchans = fullbw_channel_count(Acmod::k2_0);
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        const bool first = block == 0;
        for (int ch = 0; ch < kNfchans; ++ch) {
            w.put(0, 1);  // blksw: long blocks only
        }
        for (int ch = 0; ch < kNfchans; ++ch) {
            w.put(0, 1);  // dithflag: no dither -> true digital silence
        }
        w.put(0, 1);  // dynrnge: no dynamic range word

        // Coupling strategy: shall be present in block 0 (§5.4.3.7).
        w.put(first ? 1 : 0, 1);  // cplstre
        if (first) {
            w.put(0, 1);  // cplinu: coupling not in use
        }

        // Rematrixing, 2/0 mode: shall be present in block 0 (§5.4.3.19).
        w.put(first ? 1 : 0, 1);  // rematstr
        if (first) {
            for (int band = 0; band < 4; ++band) {  // cplinu==0 -> 4 bands (Table 5.15)
                w.put(0, 1);                        // rematflg: no rematrixing
            }
        }

        // Exponent strategy: block 0 shall not reuse (§5.4.3.22).
        for (int ch = 0; ch < kNfchans; ++ch) {
            w.put(static_cast<std::uint32_t>(first ? ExpStrategy::kD15 : ExpStrategy::kReuse), 2);
        }
        if (first) {
            for (int ch = 0; ch < kNfchans; ++ch) {
                w.put(detail::kSilentChbwcod, 6);  // chbwcod
            }
            for (int ch = 0; ch < kNfchans; ++ch) {
                w.put(15, 4);  // exps[ch][0]: absolute exponent (field max)
                for (const auto group : detail::kSilentExpGroups) {
                    w.put(group, 7);
                }
                w.put(0, 2);  // gainrng
            }
        }

        // Bit allocation info: shall be present in block 0 (§5.4.3.30).
        w.put(first ? 1 : 0, 1);  // baie
        if (first) {
            w.put(2, 2);  // sdcycod  (Table 7.6 slow decay)
            w.put(1, 2);  // fdcycod  (Table 7.7 fast decay)
            w.put(1, 2);  // sgaincod (Table 7.8 slow gain)
            w.put(2, 2);  // dbpbcod  (Table 7.9 dB per bit)
            w.put(7, 3);  // floorcod (Table 7.10 masking floor)
        }

        // SNR offsets: shall be present in block 0 (§5.4.3.36). All-zero
        // offsets are the §7.2.2.1.1 special case: every bap is zero.
        w.put(first ? 1 : 0, 1);  // snroffste
        if (first) {
            w.put(0, 6);  // csnroffst
            for (int ch = 0; ch < kNfchans; ++ch) {
                w.put(0, 4);  // fsnroffst
                w.put(0, 3);  // fgaincod
            }
        }

        // cplinu == 0: no coupling leak fields.
        w.put(0, 1);  // deltbaie: legal in block 0 -> "no delta allocation" (§5.4.3.47)

        // Frame padding lives here as skip-field dummy data (see SkipPlan).
        const std::uint16_t skip = plan.skip_bytes[static_cast<std::size_t>(block)];
        w.put(skip > 0 ? 1 : 0, 1);  // skiple
        if (skip > 0) {
            w.put(skip, 9);  // skipl
            for (std::uint16_t i = 0; i < skip; ++i) {
                w.put(0, 8);  // skipfld
            }
        }
        // Mantissas: every bap is 0 (see file comment), so no mantissa bits.
    }

    // --- auxdata + errorcheck (Tables 5.4, 5.5): the byte-remainder of the
    // padding plan lands in auxbits so crc2 falls on the final word (§5.4.4).
    // Last 18 bits: auxdatae, crcrsv, crc2. ---
    assert(w.bit_count() + plan.aux_bits + detail::kTailBits == total_bits);
    for (std::uint32_t i = 0; i < plan.aux_bits; ++i) {
        w.put(0, 1);  // auxbits
    }
    w.put(0, 1);   // auxdatae
    w.put(0, 1);   // crcrsv
    w.put(0, 16);  // crc2: patched below
    assert(w.bit_count() == total_bits);

    std::vector<std::byte> frame = w.take();

    // crc1 covers bytes [2, 2*words58); its own two bytes lead the region.
    const std::span<const std::byte> frame_view{frame};
    w = {};  // done with the writer
    std::uint16_t crc1 = solve_leading_crc(frame_view.subspan(4, 2 * words58 - 4));
    frame[2] = static_cast<std::byte>(crc1 >> 8);
    frame[3] = static_cast<std::byte>(crc1 & 0xFF);

    // crc2 covers the whole frame minus the sync word; as the trailing word
    // it is the plain CRC of everything before it. If it would collide with
    // the sync word, invert crcrsv and recompute (§5.4.5.1).
    std::uint16_t crc2 = crc16(frame_view.subspan(2, total_bytes - 4));
    if (crc2 == kSyncWord) {
        frame[total_bytes - 3] ^= std::byte{0x01};  // crcrsv is that byte's lsb
        crc2 = crc16(frame_view.subspan(2, total_bytes - 4));
    }
    frame[total_bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[total_bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);

    return frame;
}

}  // namespace ac3
