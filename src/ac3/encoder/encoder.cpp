#include "ac3/encoder/encoder.hpp"

#include <algorithm>
#include <cassert>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"

namespace ac3 {

namespace {

constexpr int kChannels = 2;  // acmod 2/0

// Side-information bits of the fixed frame layout (everything except
// mantissa tokens, skip payloads, aux fill, and the 18-bit tail), as a
// function of the D15 exponent group count. Cross-checked against the
// Milestone-2 silent frame: groups = 24 gives the audited 553 bits.
constexpr std::uint32_t side_info_bits(int groups) {
    const std::uint32_t block0 = 28 + 2 * (6 + static_cast<std::uint32_t>(groups) * 7) + 12 + 21 + 2;
    return detail::kSyncinfoBsiBits + block0 + 5 * detail::kReuseBlockBits;
}
static_assert(side_info_bits(24) == detail::kContentBits);

}  // namespace

std::expected<std::vector<std::byte>, FrameError> StereoEncoder::encode_frame(
    std::span<const float> left, std::span<const float> right) {
    const auto index = bitrate_index(config_.bitrate_kbps);
    if (!index) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    if (config_.dialnorm < 1 || config_.dialnorm > 31) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }
    assert(left.size() == kSamplesPerFrame && right.size() == kSamplesPerFrame);
    assert(config_.chbwcod >= 0 && config_.chbwcod <= 60);

    const int endmant = ((config_.chbwcod + 12) * 3) + 37;
    const std::uint32_t words = *frame_size_words(config_.sample_rate, config_.bitrate_kbps,
                                                  config_.pad441);
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;
    const std::uint32_t words58 = frame_size_58_words(words);
    const BitAllocCodes codes{};  // §8.2.12 basic-encoder defaults

    // --- 1. Analysis: MDCT, fixed-point conversion, exponent extraction ---
    // fixed[ch][block][bin]; raw exponents are minimized across the six
    // blocks so that block-0 exponents are safe for every reuse block.
    std::array<std::span<const float>, kChannels> input = {left, right};
    std::vector<std::int32_t> fixed(static_cast<std::size_t>(kChannels) * kBlocksPerFrame *
                                    static_cast<std::size_t>(endmant));
    const auto fixed_at = [&](int ch, int block, int bin) -> std::int32_t& {
        return fixed[(static_cast<std::size_t>(ch) * kBlocksPerFrame +
                      static_cast<std::size_t>(block)) *
                         static_cast<std::size_t>(endmant) +
                     static_cast<std::size_t>(bin)];
    };

    std::array<std::vector<std::uint8_t>, kChannels> raw_exps;
    std::array<EncodedExponents, kChannels> encoded_exps;
    std::array<std::vector<std::uint8_t>, kChannels> decoded_exps;
    for (int ch = 0; ch < kChannels; ++ch) {
        raw_exps[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(endmant),
                                                      kMaxExponent);
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            // Block b transforms 512 samples starting 256 before b*256,
            // reaching into the previous frame's tail for block 0.
            std::array<double, 512> time{};
            for (int n = 0; n < 512; ++n) {
                const int pos = block * 256 - 256 + n;
                time[static_cast<std::size_t>(n)] =
                    pos < 0 ? history_[static_cast<std::size_t>(ch)]
                                      [static_cast<std::size_t>(pos + 256)]
                            : static_cast<double>(input[static_cast<std::size_t>(ch)]
                                                       [static_cast<std::size_t>(pos)]);
            }
            std::array<double, 512> windowed{};
            std::array<double, 256> coeffs{};
            apply_analysis_window(time, windowed);
            mdct512_forward(windowed, coeffs);
            for (int bin = 0; bin < endmant; ++bin) {
                const std::int32_t f = to_fixed25(coeffs[static_cast<std::size_t>(bin)]);
                fixed_at(ch, block, bin) = f;
                auto& raw = raw_exps[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                raw = std::min(raw, static_cast<std::uint8_t>(exponent_from_fixed(f)));
            }
        }
        encoded_exps[static_cast<std::size_t>(ch)] =
            encode_exponents(raw_exps[static_cast<std::size_t>(ch)], ExpStrategy::kD15);
        decoded_exps[static_cast<std::size_t>(ch)].resize(static_cast<std::size_t>(endmant));
        decode_exponents(encoded_exps[static_cast<std::size_t>(ch)].absolute,
                         encoded_exps[static_cast<std::size_t>(ch)].groups, ExpStrategy::kD15,
                         decoded_exps[static_cast<std::size_t>(ch)]);
        // Update the overlap history from this frame's final 256 samples.
        for (int n = 0; n < 256; ++n) {
            history_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] =
                static_cast<double>(
                    input[static_cast<std::size_t>(ch)][static_cast<std::size_t>(1280 + n)]);
        }
    }

    // --- 2. SNR-offset search: the largest composite (csnroffst, fsnroffst)
    // whose six blocks of mantissas fit the frame's budget (§8.2.12). ---
    const int groups = exponent_group_count(ExpStrategy::kD15, endmant);
    const std::uint32_t side_bits = side_info_bits(groups);
    assert(side_bits + detail::kTailBits <= total_bits);
    const std::uint32_t budget = total_bits - side_bits - detail::kTailBits;

    std::array<std::vector<std::uint8_t>, kChannels> bap;
    for (auto& b : bap) {
        b.resize(static_cast<std::size_t>(endmant));
    }
    const auto bits_at = [&](int composite) {
        for (int ch = 0; ch < kChannels; ++ch) {
            compute_bit_allocation(decoded_exps[static_cast<std::size_t>(ch)],
                                   config_.sample_rate, codes, composite >> 4, composite & 15,
                                   bap[static_cast<std::size_t>(ch)]);
        }
        const std::array<std::span<const std::uint8_t>, kChannels> views = {bap[0], bap[1]};
        return static_cast<std::uint32_t>(kBlocksPerFrame * mantissa_bits_per_block(views));
    };

    int lo = 0;
    int hi = 1023;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (bits_at(mid) <= budget) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const int csnroffst = lo >> 4;
    const int fsnroffst = lo & 15;
    const std::uint32_t mantissa_bits = bits_at(lo);  // leaves bap[] at the final allocation
    assert(mantissa_bits <= budget);

    // --- 3. Quantize mantissas into per-block token streams (§7.3.5:
    // grouping shared across channels within a block, flushed at block end).
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> block_tokens;
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        MantissaBlockWriter writer;
        for (int ch = 0; ch < kChannels; ++ch) {
            for (int bin = 0; bin < endmant; ++bin) {
                const int exp =
                    decoded_exps[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(fixed_at(ch, block, bin)) << exp);
                writer.add(mantissa, bap[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)]);
            }
        }
        writer.finish_block();
        // Identical bap arrays every block -> identical per-block bit counts.
        assert(writer.bit_count() == mantissa_bits / kBlocksPerFrame);
        block_tokens[static_cast<std::size_t>(block)] = writer.tokens();
    }

    // --- 4. Padding plan for the leftover bits. ---
    const auto plan = detail::plan_padding(budget - mantissa_bits);

    // --- 5. Pack the frame. ---
    BitWriter w;
    w.put(kSyncWord, 16);
    w.put(0, 16);  // crc1, patched below
    w.put(static_cast<std::uint32_t>(config_.sample_rate), 2);
    const bool pad = config_.sample_rate == SampleRate::k44100 && config_.pad441;
    w.put(static_cast<std::uint32_t>(*index) * 2 + (pad ? 1u : 0u), 6);

    w.put(8, 5);                                        // bsid
    w.put(0, 3);                                        // bsmod
    w.put(static_cast<std::uint32_t>(Acmod::k2_0), 3);  // acmod
    w.put(0, 2);                                        // dsurmod
    w.put(0, 1);                                        // lfeon
    w.put(static_cast<std::uint32_t>(config_.dialnorm), 5);
    w.put(0, 1);  // compre
    w.put(0, 1);  // langcode
    w.put(0, 1);  // audprodie
    w.put(0, 1);  // copyrightb
    w.put(1, 1);  // origbs
    w.put(0, 1);  // timecod1e
    w.put(0, 1);  // timecod2e
    w.put(0, 1);  // addbsie

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        const bool first = block == 0;
        for (int ch = 0; ch < kChannels; ++ch) {
            w.put(0, 1);  // blksw
        }
        for (int ch = 0; ch < kChannels; ++ch) {
            w.put(0, 1);  // dithflag: off -> bap-0 bins decode to true zero
        }
        w.put(0, 1);              // dynrnge
        w.put(first ? 1 : 0, 1);  // cplstre
        if (first) {
            w.put(0, 1);  // cplinu
        }
        w.put(first ? 1 : 0, 1);  // rematstr
        if (first) {
            for (int band = 0; band < 4; ++band) {
                w.put(0, 1);  // rematflg
            }
        }
        for (int ch = 0; ch < kChannels; ++ch) {
            w.put(static_cast<std::uint32_t>(first ? ExpStrategy::kD15 : ExpStrategy::kReuse), 2);
        }
        if (first) {
            for (int ch = 0; ch < kChannels; ++ch) {
                w.put(static_cast<std::uint32_t>(config_.chbwcod), 6);
            }
            for (int ch = 0; ch < kChannels; ++ch) {
                const auto& e = encoded_exps[static_cast<std::size_t>(ch)];
                w.put(e.absolute, 4);
                for (const auto group : e.groups) {
                    w.put(group, 7);
                }
                w.put(0, 2);  // gainrng
            }
        }
        w.put(first ? 1 : 0, 1);  // baie
        if (first) {
            w.put(static_cast<std::uint32_t>(codes.sdcycod), 2);
            w.put(static_cast<std::uint32_t>(codes.fdcycod), 2);
            w.put(static_cast<std::uint32_t>(codes.sgaincod), 2);
            w.put(static_cast<std::uint32_t>(codes.dbpbcod), 2);
            w.put(static_cast<std::uint32_t>(codes.floorcod), 3);
        }
        w.put(first ? 1 : 0, 1);  // snroffste
        if (first) {
            w.put(static_cast<std::uint32_t>(csnroffst), 6);
            for (int ch = 0; ch < kChannels; ++ch) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);
            }
        }
        w.put(0, 1);  // deltbaie

        const std::uint16_t skip = plan.skip_bytes[static_cast<std::size_t>(block)];
        w.put(skip > 0 ? 1 : 0, 1);  // skiple
        if (skip > 0) {
            w.put(skip, 9);
            for (std::uint16_t i = 0; i < skip; ++i) {
                w.put(0, 8);
            }
        }
        for (const auto& token : block_tokens[static_cast<std::size_t>(block)]) {
            w.put(token.value, token.bits);
        }
    }

    assert(w.bit_count() + plan.aux_bits + detail::kTailBits == total_bits);
    for (std::uint32_t i = 0; i < plan.aux_bits; ++i) {
        w.put(0, 1);
    }
    w.put(0, 1);   // auxdatae
    w.put(0, 1);   // crcrsv
    w.put(0, 16);  // crc2, patched below
    assert(w.bit_count() == total_bits);

    std::vector<std::byte> frame = w.take();
    const std::span<const std::byte> view{frame};
    const std::uint16_t crc1 = solve_leading_crc(view.subspan(4, 2 * words58 - 4));
    frame[2] = static_cast<std::byte>(crc1 >> 8);
    frame[3] = static_cast<std::byte>(crc1 & 0xFF);
    std::uint16_t crc2 = crc16(view.subspan(2, total_bytes - 4));
    if (crc2 == kSyncWord) {
        frame[total_bytes - 3] ^= std::byte{0x01};
        crc2 = crc16(view.subspan(2, total_bytes - 4));
    }
    frame[total_bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[total_bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
    return frame;
}

}  // namespace ac3
