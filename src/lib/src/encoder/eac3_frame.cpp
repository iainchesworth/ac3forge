#include "ac3/encoder/eac3_frame.hpp"

#include <algorithm>
#include <cassert>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/window.hpp"

namespace ac3::eac3 {

namespace {

// Frame-level strategy flags for this minimal profile. Every advanced tool
// is switched off; what remains is the AC-3 coding path inside an E-AC-3
// container.
//
// expstre and snroffststr each have a frame-level and a per-block form. The
// frame-level ones are chosen deliberately: they are what real encoders emit,
// so they are the paths reference decoders are actually exercised on. They
// are also strictly smaller - the whole frame's exponent strategy collapses
// to one code per channel. Table E2.10 code 0 is exactly "D15 in block 0,
// reuse for the rest", which is the strategy this profile wanted anyway.
constexpr int kExpstre = 0;
constexpr int kFrmExpStrategyCode = 0;  // Table E2.10 row 0: D15 R R R R R
constexpr int kAhte = 0;           // no adaptive hybrid transform
constexpr int kSnroffststr = 0;    // one SNR offset pair for the whole frame
constexpr int kTransproce = 0;     // no transient pre-noise processing
constexpr int kBlkswe = 0;         // long blocks only, so no per-block flags
constexpr int kDithflage = 1;      // sent explicitly: the DEFAULT when absent is
                                   // dither ON, which would fill every zero-bit
                                   // bin with noise and make "silence" audible
constexpr int kBamode = 0;         // default allocation parameters, zero bits
// Table E1.4, the else-branch of if(bamode): with bamode == 0 the allocation
// parameters take THESE values. They are not the §8.2.12 basic-encoder
// recommendations that AC-3 uses - floorcod is 0x7 here against §8.2.12's 4,
// and BitAllocCodes defaults to the latter. floorcod sets the masking floor,
// so the discrepancy changes every bap and hence the whole mantissa bit
// count: the encoder sized the frame for one allocation while the decoder
// read it with another, and every block after the first landed at the wrong
// offset. Digital silence cannot catch this, because zero SNR offsets make
// §7.2.2.1.1 zero the allocation before floorcod is ever consulted.
constexpr BitAllocCodes kBamode0Codes{.sdcycod = 2,
                                      .fdcycod = 1,
                                      .sgaincod = 1,
                                      .dbpbcod = 2,
                                      .floorcod = 7,
                                      .fgaincod = 4};  // frmfgaincode == 0 (§8.2.12)
constexpr int kFrmfgaincode = 0;   // fgaincod defaults to 0x4, matching AC-3
constexpr int kDbaflde = 0;        // no delta bit allocation
// Padding goes through auxbits. AC-3 cannot do that - §5.5 confines its aux
// field to the final 3/8 of the frame, to protect the crc1-at-5/8 checkpoint -
// but E-AC-3 has no crc1 and Annex E states no equivalent constraint, so
// auxbits absorb the whole remainder. FFmpeg's own encoder likewise sets
// skipflde to 0 when it has nothing to carry.
//
// Metadata is a different matter. The skip field exists in EVERY block
// (§2.3.2.10: "full skip field syntax shall be present in each audio block"),
// so switching it on costs one bit per block whether or not anything is
// carried, and the frame-level flag has to be decided before the blocks are
// written.
constexpr int kSpxattene = 0;      // no spectral extension attenuation

constexpr int kTailBits = 18;  // auxdatae + crcrsv + crc2

// The skip field is 9 bits of length, so one block can hold this much.
constexpr std::size_t kMaxSkipBytes = 511;

// §5.4.3.58-60, at the position Annex E's audblk gives it: after the delta
// bit allocation fields and before the quantized mantissas. Getting that
// order wrong does not fail to parse - it shifts every mantissa in the block,
// which comes back as noise rather than as an error.
void put_skip_field(BitWriter& w, std::span<const std::byte> payload) {
    if (payload.empty()) {
        w.put(0, 1);  // skiple: this block carries nothing
        return;
    }
    w.put(1, 1);  // skiple
    w.put(static_cast<std::uint32_t>(payload.size()), 9);  // skipl, in bytes
    for (const auto byte : payload) {
        w.put(std::to_integer<std::uint32_t>(byte), 8);
    }
}

// Bits a payload costs the frame: skipl and the bytes themselves, on top of
// the one skiple bit every block pays once skipflde is set.
[[nodiscard]] std::uint32_t skip_field_bits(std::span<const std::byte> payload) {
    if (payload.empty()) {
        return 0;
    }
    return 9 + static_cast<std::uint32_t>(payload.size()) * 8;
}

// One coded channel: its exponents (frame-constant, D15 in block 0) and the
// allocation they produce. LFE, when present, is the last entry.
struct ChannelPlan {
    int endmant = 0;
    EncodedExponents coded;
    std::vector<std::uint8_t> decoded;  // decoder-mirror exponents
    std::vector<std::uint8_t> bap;
};

struct Payload {
    int csnroffst = 0;
    int fsnroffst = 0;
    std::vector<ChannelPlan> chans;
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> mantissas;
};

// Everything from the sync word to the end of the last block: the whole
// frame bar padding and the tail. Silence and real audio go through this one
// function, so the two can never drift apart on field placement.
void emit_frame(BitWriter& w, const FrameConfig& config, std::uint32_t words,
                const Payload& payload, std::span<const std::byte> metadata = {}) {
    const int nfchans = fullbw_channel_count(config.acmod);
    const bool dependent = config.strmtyp == StreamType::kDependent;
    const int skipflde = metadata.empty() ? 0 : 1;

    w.put(kSyncWord, 16);

    // --- bsi (Table E1.2) ---
    w.put(static_cast<std::uint32_t>(config.strmtyp), 2);
    w.put(static_cast<std::uint32_t>(config.substreamid), 3);
    w.put(words - 1, 11);  // frmsiz is words - 1
    w.put(static_cast<std::uint32_t>(config.sample_rate), 2);  // fscod (not 0x3)
    w.put(3, 2);  // numblkscod: six blocks per syncframe
    w.put(static_cast<std::uint32_t>(config.acmod), 3);
    w.put(config.lfe ? 1 : 0, 1);
    w.put(kBsid, 5);
    w.put(static_cast<std::uint32_t>(config.dialnorm), 5);
    // §E3.8.5: in a dependent substream compre is not really "a compression
    // word follows" - it marks the LAST dependent of the program, which is how
    // a decoder knows every channel has arrived. The last one must set it and
    // the others must clear it. The compr word it drags in is 0x00, which
    // §7.7.1 defines as unity gain and directs an encoder applying no
    // compression to send.
    const bool compre = dependent && config.last_dependent;
    w.put(compre ? 1 : 0, 1);
    if (compre) {
        w.put(0, 8);  // compr: 0 dB
    }
    // acmod == 0x0 (1+1) is rejected in validate(), so the dialnorm2 /
    // compr2e block never applies.
    if (dependent) {
        w.put(config.chanmap ? 1 : 0, 1);  // chanmape
        if (config.chanmap) {
            w.put(*config.chanmap, 16);
        }
    }
    w.put(0, 1);  // mixmdate
    w.put(0, 1);  // infomdate
    // convsync is absent because numblkscod == 0x3; strmtyp != 0x2.
    if (config.oba_complexity_index) {
        // TS 103 420 §8.3.1 fixes the addbsi contents for an object-audio
        // stream: seven reserved bits, the extension flag, then the complexity
        // index. addbsil counts BYTES MINUS ONE, so the two bytes below are 1.
        w.put(1, 1);  // addbsie
        w.put(1, 6);  // addbsil
        w.put(0, 7);  // reserved
        w.put(1, 1);  // flag_ec3_extension_type_a
        w.put(static_cast<std::uint32_t>(*config.oba_complexity_index), 8);
    } else {
        w.put(0, 1);  // addbsie
    }

    // --- audfrm (Table E1.3) ---
    w.put(kExpstre, 1);
    w.put(kAhte, 1);
    w.put(kSnroffststr, 2);
    w.put(kTransproce, 1);
    w.put(kBlkswe, 1);
    w.put(kDithflage, 1);
    w.put(kBamode, 1);
    w.put(kFrmfgaincode, 1);
    w.put(kDbaflde, 1);
    w.put(static_cast<std::uint32_t>(skipflde), 1);
    w.put(kSpxattene, 1);

    if (static_cast<std::uint8_t>(config.acmod) > 0x1) {
        w.put(0, 1);  // cplinu[0]: coupling off (cplstre[0] is implied 1)
        for (int blk = 1; blk < kBlocksPerFrame; ++blk) {
            w.put(0, 1);  // cplstre[blk] = 0, so cplinu inherits 0
        }
    }
    // expstre == 0: one Table E2.10 code per channel covers all six blocks.
    // frmcplexpstr is absent because no block couples.
    for (int ch = 0; ch < nfchans; ++ch) {
        w.put(kFrmExpStrategyCode, 5);  // frmchexpstr[ch]
    }
    if (config.lfe) {
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            w.put(blk == 0 ? 1 : 0, 1);  // lfeexpstr
        }
    }
    // The whole converter-exponent element is gated on strmtyp == 0x0: only an
    // independent substream can be converted back to AC-3, so a dependent
    // sends none of it. For an independent substream numblkscod == 0x3 implies
    // convexpstre, and the strategies always follow; they describe how a
    // converter would code this frame, so mirroring the real strategy is the
    // honest value.
    if (!dependent) {
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(0, 5);  // convexpstr[ch]
        }
    }
    // snroffststr == 0: the SNR offsets live here, once for the frame, and
    // every channel inherits them. Zero for both means §7.2.2.1.1 gives an
    // all-zero allocation, hence no mantissa data at all.
    w.put(static_cast<std::uint32_t>(payload.csnroffst), 6);  // frmcsnroffst
    w.put(static_cast<std::uint32_t>(payload.fsnroffst), 4);  // frmfsnroffst
    // ahte == 0, transproce == 0, spxattene == 0 all contribute nothing - but
    // audfrm still ends with the block-start info flag whenever numblkscod
    // != 0. Omitting this one bit shifts every audio block along, which a
    // decoder reads as spectral extension being switched on.
    w.put(0, 1);  // blkstrtinfoe

    // --- audblk x6 (Table E1.4) ---
    for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
        const bool first = blk == 0;
        // blkswe == 0: blksw omitted, all long blocks.
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(0, 1);  // dithflag: off, so zero-bit bins stay silent
        }
        w.put(0, 1);  // dynrnge

        // Spectral extension: block 0 has spxstre implied, later blocks
        // send it explicitly.
        if (first) {
            w.put(0, 1);  // spxinu
        } else {
            w.put(0, 1);  // spxstre
        }
        // cplstre[0] is implied 1 with cplinu[0] == 0, which carries no
        // payload; blocks 1-5 had cplstre 0. Nothing to write.

        if (config.acmod == Acmod::k2_0) {
            // Unlike AC-3, block 0's rematstr is IMPLIED 1 rather than
            // transmitted - only later blocks carry the bit. Sending it
            // anyway shifts the rest of the block by one.
            if (!first) {
                w.put(0, 1);  // rematstr: keep the previous flags
            } else {
                for (int band = 0; band < 4; ++band) {
                    w.put(0, 1);  // rematflg
                }
            }
        }

        // chbwcod accompanies a fresh exponent strategy for uncoupled
        // channels; the strategies themselves already went out in audfrm.
        if (first) {
            for (int ch = 0; ch < nfchans; ++ch) {
                w.put(static_cast<std::uint32_t>(config.chbwcod), 6);
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto& coded = payload.chans[static_cast<std::size_t>(ch)].coded;
                w.put(coded.absolute, 4);
                for (const auto group : coded.groups) {
                    w.put(group, 7);
                }
                w.put(0, 2);  // gainrng
            }
            if (config.lfe) {
                const auto& coded = payload.chans.back().coded;
                w.put(coded.absolute, 4);
                assert(coded.groups.size() == 2);
                for (const auto group : coded.groups) {
                    w.put(group, 7);
                }
            }
        }

        // bamode == 0: the allocation parameters take their defaults.
        // snroffststr == 0: the offsets came from audfrm, so the block
        // carries no SNR fields whatsoever.
        // frmfgaincode == 0, so fgaincod defaults to 0x4 for every channel.
        if (!dependent) {
            w.put(0, 1);  // convsnroffste, gated on strmtyp == 0x0
        }
        // cplinu == 0: no coupling leak. dbaflde == 0: no delta allocation.
        if (skipflde != 0) {
            // The whole container rides in block 0. It could go in any block -
            // Dolby's own streams put it further in - but a decoder finds it by
            // scanning for the EMDF sync word, so the choice is the encoder's.
            put_skip_field(w, first ? metadata : std::span<const std::byte>{});
        }

        for (const auto& token : payload.mantissas[static_cast<std::size_t>(blk)]) {
            w.put(token.value, token.bits);
        }
    }
}

// Pad with auxbits, close the tail and patch crc2.
std::expected<std::vector<std::byte>, FrameError> finish_frame(
    const FrameConfig& config, std::uint32_t words, const Payload& payload,
    std::span<const std::byte> aux) {
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;

    // skipl is 9 bits, so one block cannot carry more than this.
    if (aux.size() > kMaxSkipBytes) {
        return std::unexpected(FrameError::kInvalidObjectAudio);
    }

    BitWriter probe;
    emit_frame(probe, config, words, payload, aux);
    const auto content_bits = static_cast<std::uint32_t>(probe.bit_count());
    if (content_bits + kTailBits > total_bits) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    const std::uint32_t spare = total_bits - content_bits - kTailBits;

    BitWriter w;
    emit_frame(w, config, words, payload, aux);
    for (std::uint32_t i = 0; i < spare; ++i) {
        w.put(0, 1);  // auxbits: padding, and nothing else
    }
    w.put(0, 1);   // auxdatae
    w.put(0, 1);   // crcrsv
    w.put(0, 16);  // crc2, patched below
    assert(w.bit_count() == total_bits);

    std::vector<std::byte> frame = w.take();
    // E-AC-3 has no crc1; crc2 covers everything after the sync word.
    const std::span<const std::byte> view{frame};
    std::uint16_t crc2 = crc16(view.subspan(2, total_bytes - 4));
    if (crc2 == kSyncWord) {
        frame[total_bytes - 3] ^= std::byte{0x01};  // crcrsv (§5.4.5.1)
        crc2 = crc16(view.subspan(2, total_bytes - 4));
    }
    frame[total_bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[total_bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
    return frame;
}

std::expected<void, FrameError> validate(const FrameConfig& config) {
    if (config.dialnorm < 1 || config.dialnorm > 31) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }
    if (!is_valid_bitrate(config.bitrate_kbps)) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    // 1+1 needs a second program's metadata throughout; out of scope here.
    if (config.acmod == Acmod::kDualMono) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    if (config.substreamid < 0 || config.substreamid > 7) {
        return std::unexpected(FrameError::kInvalidSubstream);
    }
    // TS 103 420 §8.3.2.2: complexity_index_type_a is the object count, and
    // "the maximum value of this field shall be 16".
    if (config.oba_complexity_index &&
        (*config.oba_complexity_index < 1 || *config.oba_complexity_index > 16)) {
        return std::unexpected(FrameError::kInvalidObjectAudio);
    }
    // Only a dependent substream carries a channel map, and §E2.3.1.8 requires
    // the locations it names to add up to exactly the channels acmod and lfeon
    // code. Disagreement is not a parse failure - the decoder simply puts
    // audio in the wrong speakers - so it has to be caught here.
    if (config.chanmap) {
        if (config.strmtyp != StreamType::kDependent) {
            return std::unexpected(FrameError::kInvalidSubstream);
        }
        const int coded = fullbw_channel_count(config.acmod) + (config.lfe ? 1 : 0);
        if (chanmap::channel_count(*config.chanmap) != coded) {
            return std::unexpected(FrameError::kInvalidChannelMap);
        }
    }
    return {};
}

}  // namespace

std::expected<std::vector<std::byte>, FrameError> build_silent_frame(
    const FrameConfig& config, AuxPayload aux) {
    if (const auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }

    const int nfchans = fullbw_channel_count(config.acmod);
    const int endmant = ((config.chbwcod + 12) * 3) + 37;

    // Exponents: an all-quiet ramp, so the decoder's own allocation returns
    // zero everywhere. Both offsets stay at zero, which §7.2.2.1.1 defines as
    // an all-zero allocation - no mantissas exist and the frame is pure
    // syntax.
    Payload payload;
    const std::vector<std::uint8_t> quiet(static_cast<std::size_t>(endmant), kMaxExponent);
    for (int ch = 0; ch < nfchans; ++ch) {
        payload.chans.push_back({.endmant = endmant,
                                 .coded = encode_exponents(quiet, ExpStrategy::kD15)});
    }
    if (config.lfe) {
        const std::vector<std::uint8_t> lfe_quiet(kLfeEndmant, kMaxExponent);
        payload.chans.push_back(
            {.endmant = kLfeEndmant,
             .coded = encode_exponents(lfe_quiet, ExpStrategy::kD15)});
    }

    return finish_frame(config, frame_words(config.sample_rate, config.bitrate_kbps),
                        payload, aux);
}

std::expected<std::vector<std::byte>, FrameError> FrameEncoder::encode_frame(
    std::span<const std::span<const float>> channels, AuxPayload aux) {
    if (const auto ok = validate(config_); !ok) {
        return std::unexpected(ok.error());
    }
    const int nfchans = fullbw_channel_count(config_.acmod);
    const int nchans = channel_count();
    assert(static_cast<int>(channels.size()) == nchans);
    for (const auto& channel : channels) {
        assert(channel.size() == kSamplesPerFrame);
        (void)channel;
    }

    const int fbw_endmant = ((config_.chbwcod + 12) * 3) + 37;
    const std::uint32_t words = frame_words(config_.sample_rate, config_.bitrate_kbps);
    const std::uint32_t total_bits = words * 16;

    // --- 1. MDCT, then the 25-bit fixed-point coefficients -----------------
    std::vector<std::array<std::int32_t, 256>> fixed(
        static_cast<std::size_t>(nchans) * kBlocksPerFrame);
    const auto fixed_at = [&](int ch, int blk) -> std::array<std::int32_t, 256>& {
        return fixed[static_cast<std::size_t>(ch) * kBlocksPerFrame +
                     static_cast<std::size_t>(blk)];
    };
    for (int ch = 0; ch < nchans; ++ch) {
        const auto& pcm = channels[static_cast<std::size_t>(ch)];
        auto& hist = history_[static_cast<std::size_t>(ch)];
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            std::array<double, 512> time{};
            for (int n = 0; n < 512; ++n) {
                const int pos = blk * 256 - 256 + n;
                time[static_cast<std::size_t>(n)] =
                    pos < 0 ? hist[static_cast<std::size_t>(pos + 256)]
                            : static_cast<double>(pcm[static_cast<std::size_t>(pos)]);
            }
            std::array<double, 512> windowed{};
            apply_analysis_window(time, windowed);
            std::array<double, 256> coeffs{};
            mdct512_forward(windowed, coeffs);
            auto& out = fixed_at(ch, blk);
            for (int bin = 0; bin < 256; ++bin) {
                out[static_cast<std::size_t>(bin)] =
                    to_fixed25(coeffs[static_cast<std::size_t>(bin)]);
            }
        }
        for (int n = 0; n < 256; ++n) {
            hist[static_cast<std::size_t>(n)] =
                static_cast<double>(pcm[static_cast<std::size_t>(1280 + n)]);
        }
    }

    // --- 2. One frame-constant exponent set per channel --------------------
    // Table E2.10 code 0 sends D15 in block 0 and reuses it for the other
    // five, so a bin's exponent has to accommodate its LOUDEST block. The
    // smallest exponent across the frame is that bin's worst case; anything
    // larger would overflow the mantissa in the block that peaks.
    Payload payload;
    payload.chans.resize(static_cast<std::size_t>(nchans));
    for (int ch = 0; ch < nchans; ++ch) {
        auto& plan = payload.chans[static_cast<std::size_t>(ch)];
        plan.endmant = ch < nfchans ? fbw_endmant : kLfeEndmant;
        const auto span = static_cast<std::size_t>(plan.endmant);

        std::vector<std::uint8_t> raw(span, kMaxExponent);
        std::vector<std::uint8_t> block_exps(span, 0);
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            extract_exponents(std::span{fixed_at(ch, blk)}.first(span), block_exps);
            for (std::size_t bin = 0; bin < span; ++bin) {
                raw[bin] = std::min(raw[bin], block_exps[bin]);
            }
        }
        plan.coded = encode_exponents(raw, ExpStrategy::kD15);
        plan.decoded.assign(span, 0);
        decode_exponents(plan.coded.absolute, plan.coded.groups, ExpStrategy::kD15,
                         plan.decoded);
        plan.bap.assign(span, 0);
    }

    // --- 3. SNR-offset search ----------------------------------------------
    // The side info is offset-independent here (bamode 0, no delta
    // allocation), so it can be measured once and the remainder handed
    // wholly to the mantissas.
    // The metadata competes with the mantissas for the same frame. It is
    // inside emit_frame's output now that it rides in a skip field, so the
    // side-info measurement already accounts for it.
    const auto side_bits = [&] {
        BitWriter probe;
        emit_frame(probe, config_, words, payload, aux);
        return static_cast<std::uint32_t>(probe.bit_count());
    }();
    if (side_bits + kTailBits > total_bits) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    const std::uint32_t budget = total_bits - side_bits - kTailBits;

    std::vector<std::span<const std::uint8_t>> bap_views(
        static_cast<std::size_t>(nchans));
    const auto bits_at = [&](int composite) {
        // Every channel shares one fsnroffst, so the frame-wide
        // §7.2.2.1.1 condition reduces to the composite being zero.
        const BitAllocRegion region{.snr_all_zero = composite == 0};
        for (int ch = 0; ch < nchans; ++ch) {
            auto& plan = payload.chans[static_cast<std::size_t>(ch)];
            compute_bit_allocation(plan.decoded, config_.sample_rate, kBamode0Codes,
                                   composite >> 4, composite & 15, plan.bap, region);
            bap_views[static_cast<std::size_t>(ch)] = plan.bap;
        }
        // Every block reuses the same exponents, hence the same allocation.
        return static_cast<std::uint32_t>(mantissa_bits_per_block(bap_views)) *
               kBlocksPerFrame;
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
    const std::uint32_t mantissa_bits = bits_at(lo);  // leaves payload.bap at lo
    assert(mantissa_bits <= budget);
    payload.csnroffst = lo >> 4;
    payload.fsnroffst = lo & 15;

    // --- 4. Mantissa tokens per block --------------------------------------
    std::size_t token_bits = 0;
    for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
        MantissaBlockWriter writer;
        for (int ch = 0; ch < nchans; ++ch) {
            const auto& plan = payload.chans[static_cast<std::size_t>(ch)];
            const auto& coeffs = fixed_at(ch, blk);
            for (int bin = 0; bin < plan.endmant; ++bin) {
                const int exp = plan.decoded[static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(coeffs[static_cast<std::size_t>(bin)])
                    << exp);
                writer.add(mantissa, plan.bap[static_cast<std::size_t>(bin)]);
            }
        }
        writer.finish_block();
        token_bits += writer.bit_count();
        payload.mantissas[static_cast<std::size_t>(blk)] = writer.tokens();
    }
    // The search's fast counter and the packer must agree exactly, or every
    // block after the first lands at the wrong bit offset.
    assert(token_bits == mantissa_bits);
    (void)token_bits;

    return finish_frame(config_, words, payload, aux);
}

// --- access units ----------------------------------------------------------

namespace {

// The substreams of one access unit in transmission order, with the identity
// fields Annex E fixes rather than leaves to the caller: the independent one
// first, then dependents numbered from 0 in their own space, the last of which
// carries the compre marker that closes the program.
std::expected<std::vector<FrameConfig>, FrameError> substream_configs(
    const AccessUnitConfig& config) {
    if (config.independent.strmtyp != StreamType::kIndependent) {
        return std::unexpected(FrameError::kInvalidSubstream);
    }
    // §E2.3.1.2: eight dependents per independent substream, no more.
    if (config.dependents.size() > 8) {
        return std::unexpected(FrameError::kInvalidSubstream);
    }
    std::vector<FrameConfig> out;
    out.reserve(config.dependents.size() + 1);
    out.push_back(config.independent);
    out.back().substreamid = 0;
    out.back().last_dependent = false;

    for (std::size_t i = 0; i < config.dependents.size(); ++i) {
        FrameConfig dep = config.dependents[i];
        // Every substream codes the same 1536 samples of one program, so a
        // dependent cannot disagree with its parent about the sample rate.
        if (dep.sample_rate != config.independent.sample_rate) {
            return std::unexpected(FrameError::kInvalidSubstream);
        }
        dep.strmtyp = StreamType::kDependent;
        dep.substreamid = static_cast<int>(i);
        dep.last_dependent = i + 1 == config.dependents.size();
        out.push_back(dep);
    }
    for (const auto& sub : out) {
        if (const auto ok = validate(sub); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return out;
}

}  // namespace

std::span<const std::byte> AccessUnit::substream(std::size_t index) const {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < index; ++i) {
        offset += substream_bytes[i];
    }
    return std::span{bytes}.subspan(offset, substream_bytes[index]);
}

std::uint32_t access_unit_words(const AccessUnitConfig& config) {
    std::uint32_t words =
        frame_words(config.independent.sample_rate, config.independent.bitrate_kbps);
    for (const auto& dep : config.dependents) {
        words += frame_words(dep.sample_rate, dep.bitrate_kbps);
    }
    return words;
}

std::expected<AccessUnit, FrameError> build_silent_access_unit(
    const AccessUnitConfig& config, AuxPayload aux) {
    const auto subs = substream_configs(config);
    if (!subs) {
        return std::unexpected(subs.error());
    }
    AccessUnit unit;
    for (const auto& sub : *subs) {
        const bool carries_aux = &sub == &subs->back();  // §8.2: the last one
        const auto frame = build_silent_frame(sub, carries_aux ? aux : AuxPayload{});
        if (!frame) {
            return std::unexpected(frame.error());
        }
        unit.substream_bytes.push_back(static_cast<std::uint32_t>(frame->size()));
        unit.bytes.insert(unit.bytes.end(), frame->begin(), frame->end());
    }
    return unit;
}

AccessUnitEncoder::AccessUnitEncoder(const AccessUnitConfig& config) : config_(config) {
    // Identity is settled once here so encode_access_unit stays a hot path and
    // so a caller cannot renumber substreams between frames.
    if (const auto subs = substream_configs(config)) {
        for (const auto& sub : *subs) {
            substreams_.emplace_back(sub);
        }
    }
}

int AccessUnitEncoder::channel_count() const {
    int total = 0;
    for (const auto& sub : substreams_) {
        total += sub.channel_count();
    }
    return total;
}

std::expected<AccessUnit, FrameError> AccessUnitEncoder::encode_access_unit(
    std::span<const std::span<const float>> channels, AuxPayload aux) {
    if (substreams_.empty()) {
        // The constructor rejected the layout; re-run it for the real reason.
        const auto subs = substream_configs(config_);
        return std::unexpected(subs ? FrameError::kInvalidSubstream : subs.error());
    }
    assert(static_cast<int>(channels.size()) == channel_count());

    AccessUnit unit;
    std::size_t taken = 0;
    for (auto& sub : substreams_) {
        const auto count = static_cast<std::size_t>(sub.channel_count());
        const bool carries_aux = &sub == &substreams_.back();  // §8.2
        const auto frame =
            sub.encode_frame(channels.subspan(taken, count), carries_aux ? aux : AuxPayload{});
        if (!frame) {
            return std::unexpected(frame.error());
        }
        taken += count;
        unit.substream_bytes.push_back(static_cast<std::uint32_t>(frame->size()));
        unit.bytes.insert(unit.bytes.end(), frame->begin(), frame->end());
    }
    return unit;
}

}  // namespace ac3::eac3
