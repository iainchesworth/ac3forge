#include "ac3/encoder/eac3_frame.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/window.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/encoder/eac3_tools.hpp"

namespace ac3::eac3 {

namespace {

// Frame-level strategy flags. The tools that stay off here are off because
// nothing in this encoder drives them, not because the container cannot
// carry them; the ones a FrameConfig can switch on appear below.
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
// AC-3 has to push its padding through in-block skip fields, because §5.5
// confines the aux field to the final 3/8 of the frame - a rule that exists
// to protect the crc1-at-5/8 checkpoint. E-AC-3 has no crc1 and Annex E
// states no equivalent constraint, so auxbits can absorb the whole
// remainder. That is both simpler and, measurably, what decoders expect:
// padding routed through a block-0 skip field came back as audible data,
// and FFmpeg's own encoder likewise sets skipflde to 0.
constexpr int kSkipflde = 0;
constexpr int kSpxattene = 0;      // no spectral extension attenuation

constexpr int kTailBits = 18;  // auxdatae + crcrsv + crc2

// One coded stream: its exponents (frame-constant, D15 in block 0) and the
// allocation they produce. The full-bandwidth channels come first, then LFE,
// then - when coupling is in use - the shared coupling channel, which is a
// stream like any other except that it starts above bin 0.
struct ChannelPlan {
    int start = 0;    // strtmant: 0 for fbw and LFE, cplstrtmant for coupling
    int endmant = 0;
    EncodedExponents coded;              // fbw and LFE channels
    EncodedCouplingExponents cpl_coded;  // the coupling channel
    // Both are indexed from bin 0 even when the stream starts higher, because
    // that is what the allocator wants; the bins below `start` are inert.
    std::vector<std::uint8_t> decoded;  // decoder-mirror exponents
    std::vector<std::uint8_t> bap;
};

// Everything the coupling tool contributes to a frame. Annex E hoists
// cplstre/cplinu out of the blocks and into audfrm, so whether a block
// couples is a frame-level decision; this encoder either couples every block
// or none, which is also the only shape that leaves ncplregs at 1.
struct CouplingPlan {
    bool in_use = false;
    int begf = 0;
    int endf = 0;
    int strtmant = 0;
    int endmant = 0;
    int nsubnd = 0;
    std::array<bool, kMaxSubBands> structure{};
    BandLayout bands{};
    int fleak = 0;
    int sleak = 0;
    // Coordinates go out in blocks 0, 2 and 4 and are reused in between
    // (§8.2.4.1). A reusing block holds a copy of what was actually sent, so
    // the encoder's own view of the decoder's state is never a special case.
    std::array<bool, kBlocksPerFrame> send{};
    std::vector<int> master;                   // [blk][ch]
    std::vector<coupling::Coordinate> coords;  // [blk][ch][bnd]
};

struct Payload {
    int csnroffst = 0;
    int fsnroffst = 0;
    CouplingPlan cpl;
    std::vector<ChannelPlan> chans;
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> mantissas;
};

// §7.5.2.1-7.5.2.4: with coupling in use the rematrixing bands cannot reach
// above the coupling frequency, so both their count and where the last one
// stops change. Nothing here rematrixes yet, but the COUNT is transmitted, so
// getting it wrong shifts every later field in block 0.
[[nodiscard]] int rematrix_band_count(const CouplingPlan& cpl) {
    if (!cpl.in_use) {
        return 4;
    }
    if (cpl.begf == 0) {
        return 2;
    }
    return cpl.begf < 3 ? 3 : 4;
}

// Where coupling should start when the caller does not say. Sub-band 4 - bin
// 85, 8.0 kHz at 48 kHz - is the floor, because that is roughly where
// per-channel waveform detail stops being what a listener is hearing. Below
// it the envelope metric keeps improving and waveform SNR falls off a cliff;
// above it coupling still helps but has less left to save. The band edge
// rises slowly with the per-channel rate, since a channel that can afford its
// own high band should keep it.
//
// This is a default, not a limit: FrameConfig::cplbegf overrides it, and a
// caller who trusts banded envelope fidelity over waveform fidelity has good
// reason to go lower. At 96 kbit/s stereo, coupling from sub-band 0 scores a
// full dB better on log-spectral distance than not coupling at all.
[[nodiscard]] int default_cplbegf(std::uint32_t bitrate_kbps, int nfchans) {
    const int per_channel = static_cast<int>(bitrate_kbps) / std::max(nfchans, 1);
    return std::clamp(4 + (per_channel - 48) / 24, 4, 10);
}

// Everything from the sync word to the end of the last block: the whole
// frame bar padding and the tail. Silence and real audio go through this one
// function, so the two can never drift apart on field placement.
void emit_frame(BitWriter& w, const FrameConfig& config, std::uint32_t words,
                const Payload& payload) {
    const int nfchans = fullbw_channel_count(config.acmod);
    const bool dependent = config.strmtyp == StreamType::kDependent;
    const auto& cpl = payload.cpl;

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
    w.put(0, 1);  // addbsie

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
    w.put(kSkipflde, 1);
    w.put(kSpxattene, 1);

    if (static_cast<std::uint8_t>(config.acmod) > 0x1) {
        w.put(cpl.in_use ? 1 : 0, 1);  // cplinu[0] (cplstre[0] is implied 1)
        for (int blk = 1; blk < kBlocksPerFrame; ++blk) {
            w.put(0, 1);  // cplstre[blk] = 0, so cplinu inherits block 0's
        }
    }
    // expstre == 0: one Table E2.10 code per channel covers all six blocks.
    // frmcplexpstr precedes them, and exists only when some block couples.
    if (cpl.in_use) {
        w.put(kFrmExpStrategyCode, 5);  // frmcplexpstr
    }
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

        // Coupling strategy. cplstre[0] is implied 1, so block 0 carries one;
        // blocks 1-5 sent cplstre 0 in audfrm, so they carry none at all.
        if (cpl.in_use && first) {
            w.put(0, 1);  // ecplinu: standard coupling, not enhanced
            // 2/0 is the one mode where chincpl is not transmitted: both
            // channels are coupled by definition.
            if (config.acmod != Acmod::k2_0) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    w.put(1, 1);  // chincpl[ch]: every fbw channel couples
                }
            } else {
                w.put(0, 1);  // phsflginu: no phase restoration
            }
            w.put(static_cast<std::uint32_t>(cpl.begf), 4);
            w.put(static_cast<std::uint32_t>(cpl.endf), 4);  // spxinu == 0
            // The banding structure is sent rather than defaulted. Leaving
            // cplbndstrce at 0 would hand the decoder Table E2.12's default,
            // which is NOT one band per sub-band and whose indexing the
            // standard pins to the array's first element being sub-band
            // cplbegf (§5.4.3.13) - a reading real decoders do not share.
            // ncplsubnd - 1 bits a frame settles the question outright.
            w.put(1, 1);  // cplbndstrce
            for (int sbnd = 1; sbnd < cpl.nsubnd; ++sbnd) {
                w.put(cpl.structure[static_cast<std::size_t>(sbnd)] ? 1 : 0, 1);
            }
        }

        // Coupling coordinates. firstcplcos[ch] starts at 1, so block 0's
        // cplcoe is implied 1 and not transmitted - one bit per channel that
        // AC-3 does send and Annex E does not.
        if (cpl.in_use) {
            const bool send = cpl.send[static_cast<std::size_t>(blk)];
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!first) {
                    w.put(send ? 1 : 0, 1);  // cplcoe[ch]
                }
                if (send) {
                    const auto at = static_cast<std::size_t>(blk) *
                                        static_cast<std::size_t>(nfchans) +
                                    static_cast<std::size_t>(ch);
                    w.put(static_cast<std::uint32_t>(cpl.master[at]), 2);  // mstrcplco
                    for (int bnd = 0; bnd < cpl.bands.count; ++bnd) {
                        const auto coordinate =
                            cpl.coords[at * static_cast<std::size_t>(cpl.bands.count) +
                                       static_cast<std::size_t>(bnd)];
                        w.put(coordinate.exp, 4);
                        w.put(coordinate.mant, 4);
                    }
                }
            }
            // phsflginu == 0, so no phase flags follow.
        }

        if (config.acmod == Acmod::k2_0) {
            // Unlike AC-3, block 0's rematstr is IMPLIED 1 rather than
            // transmitted - only later blocks carry the bit. Sending it
            // anyway shifts the rest of the block by one.
            if (!first) {
                w.put(0, 1);  // rematstr: keep the previous flags
            } else {
                for (int band = 0; band < rematrix_band_count(cpl); ++band) {
                    w.put(0, 1);  // rematflg
                }
            }
        }

        // chbwcod accompanies a fresh exponent strategy, but only for a
        // channel carrying its own high band: a coupled channel's bandwidth
        // IS the coupling frequency, and sending chbwcod anyway would both
        // waste the bits and desynchronise the block.
        if (first) {
            if (!cpl.in_use) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    w.put(static_cast<std::uint32_t>(config.chbwcod), 6);
                }
            }
            // Exponents: the coupling channel first, then fbw, then LFE.
            if (cpl.in_use) {
                const auto& coded = payload.chans.back().cpl_coded;
                w.put(coded.cplabsexp, 4);
                for (const auto group : coded.groups) {
                    w.put(group, 7);
                }
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
                const auto& coded =
                    payload.chans[static_cast<std::size_t>(nfchans)].coded;
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
        // The coupling leak seeds follow the same first-time rule as the
        // coordinates: firstcplleak starts at 1, so block 0's cplleake is
        // implied and the seeds are mandatory there.
        if (cpl.in_use) {
            if (!first) {
                w.put(0, 1);  // cplleake: keep the seeds from block 0
            } else {
                w.put(static_cast<std::uint32_t>(cpl.fleak), 3);
                w.put(static_cast<std::uint32_t>(cpl.sleak), 3);
            }
        }
        // dbaflde == 0: no delta allocation. skipflde == 0: no skip field.

        for (const auto& token : payload.mantissas[static_cast<std::size_t>(blk)]) {
            w.put(token.value, token.bits);
        }
    }
}

// Pad with auxbits, close the tail and patch crc2.
std::expected<std::vector<std::byte>, FrameError> finish_frame(
    const FrameConfig& config, std::uint32_t words, const Payload& payload) {
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;

    BitWriter probe;
    emit_frame(probe, config, words, payload);
    const auto content_bits = static_cast<std::uint32_t>(probe.bit_count());
    if (content_bits + kTailBits > total_bits) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    const std::uint32_t spare = total_bits - content_bits - kTailBits;

    BitWriter w;
    emit_frame(w, config, words, payload);
    for (std::uint32_t i = 0; i < spare; ++i) {
        w.put(0, 1);  // auxbits
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
    const FrameConfig& config) {
    if (const auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }

    const int nfchans = fullbw_channel_count(config.acmod);
    const int endmant = ((config.chbwcod + 12) * 3) + 37;

    // Exponents: an all-quiet ramp, so the decoder's own allocation returns
    // zero everywhere. Both offsets stay at zero, which §7.2.2.1.1 defines as
    // an all-zero allocation - no mantissas exist and the frame is pure
    // syntax.
    // Coupling stays off: a silent frame has nothing to share, and switching
    // it on would only add coordinates describing zero.
    Payload payload;
    const std::vector<std::uint8_t> quiet(static_cast<std::size_t>(endmant), kMaxExponent);
    for (int ch = 0; ch < nfchans; ++ch) {
        ChannelPlan plan;
        plan.endmant = endmant;
        plan.coded = encode_exponents(quiet, ExpStrategy::kD15);
        payload.chans.push_back(std::move(plan));
    }
    if (config.lfe) {
        const std::vector<std::uint8_t> lfe_quiet(kLfeEndmant, kMaxExponent);
        ChannelPlan plan;
        plan.endmant = kLfeEndmant;
        plan.coded = encode_exponents(lfe_quiet, ExpStrategy::kD15);
        payload.chans.push_back(std::move(plan));
    }

    return finish_frame(config, frame_words(config.sample_rate, config.bitrate_kbps),
                        payload);
}

std::expected<std::vector<std::byte>, FrameError> FrameEncoder::encode_frame(
    std::span<const std::span<const float>> channels) {
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

    const std::uint32_t words = frame_words(config_.sample_rate, config_.bitrate_kbps);
    const std::uint32_t total_bits = words * 16;

    // --- 1. Coupling decision ----------------------------------------------
    Payload payload;
    auto& cpl = payload.cpl;
    // §E2.2.3 gates the whole coupling element on acmod > 0x1, so 1/0 and the
    // rejected 1+1 cannot couple however the caller asks.
    cpl.in_use = config_.coupling && static_cast<std::uint8_t>(config_.acmod) > 0x1;
    if (cpl.in_use) {
        cpl.begf = std::clamp(config_.cplbegf >= 0
                                  ? config_.cplbegf
                                  : default_cplbegf(config_.bitrate_kbps, nfchans),
                              0, 15);
        // Coupling runs to the top of the coded spectrum. With chbwcod gone
        // for a coupled channel the coupling end frequency IS its bandwidth,
        // so stopping short would not save the top band's bits - it would
        // discard the band, which is what the tool exists to avoid.
        cpl.endf = 15;
        cpl.strtmant = kCplFirstBin + kCplBinsPerSubBand * cpl.begf;
        cpl.endmant = kCplFirstBin + kCplBinsPerSubBand * (cpl.endf + 3);
        cpl.nsubnd = 3 + cpl.endf - cpl.begf;
        std::copy_n(kDefaultCplBandStructure.begin(), cpl.nsubnd, cpl.structure.begin());
        cpl.bands = group_bands(cpl.strtmant, cpl.nsubnd, kCplBinsPerSubBand,
                                std::span{cpl.structure});
    }

    const int fbw_endmant =
        cpl.in_use ? cpl.strtmant : ((config_.chbwcod + 12) * 3) + 37;
    // Streams: the fbw channels, the LFE, then the coupling channel as one
    // more stream carrying the shared high band.
    const int cpl_stream = cpl.in_use ? nchans : -1;
    const int streams = nchans + (cpl.in_use ? 1 : 0);
    const auto stream_start = [&](int s) { return s == cpl_stream ? cpl.strtmant : 0; };
    const auto stream_end = [&](int s) {
        if (s == cpl_stream) {
            return cpl.endmant;
        }
        return s < nfchans ? fbw_endmant : kLfeEndmant;
    };

    // --- 2. MDCT ------------------------------------------------------------
    std::vector<std::array<double, 256>> coeffs(
        static_cast<std::size_t>(streams) * kBlocksPerFrame);
    const auto coeffs_at = [&](int s, int blk) -> std::array<double, 256>& {
        return coeffs[static_cast<std::size_t>(s) * kBlocksPerFrame +
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
            mdct512_forward(windowed, coeffs_at(ch, blk));
        }
        for (int n = 0; n < 256; ++n) {
            hist[static_cast<std::size_t>(n)] =
                static_cast<double>(pcm[static_cast<std::size_t>(1280 + n)]);
        }
    }

    // --- 3. Coupling: the shared channel and its coordinates ---------------
    const auto nbnd = static_cast<std::size_t>(std::max(cpl.bands.count, 1));
    const auto coord_slot = [&](int blk, int ch) {
        return static_cast<std::size_t>(blk) * static_cast<std::size_t>(nfchans) +
               static_cast<std::size_t>(ch);
    };
    if (cpl.in_use) {
        cpl.master.assign(static_cast<std::size_t>(kBlocksPerFrame) *
                              static_cast<std::size_t>(nfchans),
                          0);
        cpl.coords.assign(cpl.master.size() * nbnd, {});
        std::vector<double> values(nbnd, 0.0);

        // §7.4.1: the coupling channel is the AVERAGE of the coupled
        // channels' coefficients. The divisor is not a free parameter, and
        // this encoder measured both ways it can be got wrong.
        //
        // Scaling the shared channel UP - normalising each band, or the whole
        // region, to unit peak - looks attractive because it makes the
        // coordinate small and so unclampable. But the bit allocator reads
        // psd absolutely, against a fixed hearing threshold: a coupling
        // channel normalised to full scale is simply the loudest thing in the
        // frame, and the allocator buys it bits accordingly. Measured at 128
        // kbit/s, that handed the coupling channel 291 of the 420 mantissa
        // bits in a block - more per bin than the baseband it was supposed to
        // be subsidising - and the frame's coarse SNR offset fell from 27 to
        // 11. Coupling made the encoder run out of bits SOONER.
        //
        // The mean leaves the shared channel at the natural level of one
        // coupled channel, which is the level the allocator's model expects.
        // It also has to be one constant for the whole FRAME rather than per
        // block: coordinates go out in blocks 0, 2 and 4 and are reused in 1,
        // 3 and 5, so any per-block term in the scale reaches the decoder
        // multiplied by the wrong block's value.
        const double scale = static_cast<double>(nfchans);
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            cpl.send[static_cast<std::size_t>(blk)] = blk % 2 == 0;
            auto& shared = coeffs_at(cpl_stream, blk);
            shared.fill(0.0);
            for (int bin = cpl.strtmant; bin < cpl.endmant; ++bin) {
                double sum = 0.0;
                for (int ch = 0; ch < nfchans; ++ch) {
                    sum += coeffs_at(ch, blk)[static_cast<std::size_t>(bin)];
                }
                shared[static_cast<std::size_t>(bin)] = sum;
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                for (int bnd = 0; bnd < cpl.bands.count; ++bnd) {
                    const int low = cpl.bands.start[static_cast<std::size_t>(bnd)];
                    const int high = low + cpl.bands.size[static_cast<std::size_t>(bnd)];
                    double power_ch = 0.0;
                    double power_sum = 0.0;
                    for (int bin = low; bin < high; ++bin) {
                        const double value =
                            coeffs_at(ch, blk)[static_cast<std::size_t>(bin)];
                        const double summed = shared[static_cast<std::size_t>(bin)];
                        power_ch += value * value;
                        power_sum += summed * summed;
                    }
                    // The decoder computes channel = coupling * coordinate * 8
                    // and the stored coupling is sum / scale, so the
                    // coordinate that restores this band's energy is
                    // sqrt(E_ch / E_sum) * scale / 8.
                    const double ratio =
                        power_sum > 0.0 ? std::sqrt(power_ch / power_sum) : 0.0;
                    values[static_cast<std::size_t>(bnd)] = ratio * scale / 8.0;
                }
                const int chosen = coupling::choose_master(values);
                cpl.master[coord_slot(blk, ch)] = chosen;
                for (int bnd = 0; bnd < cpl.bands.count; ++bnd) {
                    cpl.coords[coord_slot(blk, ch) * nbnd + static_cast<std::size_t>(bnd)] =
                        coupling::quantize_coordinate(values[static_cast<std::size_t>(bnd)],
                                                      chosen);
                }
            }
            // A block that reuses coordinates must reuse the ones actually
            // transmitted, or encoder and decoder diverge from block 1 on.
            if (!cpl.send[static_cast<std::size_t>(blk)]) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    cpl.master[coord_slot(blk, ch)] = cpl.master[coord_slot(blk - 1, ch)];
                    for (std::size_t bnd = 0; bnd < nbnd; ++bnd) {
                        cpl.coords[coord_slot(blk, ch) * nbnd + bnd] =
                            cpl.coords[coord_slot(blk - 1, ch) * nbnd + bnd];
                    }
                }
            }
            for (int bin = cpl.strtmant; bin < cpl.endmant; ++bin) {
                shared[static_cast<std::size_t>(bin)] /= scale;
            }
        }
    }

    // --- 4. Fixed point -----------------------------------------------------
    std::vector<std::array<std::int32_t, 256>> fixed(
        static_cast<std::size_t>(streams) * kBlocksPerFrame);
    const auto fixed_at = [&](int s, int blk) -> std::array<std::int32_t, 256>& {
        return fixed[static_cast<std::size_t>(s) * kBlocksPerFrame +
                     static_cast<std::size_t>(blk)];
    };
    for (int s = 0; s < streams; ++s) {
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            const auto& source = coeffs_at(s, blk);
            auto& out = fixed_at(s, blk);
            for (int bin = stream_start(s); bin < stream_end(s); ++bin) {
                out[static_cast<std::size_t>(bin)] =
                    to_fixed25(source[static_cast<std::size_t>(bin)]);
            }
        }
    }

    // --- 5. One frame-constant exponent set per stream ---------------------
    // Table E2.10 code 0 sends D15 in block 0 and reuses it for the other
    // five, so a bin's exponent has to accommodate its LOUDEST block. The
    // smallest exponent across the frame is that bin's worst case; anything
    // larger would overflow the mantissa in the block that peaks.
    payload.chans.resize(static_cast<std::size_t>(streams));
    for (int s = 0; s < streams; ++s) {
        auto& plan = payload.chans[static_cast<std::size_t>(s)];
        plan.start = stream_start(s);
        plan.endmant = stream_end(s);
        const auto span = static_cast<std::size_t>(plan.endmant - plan.start);

        std::vector<std::uint8_t> raw(span, kMaxExponent);
        std::vector<std::uint8_t> block_exps(span, 0);
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            extract_exponents(
                std::span{fixed_at(s, blk)}.subspan(static_cast<std::size_t>(plan.start),
                                                    span),
                block_exps);
            for (std::size_t bin = 0; bin < span; ++bin) {
                raw[bin] = std::min(raw[bin], block_exps[bin]);
            }
        }
        // Bins below the stream's own start are inert but must still hold a
        // value the allocator can read; the quietest possible one keeps them
        // from influencing anything.
        plan.decoded.assign(static_cast<std::size_t>(plan.endmant), kMaxExponent);
        if (s == cpl_stream) {
            plan.cpl_coded = encode_coupling_exponents(raw, ExpStrategy::kD15);
            decode_coupling_exponents(
                plan.cpl_coded.cplabsexp, plan.cpl_coded.groups, ExpStrategy::kD15,
                std::span{plan.decoded}.subspan(static_cast<std::size_t>(plan.start)));
        } else {
            plan.coded = encode_exponents(raw, ExpStrategy::kD15);
            decode_exponents(plan.coded.absolute, plan.coded.groups, ExpStrategy::kD15,
                             plan.decoded);
        }
        plan.bap.assign(static_cast<std::size_t>(plan.endmant), 0);
    }

    // --- 6. Coupling leak seeds ---------------------------------------------
    // The coupling channel's allocation starts above the low-frequency region
    // entirely, so instead of running lowcomp it continues the masking decay
    // from transmitted leak state. Deriving the seeds from the coupling
    // channel's own first band starts the allocator at a sensible level.
    if (cpl.in_use) {
        const auto& plan = payload.chans[static_cast<std::size_t>(cpl_stream)];
        const int exp = plan.decoded[static_cast<std::size_t>(cpl.strtmant)];
        const int psd = 3072 - (exp << 7);
        cpl.fleak = std::clamp((psd - fast_gain(kBamode0Codes.fgaincod) - 768) >> 8, 0, 7);
        cpl.sleak = std::clamp((psd - slow_gain(kBamode0Codes.sgaincod) - 768) >> 8, 0, 7);
    }

    // --- 7. SNR-offset search ----------------------------------------------
    // The side info is offset-independent here (bamode 0, no delta
    // allocation), so it can be measured once and the remainder handed
    // wholly to the mantissas.
    const auto side_bits = [&] {
        BitWriter probe;
        emit_frame(probe, config_, words, payload);
        return static_cast<std::uint32_t>(probe.bit_count());
    }();
    if (side_bits + kTailBits > total_bits) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    const std::uint32_t budget = total_bits - side_bits - kTailBits;

    std::vector<std::span<const std::uint8_t>> bap_views(
        static_cast<std::size_t>(streams));
    const auto bits_at = [&](int composite) {
        for (int s = 0; s < streams; ++s) {
            auto& plan = payload.chans[static_cast<std::size_t>(s)];
            // Every stream shares one fsnroffst, so the frame-wide
            // §7.2.2.1.1 condition reduces to the composite being zero.
            const BitAllocRegion region{.start = plan.start,
                                        .coupling = s == cpl_stream,
                                        .cplfleak = cpl.fleak,
                                        .cplsleak = cpl.sleak,
                                        .snr_all_zero = composite == 0};
            compute_bit_allocation(plan.decoded, config_.sample_rate, kBamode0Codes,
                                   composite >> 4, composite & 15, plan.bap, region);
            // Only the stream's own region carries mantissas.
            bap_views[static_cast<std::size_t>(s)] =
                std::span{plan.bap}.subspan(static_cast<std::size_t>(plan.start));
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

    // --- 8. Mantissa tokens per block --------------------------------------
    // §E2.2.4 ordering: each fbw channel's mantissas, with the coupling
    // channel's inserted right after the FIRST coupled channel, then the LFE.
    std::size_t token_bits = 0;
    for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
        MantissaBlockWriter writer;
        const auto emit_stream = [&](int s) {
            const auto& plan = payload.chans[static_cast<std::size_t>(s)];
            const auto& block = fixed_at(s, blk);
            for (int bin = plan.start; bin < plan.endmant; ++bin) {
                const int exp = plan.decoded[static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(block[static_cast<std::size_t>(bin)]) << exp);
                writer.add(mantissa, plan.bap[static_cast<std::size_t>(bin)]);
            }
        };
        bool emitted_coupling = false;
        for (int ch = 0; ch < nfchans; ++ch) {
            emit_stream(ch);
            if (cpl.in_use && !emitted_coupling) {
                emit_stream(cpl_stream);
                emitted_coupling = true;
            }
        }
        if (config_.lfe) {
            emit_stream(nfchans);
        }
        writer.finish_block();
        token_bits += writer.bit_count();
        payload.mantissas[static_cast<std::size_t>(blk)] = writer.tokens();
    }
    // The search's fast counter and the packer must agree exactly, or every
    // block after the first lands at the wrong bit offset.
    assert(token_bits == mantissa_bits);
    (void)token_bits;

    return finish_frame(config_, words, payload);
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
    const AccessUnitConfig& config) {
    const auto subs = substream_configs(config);
    if (!subs) {
        return std::unexpected(subs.error());
    }
    AccessUnit unit;
    for (const auto& sub : *subs) {
        const auto frame = build_silent_frame(sub);
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
    std::span<const std::span<const float>> channels) {
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
        const auto frame = sub.encode_frame(channels.subspan(taken, count));
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
