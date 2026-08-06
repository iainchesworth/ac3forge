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

constexpr int kLfeEndmant = 7;
constexpr int kCmixlev = 1;    // -4.5 dB center downmix (Table 5.9)
constexpr int kSurmixlev = 1;  // -6 dB surround downmix (Table 5.10)

constexpr bool has_three_front(Acmod acmod) {
    const auto value = static_cast<std::uint8_t>(acmod);
    return (value & 0x1) != 0 && acmod != Acmod::k1_0;
}

constexpr bool has_surround(Acmod acmod) {
    return (static_cast<std::uint8_t>(acmod) & 0x4) != 0;
}

// Side-information bits of the fixed frame layout (everything except
// mantissa tokens, skip payloads, aux fill, and the 18-bit tail).
constexpr std::uint32_t side_info_bits(Acmod acmod, bool lfe, int groups) {
    const auto nf = static_cast<std::uint32_t>(fullbw_channel_count(acmod));
    const std::uint32_t g = static_cast<std::uint32_t>(groups);

    std::uint32_t bsi = 25;
    if (has_three_front(acmod)) bsi += 2;   // cmixlev
    if (has_surround(acmod)) bsi += 2;      // surmixlev
    if (acmod == Acmod::k2_0) bsi += 2;     // dsurmod
    // (acmod == 0 dual mono would add the ch2 info block; unsupported here)

    // Block 0: flags + strategies + exponents + bai + snr offsets.
    std::uint32_t block0 = 2 * nf + 1;                    // blksw, dithflag, dynrnge
    block0 += 2;                                          // cplstre + cplinu
    if (acmod == Acmod::k2_0) block0 += 1 + 4;            // rematstr + 4 rematflg
    block0 += 2 * nf;                                     // chexpstr
    if (lfe) block0 += 1;                                 // lfeexpstr
    block0 += 6 * nf;                                     // chbwcod
    block0 += nf * (4 + g * 7 + 2);                       // exps + gainrng
    if (lfe) block0 += 4 + 2 * 7;                         // lfeexps (no gainrng)
    block0 += 1 + 11;                                     // baie + parameters
    block0 += 1 + 6 + nf * 7 + (lfe ? 7 : 0);             // snroffste block
    block0 += 1 + 1;                                      // deltbaie + skiple

    // Blocks 1-5: the all-reuse form.
    std::uint32_t reuse = 2 * nf + 1 + 1 + 2 * nf + 1 + 1 + 1 + 1;
    if (acmod == Acmod::k2_0) reuse += 1;  // rematstr
    if (lfe) reuse += 1;                   // lfeexpstr

    return 16 + 16 + 2 + 6 + bsi + block0 + 5 * reuse;  // syncinfo + bsi + blocks
}

// Cross-check against the audited Milestone-2 stereo layout.
static_assert(side_info_bits(Acmod::k2_0, false, 24) == detail::kContentBits);

}  // namespace

std::expected<std::vector<std::byte>, FrameError> FrameEncoder::encode_frame(
    std::span<const std::span<const float>> channels) {
    const auto index = bitrate_index(config_.bitrate_kbps);
    if (!index) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    if (config_.dialnorm < 1 || config_.dialnorm > 31) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }
    const int nfchans = fullbw_channel_count(config_.acmod);
    const int nchans = channel_count();
    assert(static_cast<int>(channels.size()) == nchans);
    assert(config_.acmod != Acmod::kDualMono);  // 1+1 second-program bsi unsupported
    assert(config_.chbwcod >= 0 && config_.chbwcod <= 60);
    for (const auto& channel : channels) {
        assert(channel.size() == kSamplesPerFrame);
        (void)channel;
    }

    const int endmant = ((config_.chbwcod + 12) * 3) + 37;

    // --- Frame size via the CBR accumulator (exact at every sample rate) ---
    const std::uint64_t ideal_bits_num =
        static_cast<std::uint64_t>(config_.bitrate_kbps) * 1000 * kSamplesPerFrame;
    const std::uint64_t denom =
        static_cast<std::uint64_t>(sample_rate_hz(config_.sample_rate)) * 16;
    rate_accumulator_ += ideal_bits_num;
    const std::uint64_t words64 = rate_accumulator_ / denom - words_emitted_;
    words_emitted_ += words64;
    const auto words = static_cast<std::uint32_t>(words64);
    const std::uint32_t base_words =
        *frame_size_words(config_.sample_rate, config_.bitrate_kbps, false);
    assert(words == base_words || words == base_words + 1);
    const bool pad = words != base_words;
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;
    const std::uint32_t words58 = frame_size_58_words(words);
    const BitAllocCodes codes{};  // §8.2.12 basic-encoder defaults

    // --- 1. Analysis: MDCT, fixed-point conversion, exponent extraction ---
    const auto channel_endmant = [&](int ch) { return ch < nfchans ? endmant : kLfeEndmant; };
    std::vector<std::int32_t> fixed;
    fixed.reserve(static_cast<std::size_t>(nchans) * kBlocksPerFrame *
                  static_cast<std::size_t>(endmant));
    std::vector<std::size_t> fixed_base(static_cast<std::size_t>(nchans) * kBlocksPerFrame);
    std::vector<std::vector<std::uint8_t>> raw_exps(static_cast<std::size_t>(nchans));
    std::vector<EncodedExponents> encoded_exps(static_cast<std::size_t>(nchans));
    std::vector<std::vector<std::uint8_t>> decoded_exps(static_cast<std::size_t>(nchans));

    for (int ch = 0; ch < nchans; ++ch) {
        const int ch_end = channel_endmant(ch);
        raw_exps[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(ch_end),
                                                      kMaxExponent);
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            std::array<double, 512> time{};
            for (int n = 0; n < 512; ++n) {
                const int pos = block * 256 - 256 + n;
                time[static_cast<std::size_t>(n)] =
                    pos < 0 ? history_[static_cast<std::size_t>(ch)]
                                      [static_cast<std::size_t>(pos + 256)]
                            : static_cast<double>(
                                  channels[static_cast<std::size_t>(ch)]
                                          [static_cast<std::size_t>(pos)]);
            }
            std::array<double, 512> windowed{};
            std::array<double, 256> coeffs{};
            apply_analysis_window(time, windowed);
            mdct512_forward(windowed, coeffs);
            fixed_base[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                       static_cast<std::size_t>(block)] = fixed.size();
            for (int bin = 0; bin < ch_end; ++bin) {
                const std::int32_t f = to_fixed25(coeffs[static_cast<std::size_t>(bin)]);
                fixed.push_back(f);
                auto& raw = raw_exps[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                raw = std::min(raw, static_cast<std::uint8_t>(exponent_from_fixed(f)));
            }
        }
        encoded_exps[static_cast<std::size_t>(ch)] =
            encode_exponents(raw_exps[static_cast<std::size_t>(ch)], ExpStrategy::kD15);
        decoded_exps[static_cast<std::size_t>(ch)].resize(static_cast<std::size_t>(ch_end));
        decode_exponents(encoded_exps[static_cast<std::size_t>(ch)].absolute,
                         encoded_exps[static_cast<std::size_t>(ch)].groups, ExpStrategy::kD15,
                         decoded_exps[static_cast<std::size_t>(ch)]);
        for (int n = 0; n < 256; ++n) {
            history_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] =
                static_cast<double>(
                    channels[static_cast<std::size_t>(ch)][static_cast<std::size_t>(1280 + n)]);
        }
    }
    const auto fixed_at = [&](int ch, int block, int bin) {
        return fixed[fixed_base[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                                static_cast<std::size_t>(block)] +
                     static_cast<std::size_t>(bin)];
    };

    // --- 2. SNR-offset search over all channels (LFE shares the offsets) ---
    const int groups = exponent_group_count(ExpStrategy::kD15, endmant);
    const std::uint32_t side_bits = side_info_bits(config_.acmod, config_.lfe, groups);
    assert(side_bits + detail::kTailBits <= total_bits);
    const std::uint32_t budget = total_bits - side_bits - detail::kTailBits;

    std::vector<std::vector<std::uint8_t>> bap(static_cast<std::size_t>(nchans));
    for (int ch = 0; ch < nchans; ++ch) {
        bap[static_cast<std::size_t>(ch)].resize(
            static_cast<std::size_t>(channel_endmant(ch)));
    }
    std::vector<std::span<const std::uint8_t>> bap_views(static_cast<std::size_t>(nchans));
    const auto bits_at = [&](int composite) {
        for (int ch = 0; ch < nchans; ++ch) {
            compute_bit_allocation(decoded_exps[static_cast<std::size_t>(ch)],
                                   config_.sample_rate, codes, composite >> 4, composite & 15,
                                   bap[static_cast<std::size_t>(ch)]);
            bap_views[static_cast<std::size_t>(ch)] = bap[static_cast<std::size_t>(ch)];
        }
        return static_cast<std::uint32_t>(kBlocksPerFrame * mantissa_bits_per_block(bap_views));
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
    const std::uint32_t mantissa_bits = bits_at(lo);
    assert(mantissa_bits <= budget);

    // --- 3. Mantissa tokens per block (grouping shared across channels) ---
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> block_tokens;
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        MantissaBlockWriter writer;
        for (int ch = 0; ch < nchans; ++ch) {
            for (int bin = 0; bin < channel_endmant(ch); ++bin) {
                const int exp =
                    decoded_exps[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(fixed_at(ch, block, bin)) << exp);
                writer.add(mantissa,
                           bap[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)]);
            }
        }
        writer.finish_block();
        assert(writer.bit_count() == mantissa_bits / kBlocksPerFrame);
        block_tokens[static_cast<std::size_t>(block)] = writer.tokens();
    }

    // --- 4. Padding plan, 5. packing ---
    const auto plan = detail::plan_padding(budget - mantissa_bits);

    BitWriter w;
    w.put(kSyncWord, 16);
    w.put(0, 16);  // crc1, patched below
    w.put(static_cast<std::uint32_t>(config_.sample_rate), 2);
    w.put(static_cast<std::uint32_t>(*index) * 2 + (pad ? 1u : 0u), 6);

    w.put(8, 5);  // bsid
    w.put(0, 3);  // bsmod
    w.put(static_cast<std::uint32_t>(config_.acmod), 3);
    if (has_three_front(config_.acmod)) {
        w.put(kCmixlev, 2);
    }
    if (has_surround(config_.acmod)) {
        w.put(kSurmixlev, 2);
    }
    if (config_.acmod == Acmod::k2_0) {
        w.put(0, 2);  // dsurmod: not indicated
    }
    w.put(config_.lfe ? 1 : 0, 1);
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
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(0, 1);  // blksw
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(0, 1);  // dithflag
        }
        w.put(0, 1);              // dynrnge
        w.put(first ? 1 : 0, 1);  // cplstre
        if (first) {
            w.put(0, 1);  // cplinu
        }
        if (config_.acmod == Acmod::k2_0) {
            w.put(first ? 1 : 0, 1);  // rematstr
            if (first) {
                for (int band = 0; band < 4; ++band) {
                    w.put(0, 1);  // rematflg
                }
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(static_cast<std::uint32_t>(first ? ExpStrategy::kD15 : ExpStrategy::kReuse), 2);
        }
        if (config_.lfe) {
            w.put(first ? 1 : 0, 1);  // lfeexpstr: D15 in block 0, reuse after
        }
        if (first) {
            for (int ch = 0; ch < nfchans; ++ch) {
                w.put(static_cast<std::uint32_t>(config_.chbwcod), 6);
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto& e = encoded_exps[static_cast<std::size_t>(ch)];
                w.put(e.absolute, 4);
                for (const auto group : e.groups) {
                    w.put(group, 7);
                }
                w.put(0, 2);  // gainrng
            }
            if (config_.lfe) {
                const auto& e = encoded_exps[static_cast<std::size_t>(nfchans)];
                w.put(e.absolute, 4);
                assert(e.groups.size() == 2);  // nlfegrps
                for (const auto group : e.groups) {
                    w.put(group, 7);
                }
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
            for (int ch = 0; ch < nfchans; ++ch) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);
            }
            if (config_.lfe) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);  // lfefsnroffst
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);  // lfefgaincod
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
