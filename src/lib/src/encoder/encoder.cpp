#include "ac3/encoder/encoder.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

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

// Rematrixing bands, coupling not in use (Table 7.25): [low, high] inclusive.
constexpr std::array<std::array<int, 2>, 4> kRematrixBands = {{
    {13, 24}, {25, 36}, {37, 60}, {61, 252},
}};

constexpr bool has_three_front(Acmod acmod) {
    const auto value = static_cast<std::uint8_t>(acmod);
    return (value & 0x1) != 0 && acmod != Acmod::k1_0;
}

constexpr bool has_surround(Acmod acmod) {
    return (static_cast<std::uint8_t>(acmod) & 0x4) != 0;
}

// §8.2.8: strategy by the number of blocks an exponent set serves.
constexpr ExpStrategy strategy_for_span(int span) {
    if (span <= 1) {
        return ExpStrategy::kD45;
    }
    if (span <= 3) {
        return ExpStrategy::kD25;
    }
    return ExpStrategy::kD15;
}

// Exponent-set change detection (§8.2.8: "when the variation exceeds a
// threshold, new exponents will be sent"). Sum of absolute exponent
// differences against the set currently in force, scaled per bin.
bool needs_new_exponents(std::span<const std::uint8_t> current,
                         std::span<const std::uint8_t> reference) {
    long long diff = 0;
    for (std::size_t i = 0; i < current.size(); ++i) {
        diff += std::abs(static_cast<int>(current[i]) - static_cast<int>(reference[i]));
    }
    return diff > 2 * static_cast<long long>(current.size());
}

struct ChannelPlan {
    // For each block: the index of the exponent run it belongs to.
    std::array<int, kBlocksPerFrame> run_of_block{};
    // Per run: first block, strategy, encoded + decoded exponents.
    std::vector<int> run_start;
    std::vector<ExpStrategy> run_strategy;
    std::vector<EncodedExponents> run_encoded;
    std::vector<std::vector<std::uint8_t>> run_decoded;
};

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
    assert(config_.acmod != Acmod::kDualMono);
    for (const auto& channel : channels) {
        assert(channel.size() == kSamplesPerFrame);
        (void)channel;
    }

    // Bandwidth: explicit config, or a bitrate-aware default (roughly two
    // thirds of the per-channel kilobit rate, clamped to the legal range).
    int chbwcod = config_.chbwcod;
    if (chbwcod < 0) {
        const int per_channel_kbps =
            static_cast<int>(config_.bitrate_kbps) / std::max(nfchans, 1);
        chbwcod = std::clamp(per_channel_kbps * 2 / 3, 24, 60);
    }
    assert(chbwcod >= 0 && chbwcod <= 60);
    const int endmant = ((chbwcod + 12) * 3) + 37;

    // --- Frame size via the CBR accumulator ---
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

    // --- 1. MDCT per channel per block (double coefficients) ---
    const auto channel_endmant = [&](int ch) { return ch < nfchans ? endmant : kLfeEndmant; };
    std::vector<std::array<double, 256>> coeffs(
        static_cast<std::size_t>(nchans) * kBlocksPerFrame);
    const auto coeffs_at = [&](int ch, int block) -> std::array<double, 256>& {
        return coeffs[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                      static_cast<std::size_t>(block)];
    };
    for (int ch = 0; ch < nchans; ++ch) {
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
            apply_analysis_window(time, windowed);
            mdct512_forward(windowed, coeffs_at(ch, block));
        }
        for (int n = 0; n < 256; ++n) {
            history_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] =
                static_cast<double>(
                    channels[static_cast<std::size_t>(ch)][static_cast<std::size_t>(1280 + n)]);
        }
    }

    // --- 2. Rematrixing (2/0 only, §7.5.3): per block, per band, code the
    // half-sum/half-difference when their power is smaller. ---
    std::array<std::array<bool, 4>, kBlocksPerFrame> rematflg{};
    const bool rematrixing = config_.acmod == Acmod::k2_0;
    if (rematrixing) {
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            auto& left = coeffs_at(0, block);
            auto& right = coeffs_at(1, block);
            for (std::size_t band = 0; band < kRematrixBands.size(); ++band) {
                const int low = kRematrixBands[band][0];
                const int high = std::min(kRematrixBands[band][1], endmant - 1);
                double power_l = 0.0;
                double power_r = 0.0;
                double power_sum = 0.0;
                double power_diff = 0.0;
                for (int bin = low; bin <= high; ++bin) {
                    const double l = left[static_cast<std::size_t>(bin)];
                    const double r = right[static_cast<std::size_t>(bin)];
                    power_l += l * l;
                    power_r += r * r;
                    power_sum += (l + r) * (l + r);
                    power_diff += (l - r) * (l - r);
                }
                const double keep = std::min(power_l, power_r);
                const double matrix = std::min(power_sum, power_diff);
                if (matrix < keep) {
                    rematflg[static_cast<std::size_t>(block)][band] = true;
                    for (int bin = low; bin <= high; ++bin) {
                        const double l = left[static_cast<std::size_t>(bin)];
                        const double r = right[static_cast<std::size_t>(bin)];
                        left[static_cast<std::size_t>(bin)] = 0.5 * (l + r);
                        right[static_cast<std::size_t>(bin)] = 0.5 * (l - r);
                    }
                }
            }
        }
    }

    // --- 3. Fixed-point conversion + per-block raw exponents ---
    std::vector<std::int32_t> fixed;
    fixed.reserve(static_cast<std::size_t>(nchans) * kBlocksPerFrame *
                  static_cast<std::size_t>(endmant));
    std::vector<std::size_t> fixed_base(static_cast<std::size_t>(nchans) * kBlocksPerFrame);
    std::vector<std::vector<std::uint8_t>> block_exps(
        static_cast<std::size_t>(nchans) * kBlocksPerFrame);
    for (int ch = 0; ch < nchans; ++ch) {
        const int ch_end = channel_endmant(ch);
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            const auto slot = static_cast<std::size_t>(ch) * kBlocksPerFrame +
                              static_cast<std::size_t>(block);
            fixed_base[slot] = fixed.size();
            block_exps[slot].resize(static_cast<std::size_t>(ch_end));
            for (int bin = 0; bin < ch_end; ++bin) {
                const std::int32_t f =
                    to_fixed25(coeffs_at(ch, block)[static_cast<std::size_t>(bin)]);
                fixed.push_back(f);
                block_exps[slot][static_cast<std::size_t>(bin)] =
                    static_cast<std::uint8_t>(exponent_from_fixed(f));
            }
        }
    }
    const auto fixed_at = [&](int ch, int block, int bin) {
        return fixed[fixed_base[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                                static_cast<std::size_t>(block)] +
                     static_cast<std::size_t>(bin)];
    };

    // --- 4. Exponent strategy plan per channel (§8.2.8) ---
    std::vector<ChannelPlan> plan(static_cast<std::size_t>(nchans));
    for (int ch = 0; ch < nchans; ++ch) {
        auto& p = plan[static_cast<std::size_t>(ch)];
        const bool is_lfe = ch >= nfchans;
        // Decide run boundaries from raw exponent variation.
        std::vector<int> starts{0};
        if (!is_lfe) {  // the 7-bin LFE set: one D15 set per frame
            const auto* reference =
                &block_exps[static_cast<std::size_t>(ch) * kBlocksPerFrame];
            for (int block = 1; block < kBlocksPerFrame; ++block) {
                const auto& current = block_exps[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                                                 static_cast<std::size_t>(block)];
                if (needs_new_exponents(current, *reference)) {
                    starts.push_back(block);
                    reference = &current;
                }
            }
        }
        starts.push_back(kBlocksPerFrame);
        for (std::size_t run = 0; run + 1 < starts.size(); ++run) {
            const int begin = starts[run];
            const int end = starts[run + 1];
            const auto strategy =
                is_lfe ? ExpStrategy::kD15 : strategy_for_span(end - begin);
            // The run's exponents: minimum across its blocks, so reuse is safe.
            std::vector<std::uint8_t> raw(
                block_exps[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                           static_cast<std::size_t>(begin)]);
            for (int block = begin + 1; block < end; ++block) {
                const auto& other = block_exps[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                                               static_cast<std::size_t>(block)];
                for (std::size_t bin = 0; bin < raw.size(); ++bin) {
                    raw[bin] = std::min(raw[bin], other[bin]);
                }
            }
            p.run_start.push_back(begin);
            p.run_strategy.push_back(strategy);
            p.run_encoded.push_back(encode_exponents(raw, strategy));
            auto& decoded = p.run_decoded.emplace_back(raw.size());
            decode_exponents(p.run_encoded.back().absolute, p.run_encoded.back().groups,
                             strategy, decoded);
            for (int block = begin; block < end; ++block) {
                p.run_of_block[static_cast<std::size_t>(block)] = static_cast<int>(run);
            }
        }
    }

    // --- 5. Side-information bits for this frame's exact plan ---
    std::uint32_t side_bits = 16 + 16 + 2 + 6;  // syncinfo
    {
        std::uint32_t bsi = 25;
        if (has_three_front(config_.acmod)) bsi += 2;
        if (has_surround(config_.acmod)) bsi += 2;
        if (config_.acmod == Acmod::k2_0) bsi += 2;
        side_bits += bsi;
    }
    std::array<bool, kBlocksPerFrame> send_rematstr{};
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        std::uint32_t bits = 2 * static_cast<std::uint32_t>(nfchans) + 1;  // blksw+dith+dynrnge
        bits += block == 0 ? 2 : 1;  // cplstre (+cplinu in block 0)
        if (rematrixing) {
            send_rematstr[static_cast<std::size_t>(block)] =
                block == 0 || rematflg[static_cast<std::size_t>(block)] !=
                                  rematflg[static_cast<std::size_t>(block) - 1];
            bits += 1 + (send_rematstr[static_cast<std::size_t>(block)] ? 4 : 0);
        }
        bits += 2 * static_cast<std::uint32_t>(nfchans);  // chexpstr
        if (config_.lfe) bits += 1;                       // lfeexpstr
        for (int ch = 0; ch < nchans; ++ch) {
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const int run = p.run_of_block[static_cast<std::size_t>(block)];
            if (p.run_start[static_cast<std::size_t>(run)] != block) {
                continue;  // reuse: no exponent payload
            }
            if (ch < nfchans) {
                bits += 6;  // chbwcod
                bits += 4 + static_cast<std::uint32_t>(
                                p.run_encoded[static_cast<std::size_t>(run)].groups.size()) *
                                7 +
                        2;  // abs + groups + gainrng
            } else {
                bits += 4 + 2 * 7;  // LFE: abs + nlfegrps, no gainrng
            }
        }
        bits += 1 + (block == 0 ? 11u : 0u);  // baie
        bits += 1 + (block == 0 ? 6 + static_cast<std::uint32_t>(nfchans) * 7 +
                                      (config_.lfe ? 7u : 0u)
                                : 0u);        // snroffste
        bits += 1 + 1;                        // deltbaie + skiple
        side_bits += bits;
    }
    assert(side_bits + detail::kTailBits <= total_bits);
    const std::uint32_t budget = total_bits - side_bits - detail::kTailBits;

    // --- 6. SNR-offset search (bap varies per run) ---
    // bap cache: per channel, per run, for the current composite offset.
    std::vector<std::vector<std::vector<std::uint8_t>>> run_bap(
        static_cast<std::size_t>(nchans));
    for (int ch = 0; ch < nchans; ++ch) {
        run_bap[static_cast<std::size_t>(ch)].resize(
            plan[static_cast<std::size_t>(ch)].run_start.size());
    }
    std::vector<std::span<const std::uint8_t>> bap_views(static_cast<std::size_t>(nchans));
    const auto bits_at = [&](int composite) {
        for (int ch = 0; ch < nchans; ++ch) {
            auto& p = plan[static_cast<std::size_t>(ch)];
            for (std::size_t run = 0; run < p.run_start.size(); ++run) {
                auto& bap = run_bap[static_cast<std::size_t>(ch)][run];
                bap.resize(p.run_decoded[run].size());
                compute_bit_allocation(p.run_decoded[run], config_.sample_rate, codes,
                                       composite >> 4, composite & 15, bap);
            }
        }
        std::uint32_t total = 0;
        for (int block = 0; block < kBlocksPerFrame; ++block) {
            for (int ch = 0; ch < nchans; ++ch) {
                const auto& p = plan[static_cast<std::size_t>(ch)];
                bap_views[static_cast<std::size_t>(ch)] =
                    run_bap[static_cast<std::size_t>(ch)]
                           [static_cast<std::size_t>(
                               p.run_of_block[static_cast<std::size_t>(block)])];
            }
            total += static_cast<std::uint32_t>(mantissa_bits_per_block(bap_views));
        }
        return total;
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

    // --- 7. Mantissa tokens per block ---
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> block_tokens;
    std::size_t token_bits_total = 0;
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        MantissaBlockWriter writer;
        for (int ch = 0; ch < nchans; ++ch) {
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const auto run = static_cast<std::size_t>(
                p.run_of_block[static_cast<std::size_t>(block)]);
            const auto& exps = p.run_decoded[run];
            const auto& bap = run_bap[static_cast<std::size_t>(ch)][run];
            for (int bin = 0; bin < channel_endmant(ch); ++bin) {
                const int exp = exps[static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(fixed_at(ch, block, bin)) << exp);
                writer.add(mantissa, bap[static_cast<std::size_t>(bin)]);
            }
        }
        writer.finish_block();
        token_bits_total += writer.bit_count();
        block_tokens[static_cast<std::size_t>(block)] = writer.tokens();
    }
    assert(token_bits_total == mantissa_bits);

    // --- 8. Padding plan, 9. packing ---
    const auto plan_pad = detail::plan_padding(budget - mantissa_bits);

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
        w.put(0, 2);  // dsurmod
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
        if (rematrixing) {
            const bool send = send_rematstr[static_cast<std::size_t>(block)];
            w.put(send ? 1 : 0, 1);  // rematstr
            if (send) {
                for (std::size_t band = 0; band < 4; ++band) {
                    w.put(rematflg[static_cast<std::size_t>(block)][band] ? 1 : 0, 1);
                }
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const int run = p.run_of_block[static_cast<std::size_t>(block)];
            const bool fresh = p.run_start[static_cast<std::size_t>(run)] == block;
            w.put(static_cast<std::uint32_t>(
                      fresh ? p.run_strategy[static_cast<std::size_t>(run)]
                            : ExpStrategy::kReuse),
                  2);
        }
        if (config_.lfe) {
            const auto& p = plan[static_cast<std::size_t>(nfchans)];
            const int run = p.run_of_block[static_cast<std::size_t>(block)];
            w.put(p.run_start[static_cast<std::size_t>(run)] == block ? 1 : 0, 1);
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const int run = p.run_of_block[static_cast<std::size_t>(block)];
            if (p.run_start[static_cast<std::size_t>(run)] == block) {
                w.put(static_cast<std::uint32_t>(chbwcod), 6);
            }
        }
        for (int ch = 0; ch < nchans; ++ch) {
            const auto& p = plan[static_cast<std::size_t>(ch)];
            const auto run = static_cast<std::size_t>(
                p.run_of_block[static_cast<std::size_t>(block)]);
            if (p.run_start[run] != block) {
                continue;
            }
            const auto& e = p.run_encoded[run];
            w.put(e.absolute, 4);
            for (const auto group : e.groups) {
                w.put(group, 7);
            }
            if (ch < nfchans) {
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
            for (int ch = 0; ch < nchans; ++ch) {
                w.put(static_cast<std::uint32_t>(fsnroffst), 4);
                w.put(static_cast<std::uint32_t>(codes.fgaincod), 3);
            }
        }
        w.put(0, 1);  // deltbaie

        const std::uint16_t skip = plan_pad.skip_bytes[static_cast<std::size_t>(block)];
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

    assert(w.bit_count() + plan_pad.aux_bits + detail::kTailBits == total_bits);
    for (std::uint32_t i = 0; i < plan_pad.aux_bits; ++i) {
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
