#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <cassert>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"

namespace ac3 {

namespace {

constexpr int kLfeEndmant = 7;

// Read-side §7.3.5 grouping state: the codeword arrives at the position of
// the group's first member; later members consume nothing. State is shared
// across channels within a block and discarded at block end (the dummies).
struct GroupReadState {
    struct Cache {
        int remaining = 0;
        std::array<std::uint32_t, 2> values{};
    };
    Cache bap1;
    Cache bap2;
    Cache bap4;

    std::uint32_t read_code(BitReader& reader, int bap) {
        const auto grouped = [&](Cache& cache, int bits, std::uint32_t radix,
                                 int members) -> std::uint32_t {
            if (cache.remaining == 0) {
                std::uint32_t group = reader.read(bits);
                if (members == 3) {
                    const std::uint32_t a = group / (radix * radix);
                    const std::uint32_t b = (group % (radix * radix)) / radix;
                    const std::uint32_t c = group % radix;
                    cache.values = {b, c};
                    cache.remaining = 2;
                    return a;
                }
                const std::uint32_t a = group / 11;
                cache.values = {group % 11, 0};
                cache.remaining = 1;
                return a;
            }
            const std::uint32_t next =
                cache.values[static_cast<std::size_t>(members == 3 ? 2 - cache.remaining
                                                                   : 1 - cache.remaining)];
            --cache.remaining;
            return next;
        };
        switch (bap) {
            case 1: return grouped(bap1, 5, 3, 3);
            case 2: return grouped(bap2, 7, 5, 3);
            case 4: return grouped(bap4, 7, 11, 2);
            default: return reader.read(kBapBits[static_cast<std::size_t>(bap)]);
        }
    }
};

}  // namespace

std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_frames(
    std::span<const std::byte> stream) {
    std::vector<std::span<const std::byte>> frames;
    std::size_t offset = 0;
    while (offset + 5 <= stream.size()) {
        if (std::to_integer<std::uint8_t>(stream[offset]) != 0x0B ||
            std::to_integer<std::uint8_t>(stream[offset + 1]) != 0x77) {
            return std::unexpected(DecodeError::kBadSyncWord);
        }
        const auto byte4 = std::to_integer<std::uint32_t>(stream[offset + 4]);
        const auto fscod = byte4 >> 6;
        const auto frmsizecod = byte4 & 0x3F;
        if (fscod == 3 || frmsizecod > 37) {
            return std::unexpected(DecodeError::kReservedValue);
        }
        const auto sr = static_cast<SampleRate>(fscod);
        const std::uint32_t kbps = kBitratesKbps[frmsizecod >> 1];
        const auto bytes = frame_size_bytes(sr, kbps, (frmsizecod & 1) != 0).value();
        if (offset + bytes > stream.size()) {
            return std::unexpected(DecodeError::kTruncated);
        }
        frames.push_back(stream.subspan(offset, bytes));
        offset += bytes;
    }
    if (offset != stream.size()) {
        return std::unexpected(DecodeError::kTruncated);
    }
    return frames;
}

std::expected<DecodedFrame, DecodeError> FrameDecoder::decode_frame(
    std::span<const std::byte> frame) {
    if (frame.size() < 6) {
        return std::unexpected(DecodeError::kTruncated);
    }
    BitReader r{frame};
    if (r.read(16) != kSyncWord) {
        return std::unexpected(DecodeError::kBadSyncWord);
    }
    (void)r.read(16);  // crc1 (validated by register property below)
    const auto fscod = r.read(2);
    const auto frmsizecod = r.read(6);
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    const auto sample_rate = static_cast<SampleRate>(fscod);
    const std::uint32_t kbps = kBitratesKbps[frmsizecod >> 1];
    const auto expected_bytes = frame_size_bytes(sample_rate, kbps, (frmsizecod & 1) != 0);
    if (!expected_bytes || frame.size() != *expected_bytes) {
        return std::unexpected(DecodeError::kTruncated);
    }
    const std::uint32_t words = *expected_bytes / 2;
    const std::uint32_t words58 = frame_size_58_words(words);
    if (crc16(frame.subspan(2, 2 * words58 - 2)) != 0 || crc16(frame.subspan(2)) != 0) {
        return std::unexpected(DecodeError::kBadCrc);
    }

    // --- bsi ---
    const auto bsid = r.read(5);
    if (bsid > 8) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    (void)r.read(3);  // bsmod
    const auto acmod_value = r.read(3);
    const auto acmod = static_cast<Acmod>(acmod_value);
    if (acmod == Acmod::kDualMono) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    if ((acmod_value & 0x1) != 0 && acmod != Acmod::k1_0) {
        (void)r.read(2);  // cmixlev
    }
    if ((acmod_value & 0x4) != 0) {
        (void)r.read(2);  // surmixlev
    }
    if (acmod == Acmod::k2_0) {
        (void)r.read(2);  // dsurmod
    }
    const bool lfe = r.read(1) != 0;
    const auto dialnorm = static_cast<int>(r.read(5));
    if (r.read(1) != 0) r.skip(8);   // compre/compr
    if (r.read(1) != 0) r.skip(8);   // langcode/langcod
    if (r.read(1) != 0) r.skip(7);   // audprodie: mixlevel + roomtyp
    (void)r.read(1);                 // copyrightb
    (void)r.read(1);                 // origbs
    if (r.read(1) != 0) r.skip(14);  // timecod1
    if (r.read(1) != 0) r.skip(14);  // timecod2
    if (r.read(1) != 0) {            // addbsie
        const auto addbsil = r.read(6);
        r.skip((addbsil + 1) * 8);
    }

    const int nfchans = fullbw_channel_count(acmod);
    const int nchans = nfchans + (lfe ? 1 : 0);

    DecodedFrame out;
    out.sample_rate = sample_rate;
    out.bitrate_kbps = kbps;
    out.acmod = acmod;
    out.lfe = lfe;
    out.dialnorm = dialnorm;
    out.channels.assign(static_cast<std::size_t>(nchans),
                        std::vector<float>(kSamplesPerFrame, 0.0f));

    // Per-channel decode state persisting across blocks.
    std::vector<int> endmant(static_cast<std::size_t>(nchans), kLfeEndmant);
    std::vector<std::vector<std::uint8_t>> exps(static_cast<std::size_t>(nchans));
    BitAllocCodes base_codes{};
    std::vector<int> fgaincod(static_cast<std::size_t>(nchans), base_codes.fgaincod);
    int csnroffst = 0;
    std::vector<int> fsnroffst(static_cast<std::size_t>(nchans), 0);
    std::array<bool, 4> rematflg{};

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {  // blksw
                return std::unexpected(DecodeError::kUnsupported);
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            (void)r.read(1);  // dithflag: bap-0 bins reconstruct as zero either way
        }
        if (r.read(1) != 0) r.skip(8);  // dynrnge/dynrng: parsed, not applied
        if (r.read(1) != 0) {           // cplstre
            if (r.read(1) != 0) {       // cplinu
                return std::unexpected(DecodeError::kUnsupported);
            }
        }
        if (acmod == Acmod::k2_0) {
            if (r.read(1) != 0) {  // rematstr: new flags; else prior flags persist
                for (auto& flag : rematflg) {
                    flag = r.read(1) != 0;
                }
            }
        }

        std::vector<ExpStrategy> strategy(static_cast<std::size_t>(nchans), ExpStrategy::kReuse);
        for (int ch = 0; ch < nfchans; ++ch) {
            strategy[static_cast<std::size_t>(ch)] = static_cast<ExpStrategy>(r.read(2));
            if (block == 0 && strategy[static_cast<std::size_t>(ch)] == ExpStrategy::kReuse) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
        }
        if (lfe) {
            strategy[static_cast<std::size_t>(nfchans)] =
                r.read(1) != 0 ? ExpStrategy::kD15 : ExpStrategy::kReuse;
            if (block == 0 && strategy[static_cast<std::size_t>(nfchans)] == ExpStrategy::kReuse) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            if (strategy[static_cast<std::size_t>(ch)] != ExpStrategy::kReuse) {
                const auto chbwcod = r.read(6);
                if (chbwcod > 60) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                endmant[static_cast<std::size_t>(ch)] =
                    ((static_cast<int>(chbwcod) + 12) * 3) + 37;
            }
        }
        for (int ch = 0; ch < nchans; ++ch) {
            const auto strat = strategy[static_cast<std::size_t>(ch)];
            if (strat == ExpStrategy::kReuse) {
                continue;
            }
            const int end = endmant[static_cast<std::size_t>(ch)];
            const int ngrps = ch < nfchans ? exponent_group_count(strat, end) : 2;
            const auto absolute = static_cast<std::uint8_t>(r.read(4));
            std::vector<std::uint8_t> groups(static_cast<std::size_t>(ngrps));
            for (auto& g : groups) {
                g = static_cast<std::uint8_t>(r.read(7));
            }
            exps[static_cast<std::size_t>(ch)].resize(static_cast<std::size_t>(end));
            decode_exponents(absolute, groups, strat, exps[static_cast<std::size_t>(ch)]);
            if (ch < nfchans) {
                (void)r.read(2);  // gainrng
            }
        }

        if (r.read(1) != 0) {  // baie
            base_codes.sdcycod = static_cast<int>(r.read(2));
            base_codes.fdcycod = static_cast<int>(r.read(2));
            base_codes.sgaincod = static_cast<int>(r.read(2));
            base_codes.dbpbcod = static_cast<int>(r.read(2));
            base_codes.floorcod = static_cast<int>(r.read(3));
        } else if (block == 0) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        if (r.read(1) != 0) {  // snroffste
            csnroffst = static_cast<int>(r.read(6));
            for (int ch = 0; ch < nchans; ++ch) {
                fsnroffst[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(4));
                fgaincod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(3));
            }
        } else if (block == 0) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        if (r.read(1) != 0) {  // deltbaie
            return std::unexpected(DecodeError::kUnsupported);
        }
        if (r.read(1) != 0) {  // skiple
            const auto skipl = r.read(9);
            r.skip(skipl * 8);
        }

        // Bit allocation (recomputed every block: parameters are stored
        // state, so the result matches the decoder-update rule of §7.2.1).
        std::vector<std::vector<std::uint8_t>> bap(static_cast<std::size_t>(nchans));
        for (int ch = 0; ch < nchans; ++ch) {
            const int end = endmant[static_cast<std::size_t>(ch)];
            if (static_cast<int>(exps[static_cast<std::size_t>(ch)].size()) != end) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            BitAllocCodes codes = base_codes;
            codes.fgaincod = fgaincod[static_cast<std::size_t>(ch)];
            bap[static_cast<std::size_t>(ch)].resize(static_cast<std::size_t>(end));
            compute_bit_allocation(exps[static_cast<std::size_t>(ch)], sample_rate, codes,
                                   csnroffst, fsnroffst[static_cast<std::size_t>(ch)],
                                   bap[static_cast<std::size_t>(ch)]);
        }

        // Mantissas -> coefficients (all channels first: rematrix undo needs
        // both), then rematrix undo, inverse transform, overlap-add.
        GroupReadState groups_state;
        std::vector<std::array<double, 256>> coeffs(static_cast<std::size_t>(nchans));
        for (int ch = 0; ch < nchans; ++ch) {
            const int end = endmant[static_cast<std::size_t>(ch)];
            for (int bin = 0; bin < end; ++bin) {
                const int bap_value =
                    bap[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    continue;  // silence (dither substitution not implemented)
                }
                const auto code = groups_state.read_code(r, bap_value);
                const int exp =
                    exps[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                coeffs[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa(code, bap_value) / static_cast<double>(1u << exp);
            }
        }
        if (acmod == Acmod::k2_0) {
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, applied up
            // to the lower bandwidth of the two channels.
            static constexpr std::array<std::array<int, 2>, 4> kBands = {{
                {13, 24}, {25, 36}, {37, 60}, {61, 252},
            }};
            const int cap = std::min(endmant[0], endmant[1]) - 1;
            for (std::size_t band = 0; band < kBands.size(); ++band) {
                if (!rematflg[band]) {
                    continue;
                }
                const int high = std::min(kBands[band][1], cap);
                for (int bin = kBands[band][0]; bin <= high; ++bin) {
                    const double l = coeffs[0][static_cast<std::size_t>(bin)];
                    const double rr = coeffs[1][static_cast<std::size_t>(bin)];
                    coeffs[0][static_cast<std::size_t>(bin)] = l + rr;
                    coeffs[1][static_cast<std::size_t>(bin)] = l - rr;
                }
            }
        }
        for (int ch = 0; ch < nchans; ++ch) {
            std::array<double, 512> x{};
            imdct512_windowed(coeffs[static_cast<std::size_t>(ch)], x);
            auto& delay = delay_[static_cast<std::size_t>(ch)];
            auto& pcm = out.channels[static_cast<std::size_t>(ch)];
            for (int n = 0; n < 256; ++n) {
                pcm[static_cast<std::size_t>(block * 256 + n)] = static_cast<float>(
                    2.0 * (x[static_cast<std::size_t>(n)] + delay[static_cast<std::size_t>(n)]));
                delay[static_cast<std::size_t>(n)] = x[static_cast<std::size_t>(256 + n)];
            }
        }
        if (r.overflowed()) {
            return std::unexpected(DecodeError::kTruncated);
        }
    }
    return out;
}

}  // namespace ac3
