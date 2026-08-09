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
// How far the six blocks' energies may spread before a channel is judged too
// transient for the adaptive hybrid transform. An order of magnitude: below
// that the DCT concentrates, above it the loud block smears across all six.
constexpr double kAhtStationaryRatio = 10.0;
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

// How deep to taper the seam when the caller does not say. The taps come out
// at -1.2, -2.4 and -3.6 dB, deepest on the join - a gentle smoothing rather
// than a hole.
//
// This is a judgement, not a tuned value, and it is worth being plain about
// why: the standard offers no guidance on choosing spxattencod, Dolby's own
// encoder never emits the field at all, and the artifact the notch exists to
// soften - a splice between two unrelated pieces of spectrum - is not
// something the banded metrics this project measures with can see. The depth
// is exposed through FrameConfig for anyone who can hear the difference.
constexpr int kDefaultSpxAttenCod = 2;

constexpr int kTailBits = 18;  // auxdatae + crcrsv + crc2

// The skip field is 9 bits of length, so one block can hold this much.
constexpr std::size_t kMaxSkipBytes = 511;

// Which block carries the whole container. Dolby's own streams use a middle
// block; a decoder that scans for the EMDF sync word should not care.
constexpr int kMetadataBlock = 0;

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
    // bap for an ordinary stream; hebap (0..19, §E3.4.3.1) for an AHT one.
    std::vector<std::uint8_t> bap;
    // §E3.4: when set, this stream's six blocks are transformed together and
    // its whole frame of mantissas is emitted in block 0. The transform
    // output IS the mantissa, so the exponents are derived from it rather
    // than from the MDCT coefficients - see the note where they are built.
    bool aht = false;
    int gaqmod = 0;
    std::vector<std::array<std::int32_t, kBlocksPerFrameSize>> aht_fixed;  // [bin][j]
    // The normalised mantissas through the rate search; overwritten with the
    // decoder's reconstruction once they are packed.
    std::vector<std::array<double, kBlocksPerFrameSize>> aht_coeffs;
    std::vector<std::uint8_t> aht_gain;  // per bin: 1, 2 or 4
};

// The whole-frame mantissa cost of one AHT stream under a given gain mode,
// leaving behind the per-bin gains that produce it.
//
// Gain-adaptive quantization is what makes this a function rather than a sum
// over a table: whether a mantissa needs its escape codeword depends on the
// mantissa, so the only way to know a frame's size is to quantize it. The
// rate search therefore does exactly that on every iteration, and the packer
// reuses the gains left here so the two cannot disagree.
[[nodiscard]] std::uint32_t aht_stream_bits(ChannelPlan& plan, int gaqmod) {
    std::uint32_t bits = 2;  // chgaqmod itself, which is part of the element
    int active = 0;
    for (int bin = plan.start; bin < plan.endmant; ++bin) {
        const auto at = static_cast<std::size_t>(bin);
        const int hebap = plan.bap[at];
        plan.aht_gain[at] = 1;
        if (hebap == 0) {
            continue;
        }
        if (hebap <= 7) {
            bits += static_cast<std::uint32_t>(aht_bin_bits(hebap));  // one VQ index
            continue;
        }
        const int mantissa_bits = aht_mantissa_bits(hebap);
        if (aht_gaq_has_gain(hebap, gaqmod)) {
            plan.aht_gain[at] = static_cast<std::uint8_t>(
                aht_choose_gain(plan.aht_coeffs[at], mantissa_bits, gaqmod));
            ++active;
        }
        bits += static_cast<std::uint32_t>(
            aht_bin_gaq_bits(plan.aht_coeffs[at], mantissa_bits, plan.aht_gain[at]));
    }
    bits += static_cast<std::uint32_t>(aht_gaq_sections(active, gaqmod) *
                                       aht_gaq_gain_bits(gaqmod));
    return bits;
}

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

// Everything the spectral extension tool contributes. There is no shared
// channel and no mantissas: above startmant the bitstream carries only these
// per-band scale factors, and the decoder rebuilds the band by copying a
// lower one up, blending noise into it and scaling the result to match.
struct SpxPlan {
    bool in_use = false;
    int begf = 0;
    int endf = 0;
    int strtf = 0;
    int begin_subbnd = 0;
    int end_subbnd = 0;
    int startmant = 0;   // where synthesis begins - and coding stops
    int endmant = 0;     // one past the last synthesized coefficient
    int copystart = 0;   // first coefficient of the copy source region
    std::array<bool, kSpxSubBands> structure{};
    BandLayout bands{};
    std::array<bool, kBlocksPerFrame> send{};
    std::vector<int> blend;                    // [blk][ch] spxblnd
    std::vector<int> master;                   // [blk][ch] mstrspxco
    std::vector<coupling::Coordinate> coords;  // [blk][ch][bnd]
    // §E3.6.4.2.3. attencod is per channel and frame-constant; wrapflag says
    // which band boundaries the copy wrapped at, and so where the notch goes.
    bool atten = false;
    std::vector<int> attencod;                 // [ch], -1 when that channel opts out
    std::array<bool, kMaxSubBands> wrapflag{};
};

struct Payload {
    int csnroffst = 0;
    int fsnroffst = 0;
    bool ahte = false;  // some stream uses the adaptive hybrid transform
    CouplingPlan cpl;
    SpxPlan spx;
    std::vector<ChannelPlan> chans;
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> mantissas;
    // §7.7.1 words per block. All unity when the config carries no profile,
    // and then nothing is transmitted at all.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // §7.7.2. std::nullopt means "no heavy-compression word", which is a
    // different statement from "a word saying unity".
    std::optional<std::uint8_t> compr = std::nullopt;
    // Ch2's own words, present only when acmod is kDualMono.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    std::optional<std::uint8_t> compr2 = std::nullopt;
};

// §E3.3.2: the rematrixing bands cannot reach above whichever tool takes over
// the spectrum first, so both their count and where the last one stops depend
// on coupling and spectral extension. Nothing here rematrixes yet, but the
// COUNT is transmitted, so getting it wrong shifts every later field in
// block 0.
[[nodiscard]] int rematrix_band_count(const CouplingPlan& cpl, const SpxPlan& spx) {
    if (cpl.in_use) {
        if (cpl.begf == 0) {
            return 2;
        }
        return cpl.begf < 3 ? 3 : 4;
    }
    if (spx.in_use) {
        return spx.begf < 2 ? 3 : 4;
    }
    return 4;
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

// Where synthesis should take over when the caller does not say. Spectral
// extension is the crudest of the tools - a copied band with noise stirred in
// and an envelope painted back on - so it belongs as high as the rate allows.
//
// Code 4, coefficient 97, 9.1 kHz at 48 kHz, is where it stops costing
// anything measurable: on the reference program it improves BOTH the banded
// envelope and waveform SNR against not using it, at every rate from 96 to
// 192 kbit/s. Lower start frequencies keep improving the envelope and give up
// waveform fidelity fast, which is a trade a caller can still ask for through
// FrameConfig::spxbegf but is not one to make on their behalf.
[[nodiscard]] int default_spxbegf(std::uint32_t bitrate_kbps, int nfchans) {
    const int per_channel = static_cast<int>(bitrate_kbps) / std::max(nfchans, 1);
    if (per_channel < 40) {
        return 3;  // coefficient 85, 8.0 kHz
    }
    if (per_channel < 136) {
        return 4;  // coefficient 97, 9.1 kHz
    }
    return 5;  // coefficient 109, 10.2 kHz
}

// The copy source has to be a band the decoder actually has: it must sit
// below where synthesis begins, and it wants to be wide enough that the wrap
// does not repeat a handful of bins over and over. Two sub-bands is the floor.
[[nodiscard]] int default_spxstrtf(int startmant) {
    int strtf = 0;
    for (int s = 1; s <= 3; ++s) {
        if (spx_band_start(s) + 2 * kSpxBinsPerSubBand <= startmant) {
            strtf = s;
        }
    }
    return strtf;
}

// §E3.6.4.2.1: how much of the synthesized band is noise rather than copied
// signal. The decoder derives a per-band factor from spxblnd and the band's
// place in the spectrum; what the encoder has to decide is the offset, which
// is a judgement about the material. Tonal content wants its harmonics copied
// and noise kept out; noise-like content is better served by noise, since a
// copied band lands its harmonics at the wrong frequencies.
//
// Spectral flatness answers exactly that question: near 0 for a tone, near 1
// for noise. noffset is spxblnd/32 and SUBTRACTS from the noise ratio, so a
// tone wants the offset high.
// §E3.6.4.2.1: the fraction of a band the decoder will fill with noise rather
// than with copied signal. It rises with frequency across the extension
// region and spxblnd shifts the whole curve down.
[[nodiscard]] double spx_noise_ratio(const SpxPlan& spx, int bnd, int blend) {
    const auto at = static_cast<std::size_t>(bnd);
    const double centre =
        spx.bands.start[at] + 0.5 * static_cast<double>(spx.bands.size[at]);
    const double ratio =
        centre / static_cast<double>(spx.endmant) - static_cast<double>(blend) / 32.0;
    return std::clamp(ratio, 0.0, 1.0);
}

[[nodiscard]] int spx_blend(std::span<const double> region) {
    double log_sum = 0.0;
    double sum = 0.0;
    int count = 0;
    for (const double value : region) {
        const double power = value * value + 1e-30;
        log_sum += std::log(power);
        sum += power;
        ++count;
    }
    if (count == 0 || !(sum > 0.0)) {
        return 31;  // nothing up here to blend; copying costs nothing either
    }
    const double flatness =
        std::exp(log_sum / count) / (sum / static_cast<double>(count));
    return std::clamp(static_cast<int>(std::lround((1.0 - flatness) * 32.0)), 0, 31);
}

// Everything from the sync word to the end of the last block: the whole
// frame bar padding and the tail. Silence and real audio go through this one
// function, so the two can never drift apart on field placement.
void emit_frame(BitWriter& w, const FrameConfig& config, std::uint32_t words,
                const Payload& payload, std::span<const std::byte> metadata = {}) {
    const int nfchans = fullbw_channel_count(config.acmod);
    const bool dependent = config.strmtyp == StreamType::kDependent;
    const auto& cpl = payload.cpl;
    const auto& spx = payload.spx;
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
    // the others must clear it. That leaves no way to signal real heavy
    // compression from a dependent, so the word it drags in stays 0x00 (unity,
    // §7.7.2.2) and only the independent substream carries a live compr.
    const bool compre = dependent ? config.last_dependent : payload.compr.has_value();
    w.put(compre ? 1 : 0, 1);
    if (compre) {
        w.put(dependent ? meta::kComprUnity : *payload.compr, 8);
    }
    // Annex E Table E1.2: unconditional on strmtyp, unlike chanmape below -
    // a dependent substream coding 1+1 would need its own Ch2 metadata too,
    // though this encoder's own callers never build one (dual mono has no
    // bed/dependent split to make - it is one independent substream, always).
    if (config.acmod == Acmod::kDualMono) {
        w.put(static_cast<std::uint32_t>(*config.dialnorm2), 5);
        const bool compre2 = !dependent && payload.compr2.has_value();
        w.put(compre2 ? 1 : 0, 1);
        if (compre2) {
            w.put(*payload.compr2, 8);
        }
    }
    if (dependent) {
        w.put(config.chanmap ? 1 : 0, 1);  // chanmape
        if (config.chanmap) {
            w.put(*config.chanmap, 16);
        }
    }
    // --- mixmdate (Table E1.2) ---
    // Every field inside is conditional on THIS substream's acmod and lfeon,
    // not the programme's: a dependent coding 2/2 has no centre channel, so it
    // writes no centre mix level even though the programme has one.
    const auto acmod_value = static_cast<std::uint8_t>(config.acmod);
    w.put(config.mixing ? 1 : 0, 1);  // mixmdate
    if (config.mixing) {
        const auto& mix = *config.mixing;
        if (acmod_value > 0x2) {
            w.put(static_cast<std::uint32_t>(mix.dmixmod), 2);
        }
        if ((acmod_value & 0x1) != 0 && acmod_value > 0x2) {
            w.put(static_cast<std::uint32_t>(mix.ltrtcmixlev), 3);
            w.put(static_cast<std::uint32_t>(mix.lorocmixlev), 3);
        }
        if ((acmod_value & 0x4) != 0) {
            w.put(static_cast<std::uint32_t>(mix.ltrtsurmixlev), 3);
            w.put(static_cast<std::uint32_t>(mix.lorosurmixlev), 3);
        }
        if (config.lfe) {
            w.put(mix.lfemixlevcod ? 1 : 0, 1);  // lfemixlevcode
            if (mix.lfemixlevcod) {
                w.put(static_cast<std::uint32_t>(*mix.lfemixlevcod), 5);
            }
        }
        // The rest of the group is gated on strmtyp == 0x0: programme scale,
        // the mixing-parameter block, pan information and the per-block mixing
        // configuration all describe how to combine this programme with
        // ANOTHER one, which is an independent substream's business. A
        // dependent therefore stops after the levels above.
        if (!dependent) {
            w.put(0, 1);  // pgmscle:    §E2.3.1.12, absent means 0 dB
            if (acmod_value == 0x0) {
                w.put(0, 1);  // pgmscl2e: mirrors pgmscle - no scale sent
            }
            w.put(0, 1);  // extpgmscle: §E2.3.1.16, absent means 0 dB
            w.put(0, 2);  // mixdef:     no mixing-parameter data
            if (acmod_value < 0x2) {
                w.put(0, 1);  // paninfoe
                if (acmod_value == 0x0) {
                    w.put(0, 1);  // paninfo2e: mirrors paninfoe - no pan sent
                }
            }
            w.put(0, 1);  // frmmixcfginfoe
        }
    }
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
    w.put(payload.ahte ? 1 : 0, 1);
    w.put(kSnroffststr, 2);
    w.put(kTransproce, 1);
    w.put(kBlkswe, 1);
    w.put(kDithflage, 1);
    w.put(kBamode, 1);
    w.put(kFrmfgaincode, 1);
    w.put(kDbaflde, 1);
    w.put(static_cast<std::uint32_t>(skipflde), 1);
    w.put(spx.atten ? 1 : 0, 1);  // spxattene

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
    // §E2.2.3's AHT block. Each flag exists only where the channel's exponents
    // are transmitted exactly once in the frame - ncplregs, nchregs[ch] and
    // nlferegs all 1 - because AHT spans the whole frame and cannot straddle a
    // change of exponent set. Table E2.10 code 0 (D15 then reuse) is that
    // shape by construction, and coupling additionally has to be in use for
    // all six blocks, which this encoder's all-or-nothing coupling guarantees.
    if (payload.ahte) {
        if (cpl.in_use) {
            w.put(payload.chans.back().aht ? 1 : 0, 1);  // cplahtinu
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(payload.chans[static_cast<std::size_t>(ch)].aht ? 1 : 0, 1);
        }
        if (config.lfe) {
            w.put(payload.chans[static_cast<std::size_t>(nfchans)].aht ? 1 : 0, 1);
        }
    }
    // snroffststr == 0: the SNR offsets live here, once for the frame, and
    // every channel inherits them. Zero for both means §7.2.2.1.1 gives an
    // all-zero allocation, hence no mantissa data at all.
    w.put(static_cast<std::uint32_t>(payload.csnroffst), 6);  // frmcsnroffst
    w.put(static_cast<std::uint32_t>(payload.fsnroffst), 4);  // frmfsnroffst
    // transproce == 0 contributes nothing. The attenuation codes are
    // per channel and frame-constant, which is why they live here and not in
    // the blocks.
    if (spx.atten) {
        for (int ch = 0; ch < nfchans; ++ch) {
            const int code = spx.attencod[static_cast<std::size_t>(ch)];
            w.put(code >= 0 ? 1 : 0, 1);  // chinspxatten[ch]
            if (code >= 0) {
                w.put(static_cast<std::uint32_t>(code), 5);  // spxattencod[ch]
            }
        }
    }
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
        // Same persistence rule as AC-3 (§7.7.1.2): resend only on a change,
        // always send in block 0. Unlike almost everything else in Annex E,
        // dynrnge is NOT hoisted to a frame-level flag - block resolution is
        // the whole point of dynrng, so it stays per block.
        const bool send_dynrng =
            config.drc.has_value() &&
            (first || payload.dynrng[static_cast<std::size_t>(blk)] !=
                          payload.dynrng[static_cast<std::size_t>(blk) - 1]);
        w.put(send_dynrng ? 1 : 0, 1);  // dynrnge
        if (send_dynrng) {
            w.put(payload.dynrng[static_cast<std::size_t>(blk)], 8);
        }
        if (config.acmod == Acmod::kDualMono) {
            const bool send_dynrng2 =
                config.drc.has_value() &&
                (first || payload.dynrng2[static_cast<std::size_t>(blk)] !=
                              payload.dynrng2[static_cast<std::size_t>(blk) - 1]);
            w.put(send_dynrng2 ? 1 : 0, 1);  // dynrng2e
            if (send_dynrng2) {
                w.put(payload.dynrng2[static_cast<std::size_t>(blk)], 8);
            }
        }

        // Spectral extension strategy: block 0 has spxstre implied, later
        // blocks send it explicitly. The strategy is set once a frame, so
        // those later blocks all say "reuse".
        if (first) {
            w.put(spx.in_use ? 1 : 0, 1);  // spxinu
            if (spx.in_use) {
                // 1/0 is the one mode where chinspx is not transmitted.
                if (config.acmod != Acmod::k1_0) {
                    for (int ch = 0; ch < nfchans; ++ch) {
                        w.put(1, 1);  // chinspx[ch]
                    }
                }
                w.put(static_cast<std::uint32_t>(spx.strtf), 2);
                w.put(static_cast<std::uint32_t>(spx.begf), 3);
                w.put(static_cast<std::uint32_t>(spx.endf), 3);
                w.put(1, 1);  // spxbndstrce: sent, for the same reason as cpl
                for (int sbnd = spx.begin_subbnd + 1; sbnd < spx.end_subbnd; ++sbnd) {
                    w.put(spx.structure[static_cast<std::size_t>(sbnd)] ? 1 : 0, 1);
                }
            }
        } else {
            w.put(0, 1);  // spxstre: keep the strategy from block 0
        }

        // Spectral extension coordinates, which precede the COUPLING strategy
        // rather than following it - the two tools interleave in audblk.
        if (spx.in_use) {
            const bool send = spx.send[static_cast<std::size_t>(blk)];
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!first) {
                    w.put(send ? 1 : 0, 1);  // spxcoe[ch]
                }
                if (send) {
                    const auto at = static_cast<std::size_t>(blk) *
                                        static_cast<std::size_t>(nfchans) +
                                    static_cast<std::size_t>(ch);
                    w.put(static_cast<std::uint32_t>(spx.blend[at]), 5);   // spxblnd
                    w.put(static_cast<std::uint32_t>(spx.master[at]), 2);  // mstrspxco
                    for (int bnd = 0; bnd < spx.bands.count; ++bnd) {
                        const auto coordinate =
                            spx.coords[at * static_cast<std::size_t>(spx.bands.count) +
                                       static_cast<std::size_t>(bnd)];
                        w.put(coordinate.exp, 4);
                        w.put(coordinate.mant, 2);
                    }
                }
            }
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
            // §E3.3.1: with spectral extension in use cplendf is derived from
            // spxbegf rather than transmitted, so that the coupling region
            // ends exactly where synthesis begins.
            if (!spx.in_use) {
                w.put(static_cast<std::uint32_t>(cpl.endf), 4);
            }
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
                for (int band = 0; band < rematrix_band_count(cpl, spx); ++band) {
                    w.put(0, 1);  // rematflg
                }
            }
        }

        // chbwcod accompanies a fresh exponent strategy, but only for a
        // channel carrying its own high band: a coupled or extended channel's
        // bandwidth is fixed by where that tool takes over, and sending
        // chbwcod anyway would both waste the bits and desynchronise the block.
        if (first) {
            if (!cpl.in_use && !spx.in_use) {
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
        // dbaflde == 0: no delta allocation. The skip field, when switched on,
        // sits here - after the delta bit allocation fields and before the
        // mantissas. Getting that order wrong does not fail to parse; it
        // shifts every mantissa in the block, which comes back as noise.
        if (skipflde != 0) {
            put_skip_field(w, blk == kMetadataBlock ? metadata
                                                    : std::span<const std::byte>{});
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
    // §E2.3.1.3: frmsiz is an arbitrary 11-bit word count rather than an
    // index into Table 5.18 the way AC-3's frmsizecod is, so unlike AC-3 any
    // bitrate that lands on a legal word count is expressible here - not
    // only the 19 nominal Table 5.18 rates. bitrate_kbps == 0 gives
    // frame_words() == 0, which is not a syncframe at all; past
    // kMaxFrameWords the word count overflows frmsiz's 11 bits.
    const auto words = frame_words(config.sample_rate, config.bitrate_kbps);
    if (words < 1 || words > kMaxFrameWords) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    if (config.acmod == Acmod::kDualMono &&
        (!config.dialnorm2 || *config.dialnorm2 < 1 || *config.dialnorm2 > 31)) {
        return std::unexpected(FrameError::kInvalidDialnorm);
    }
    if (config.substreamid < 0 || config.substreamid > 7) {
        return std::unexpected(FrameError::kInvalidSubstream);
    }
    // strmtyp 0x2 needs the blkid/frmsizecod branch of Table E1.2 that emit_frame
    // does not write, and 0x3 is reserved. Both would produce a frame whose
    // header promises fields the payload does not contain.
    if (config.strmtyp != StreamType::kIndependent &&
        config.strmtyp != StreamType::kDependent) {
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
    // §E3.8.5 owns a dependent substream's compre, so heavy compression there
    // would either be ignored or break the end-of-programme marker.
    if (config.heavy && config.strmtyp != StreamType::kIndependent) {
        return std::unexpected(FrameError::kInvalidSubstream);
    }
    if (config.mixing) {
        const auto& mix = *config.mixing;
        // Tables D2.4 / D2.6 reserve the three loudest surround codes, and a
        // decoder that receives one substitutes 0.841 - so writing one means
        // the level applied is not the level asked for.
        if (!meta::valid_surround_mix_level(mix.ltrtsurmixlev) ||
            !meta::valid_surround_mix_level(mix.lorosurmixlev)) {
            return std::unexpected(FrameError::kInvalidMixLevel);
        }
        if (mix.lfemixlevcod && (*mix.lfemixlevcod < 0 || *mix.lfemixlevcod > 31)) {
            return std::unexpected(FrameError::kInvalidMixLevel);
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
                        payload, aux);
}

FrameEncoder::FrameEncoder(const FrameConfig& config) : config_(config) {
    if (config_.drc) {
        range_.emplace(*config_.drc, config_.sample_rate);
        if (config_.acmod == Acmod::kDualMono) {
            range2_.emplace(*config_.drc, config_.sample_rate);
        }
    }
    if (config_.heavy) {
        heavy_.emplace(*config_.heavy, config_.sample_rate);
        if (config_.acmod == Acmod::kDualMono) {
            heavy2_.emplace(*config_.heavy, config_.sample_rate);
        }
    }
}

namespace {

// The §7.7 words a substream would choose for itself, from its own channels.
// Also the access-unit measurement, since an access unit measures the
// independent substream.
FrameMetadata derive_metadata(const FrameConfig& config,
                              std::span<const std::array<double, 256>> history,
                              std::span<const std::span<const float>> channels,
                              std::optional<meta::RangeController>& range,
                              std::optional<meta::HeavyCompressor>& heavy,
                              std::optional<meta::RangeController>* range2 = nullptr,
                              std::optional<meta::HeavyCompressor>* heavy2 = nullptr) {
    const bool dual_mono = config.acmod == Acmod::kDualMono;
    const int nfchans = fullbw_channel_count(config.acmod);
    FrameMetadata out;
    out.dynrng.fill(meta::kDynrngUnity);
    out.dynrng2.fill(meta::kDynrngUnity);
    if (range) {
        std::array<std::span<const float>, 5> block_view{};
        const int level_chans = dual_mono ? 1 : nfchans;
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            for (int ch = 0; ch < level_chans; ++ch) {
                block_view[static_cast<std::size_t>(ch)] =
                    channels[static_cast<std::size_t>(ch)].subspan(
                        static_cast<std::size_t>(blk) * kSamplesPerBlock, kSamplesPerBlock);
            }
            const double level = meta::level_dbfs(
                std::span{block_view}.first(static_cast<std::size_t>(level_chans)));
            out.dynrng[static_cast<std::size_t>(blk)] = range->next(level, config.dialnorm);
        }
    }
    if (dual_mono && range2 && *range2) {
        std::array<std::span<const float>, 1> block_view{};
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            block_view[0] = channels[1].subspan(
                static_cast<std::size_t>(blk) * kSamplesPerBlock, kSamplesPerBlock);
            const double level = meta::level_dbfs(std::span{block_view});
            out.dynrng2[static_cast<std::size_t>(blk)] =
                (*range2)->next(level, *config.dialnorm2);
        }
    }
    if (heavy) {
        // With no mixmdate the §7.8 fallbacks stand in - the same intermediate
        // levels §5.4.2.4 and §5.4.2.5 tell a decoder to substitute. Dual mono
        // has no downmix to fall back on in the first place - §7.7.2.2 bounds
        // Ch1's own signal - so its true peak is measured directly instead.
        const double peak =
            dual_mono
                ? meta::channel_peak_dbfs(std::span<const double>(history[0]), channels[0])
                : [&] {
                      const double clev = config.mixing
                                              ? meta::coefficient(config.mixing->lorocmixlev)
                                              : meta::level::kMinus4_5dB;
                      const double slev = config.mixing
                                              ? meta::coefficient(config.mixing->lorosurmixlev)
                                              : meta::level::kMinus6dB;
                      return meta::mono_downmix_peak_dbfs(
                          history, channels.first(static_cast<std::size_t>(nfchans)),
                          config.acmod, clev, slev);
                  }();
        out.compr = heavy->next(peak, config.dialnorm);
    }
    if (dual_mono && heavy2 && *heavy2) {
        const double peak2 =
            meta::channel_peak_dbfs(std::span<const double>(history[1]), channels[1]);
        out.compr2 = (*heavy2)->next(peak2, *config.dialnorm2);
    }
    return out;
}

}  // namespace

std::expected<std::vector<std::byte>, FrameError> FrameEncoder::encode_frame(
    std::span<const std::span<const float>> channels, AuxPayload aux) {
    if (const auto ok = validate(config_); !ok) {
        return std::unexpected(ok.error());
    }
    const int nfchans = fullbw_channel_count(config_.acmod);
    return encode_frame(
        channels,
        derive_metadata(config_, std::span{history_}.first(static_cast<std::size_t>(nfchans)),
                        channels, range_, heavy_, &range2_, &heavy2_),
        aux);
}

std::expected<std::vector<std::byte>, FrameError> FrameEncoder::encode_frame(
    std::span<const std::span<const float>> channels, const FrameMetadata& metadata,
    AuxPayload aux) {
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

    // --- 1. Tool decisions --------------------------------------------------
    // Spectral extension is settled first, because when both tools are in use
    // it fixes where coupling has to stop (§E3.3.1).
    Payload payload;
    // §7.7 dynamic range, carried in before the side information is sized: a
    // transmitted dynrng costs nine bits and the SNR search spends what is
    // left. §E3.8.5 gives a DEPENDENT substream's compre to the
    // end-of-programme marker instead, so a heavy-compression word cannot
    // travel there whatever the caller asked for.
    payload.dynrng = metadata.dynrng;
    if (config_.strmtyp == StreamType::kIndependent) {
        payload.compr = metadata.compr;
    }
    payload.dynrng2 = metadata.dynrng2;
    if (config_.strmtyp == StreamType::kIndependent) {
        payload.compr2 = metadata.compr2;
    }
    auto& cpl = payload.cpl;
    auto& spx = payload.spx;
    spx.in_use = config_.spx;
    if (spx.in_use) {
        spx.begf = std::clamp(config_.spxbegf >= 0
                                  ? config_.spxbegf
                                  : default_spxbegf(config_.bitrate_kbps, nfchans),
                              0, 7);
        // Synthesis runs to sub-band 17, coefficient 229 - 21.5 kHz at 48 kHz.
        // Nothing is coded or synthesized above it, which is a bandwidth no
        // listener is going to miss and a table entry that exists for exactly
        // this purpose.
        spx.endf = 7;
        spx.begin_subbnd = spx_begin_subbnd(spx.begf);
        spx.end_subbnd = spx_end_subbnd(spx.endf);
        spx.startmant = spx_band_start(spx.begin_subbnd);
        spx.endmant = spx_band_start(spx.end_subbnd);
        spx.strtf = default_spxstrtf(spx.startmant);
        spx.copystart = spx_band_start(spx.strtf);
        spx.structure = kDefaultSpxBandStructure;
        spx.bands = group_bands(
            spx.startmant, spx.end_subbnd - spx.begin_subbnd, kSpxBinsPerSubBand,
            std::span{spx.structure}.subspan(static_cast<std::size_t>(spx.begin_subbnd)));
        // The coordinates cannot be computed until the baseband has been
        // quantized, but their SIZE is fixed now - and the side-information
        // probe below needs that size - so the arrays are laid out here and
        // filled in at the end.
        const auto slots = static_cast<std::size_t>(kBlocksPerFrame) *
                           static_cast<std::size_t>(nfchans);
        spx.blend.assign(slots, 0);
        spx.master.assign(slots, 0);
        spx.coords.assign(slots * static_cast<std::size_t>(spx.bands.count), {});
        // Which channels attenuate is a size question - chinspxatten gates a
        // 5-bit field - so it is settled here, before the side information is
        // measured. The depth itself is not, and could be refined later.
        spx.atten = config_.spx_atten;
        spx.attencod.assign(static_cast<std::size_t>(nfchans),
                            spx.atten ? std::clamp(config_.spxattencod >= 0
                                                       ? config_.spxattencod
                                                       : kDefaultSpxAttenCod,
                                                   0, kSpxAttenCodes - 1)
                                      : -1);
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            spx.send[static_cast<std::size_t>(blk)] = blk % 2 == 0;
        }
    }

    // §E2.2.3 gates the whole coupling element on acmod > 0x1, so 1/0 and the
    // rejected 1+1 cannot couple however the caller asks.
    cpl.in_use = config_.coupling && static_cast<std::uint8_t>(config_.acmod) > 0x1;
    if (cpl.in_use) {
        cpl.begf = std::clamp(config_.cplbegf >= 0
                                  ? config_.cplbegf
                                  : default_cplbegf(config_.bitrate_kbps, nfchans),
                              0, 15);
        // Without spectral extension, coupling runs to the top of the coded
        // spectrum: chbwcod is gone for a coupled channel, so the coupling end
        // frequency IS its bandwidth and stopping short discards the band
        // rather than saving its bits.
        cpl.endf = 15;
        if (spx.in_use) {
            // §E3.3.1 derives cplendf from spxbegf and stops transmitting it.
            // The value may be negative, which is legal because it is never
            // sent - but it can leave no room for coupling at all, and it can
            // leave less room than the requested cplbegf wants.
            cpl.endf = derived_cplendf(spx.begf);
            if (cpl.endf + 2 < 0) {
                cpl.in_use = false;  // synthesis starts below where coupling could
            } else {
                cpl.begf = std::min(cpl.begf, cpl.endf + 2);
            }
        }
    }
    if (cpl.in_use) {
        cpl.strtmant = kCplFirstBin + kCplBinsPerSubBand * cpl.begf;
        cpl.endmant = kCplFirstBin + kCplBinsPerSubBand * (cpl.endf + 3);
        cpl.nsubnd = 3 + cpl.endf - cpl.begf;
        assert(cpl.nsubnd >= 1);
        assert(!spx.in_use || cpl.endmant == spx.startmant);
        std::copy_n(kDefaultCplBandStructure.begin(), cpl.nsubnd, cpl.structure.begin());
        cpl.bands = group_bands(cpl.strtmant, cpl.nsubnd, kCplBinsPerSubBand,
                                std::span{cpl.structure});
    }

    // §E3.3.3: whichever tool takes over first sets the coded bandwidth.
    const int fbw_endmant = cpl.in_use    ? cpl.strtmant
                            : spx.in_use  ? spx.startmant
                                          : ((config_.chbwcod + 12) * 3) + 37;
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

    // --- 4. Which streams take the adaptive hybrid transform ---------------
    // AHT is worth having exactly when the six blocks look alike, because
    // that is when the DCT down each bin collapses them into one large
    // coefficient and five small ones. On a transient it does the opposite -
    // one loud block spreads across all six - and it cannot be undone for
    // part of a frame, so the decision is per channel per frame and the test
    // is whether the block energies are within an order of magnitude.
    payload.chans.resize(static_cast<std::size_t>(streams));
    for (int s = 0; s < streams && config_.aht; ++s) {
        auto& plan = payload.chans[static_cast<std::size_t>(s)];
        std::array<double, kBlocksPerFrameSize> energy{};
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            for (int bin = stream_start(s); bin < stream_end(s); ++bin) {
                const double value = coeffs_at(s, blk)[static_cast<std::size_t>(bin)];
                energy[static_cast<std::size_t>(blk)] += value * value;
            }
        }
        const double peak = *std::ranges::max_element(energy);
        const double quietest = *std::ranges::min_element(energy);
        // Silence is stationary, and its coefficients are all zero, so the
        // transform costs nothing either way.
        plan.aht = !(peak > 0.0) || peak <= kAhtStationaryRatio * quietest;
        payload.ahte = payload.ahte || plan.aht;
    }

    // --- 5. Fixed point and one frame-constant exponent set per stream -----
    // Table E2.10 code 0 sends D15 in block 0 and reuses it for the other
    // five, so a bin's exponent has to accommodate its LOUDEST block. The
    // smallest exponent across the frame is that bin's worst case; anything
    // larger would overflow the mantissa in the block that peaks.
    //
    // Under AHT the axis changes. The values the quantizers see are no longer
    // the six blocks' MDCT coefficients but the six DCT coefficients taken
    // down the bin, and §E3.4.5 has the decoder apply the exponent AFTER
    // inverting that DCT - so the transform output IS the mantissa, and the
    // exponent has to normalise IT. Normalising the MDCT coefficients instead
    // leaves the AHT mantissas about sqrt(12) small, which the scalar
    // quantizers merely waste headroom on but the vector quantizers cannot
    // survive: their codebooks are fixed-magnitude direction vectors with
    // components reaching full scale, so a bin presented at a third of full
    // scale comes back at three times its own level. Measured on the
    // reference program, that cost 46 dB of the vector range's SNR while the
    // scalar range sat at a comfortable 33.
    std::vector<std::array<std::int32_t, 256>> fixed(
        static_cast<std::size_t>(streams) * kBlocksPerFrame);
    const auto fixed_at = [&](int s, int blk) -> std::array<std::int32_t, 256>& {
        return fixed[static_cast<std::size_t>(s) * kBlocksPerFrame +
                     static_cast<std::size_t>(blk)];
    };
    for (int s = 0; s < streams; ++s) {
        auto& plan = payload.chans[static_cast<std::size_t>(s)];
        plan.start = stream_start(s);
        plan.endmant = stream_end(s);
        const auto span = static_cast<std::size_t>(plan.endmant - plan.start);
        std::vector<std::uint8_t> raw(span, kMaxExponent);
        std::vector<std::uint8_t> axis_exps(span, 0);

        if (plan.aht) {
            plan.aht_fixed.assign(static_cast<std::size_t>(plan.endmant), {});
            plan.aht_coeffs.assign(static_cast<std::size_t>(plan.endmant), {});
            std::vector<std::int32_t> column(span);
            for (int bin = plan.start; bin < plan.endmant; ++bin) {
                std::array<double, kBlocksPerFrameSize> blocks{};
                for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
                    blocks[static_cast<std::size_t>(blk)] =
                        coeffs_at(s, blk)[static_cast<std::size_t>(bin)];
                }
                std::array<double, kBlocksPerFrameSize> transformed{};
                aht_forward(blocks, transformed);
                for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
                    plan.aht_fixed[static_cast<std::size_t>(bin)][j] =
                        to_fixed25(transformed[j]);
                }
            }
            // The same "worst case wins" rule as below, down the transform
            // axis instead of the block axis.
            for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
                for (std::size_t bin = 0; bin < span; ++bin) {
                    column[bin] =
                        plan.aht_fixed[bin + static_cast<std::size_t>(plan.start)][j];
                }
                extract_exponents(column, axis_exps);
                for (std::size_t bin = 0; bin < span; ++bin) {
                    raw[bin] = std::min(raw[bin], axis_exps[bin]);
                }
            }
        } else {
            for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
                const auto& source = coeffs_at(s, blk);
                auto& out = fixed_at(s, blk);
                for (int bin = plan.start; bin < plan.endmant; ++bin) {
                    out[static_cast<std::size_t>(bin)] =
                        to_fixed25(source[static_cast<std::size_t>(bin)]);
                }
                extract_exponents(
                    std::span{out}.subspan(static_cast<std::size_t>(plan.start), span),
                    axis_exps);
                for (std::size_t bin = 0; bin < span; ++bin) {
                    raw[bin] = std::min(raw[bin], axis_exps[bin]);
                }
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
        if (plan.aht) {
            // The mantissas the quantizers see, normalised by each bin's own
            // exponent. They have to exist before the rate search, because
            // under GAQ the search cannot size the frame without quantizing.
            plan.aht_gain.assign(static_cast<std::size_t>(plan.endmant), 1);
            for (int bin = plan.start; bin < plan.endmant; ++bin) {
                const auto at = static_cast<std::size_t>(bin);
                const int exp = plan.decoded[at];
                for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
                    plan.aht_coeffs[at][j] =
                        std::ldexp(static_cast<double>(plan.aht_fixed[at][j]), exp - 24);
                }
            }
        }
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

    std::vector<std::span<const std::uint8_t>> bap_views;
    bap_views.reserve(static_cast<std::size_t>(streams));
    const auto bits_at = [&](int composite) {
        bap_views.clear();
        std::uint32_t aht_bits = 0;
        for (int s = 0; s < streams; ++s) {
            auto& plan = payload.chans[static_cast<std::size_t>(s)];
            // Every stream shares one fsnroffst, so the frame-wide
            // §7.2.2.1.1 condition reduces to the composite being zero.
            const BitAllocRegion region{.start = plan.start,
                                        .coupling = s == cpl_stream,
                                        .cplfleak = cpl.fleak,
                                        .cplsleak = cpl.sleak,
                                        .snr_all_zero = composite == 0,
                                        .high_efficiency = plan.aht};
            compute_bit_allocation(plan.decoded, config_.sample_rate, kBamode0Codes,
                                   composite >> 4, composite & 15, plan.bap, region);
            if (plan.aht) {
                // An AHT stream's cost is a whole-frame figure: six blocks of
                // one bin become one VQ index or six scalar mantissas, all
                // emitted in block 0. It never enters the per-block grouping.
                aht_bits += aht_stream_bits(plan, plan.gaqmod);
                continue;
            }
            // Only the stream's own region carries mantissas.
            bap_views.push_back(
                std::span{plan.bap}.subspan(static_cast<std::size_t>(plan.start)));
        }
        // Every block reuses the same exponents, hence the same allocation.
        return static_cast<std::uint32_t>(mantissa_bits_per_block(bap_views)) *
                   kBlocksPerFrame +
               aht_bits;
    };

    const auto search = [&] {
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
        return lo;
    };
    int lo = search();

    // Choosing the gain mode needs an allocation to choose against, and the
    // allocation needs a rate that depends on the mode - so the search runs
    // twice, picking each AHT stream's cheapest mode at the provisional
    // offset in between. A third pass buys nothing measurable: the modes
    // differ by a few per cent of the mantissa budget, which never moves the
    // offset far enough to change which mode wins.
    if (payload.ahte && config_.gaqmod != 0) {
        bits_at(lo);  // leaves every stream's allocation at the provisional offset
        for (int s = 0; s < streams; ++s) {
            auto& plan = payload.chans[static_cast<std::size_t>(s)];
            if (!plan.aht) {
                continue;
            }
            if (config_.gaqmod > 0) {
                plan.gaqmod = std::min(config_.gaqmod, 3);
                continue;
            }
            std::uint32_t best = aht_stream_bits(plan, 0);
            for (const int mode : {1, 2, 3}) {
                const std::uint32_t bits = aht_stream_bits(plan, mode);
                if (bits < best) {
                    best = bits;
                    plan.gaqmod = mode;
                }
            }
        }
        lo = search();
    }
    // The call is not optional: it is what leaves payload.bap holding the
    // allocation for `lo`, which every mantissa below is quantised against.
    // Only its RESULT is debug-only - checked here and against the tokens
    // actually written at the end of the function - so the variable is
    // unreferenced under NDEBUG while the call still has to happen. Folding it
    // into the assert would delete the allocation along with the check.
    [[maybe_unused]] const std::uint32_t mantissa_bits = bits_at(lo);
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
            auto& plan = payload.chans[static_cast<std::size_t>(s)];
            if (plan.aht) {
                // §E2.2.4: an AHT stream's mantissas are read once, in the
                // first block that carries them, and the decoder then marks
                // it done - so blocks 1 to 5 emit NOTHING for this stream.
                if (blk != 0) {
                    return;
                }
                writer.add_raw(static_cast<std::uint32_t>(plan.gaqmod), 2);
                // The gain words come first, all of them, before any
                // mantissa - the decoder needs them to know how long the
                // mantissas that follow are.
                if (plan.gaqmod != 0) {
                    std::vector<int> gains;
                    for (int bin = plan.start; bin < plan.endmant; ++bin) {
                        const auto at = static_cast<std::size_t>(bin);
                        if (aht_gaq_has_gain(plan.bap[at], plan.gaqmod)) {
                            gains.push_back(plan.aht_gain[at]);
                        }
                    }
                    if (plan.gaqmod == 3) {
                        // Table E3.4: three three-state gains to a 5-bit word,
                        // most significant first. A short final triplet is
                        // padded with unity, which costs a whole word either
                        // way - aht_gaq_sections counts it that way too.
                        for (std::size_t i = 0; i < gains.size(); i += 3) {
                            std::uint32_t packed = 0;
                            for (std::size_t t = 0; t < 3; ++t) {
                                const int gain = i + t < gains.size() ? gains[i + t] : 1;
                                packed = packed * 3 +
                                         static_cast<std::uint32_t>(aht_gaq_mapped(gain));
                            }
                            writer.add_raw(packed, 5);
                        }
                    } else {
                        // Modes 1 and 2 have only two gains to distinguish, so
                        // the bit is a plain flag rather than Table E3.4's
                        // mapping - 1 means "this mode's other gain".
                        for (const int gain : gains) {
                            writer.add_raw(gain == 1 ? 0u : 1u, 1);
                        }
                    }
                }
                for (int bin = plan.start; bin < plan.endmant; ++bin) {
                    const auto at = static_cast<std::size_t>(bin);
                    const int hebap = plan.bap[at];
                    auto& values = plan.aht_coeffs[at];
                    if (hebap == 0) {
                        values.fill(0.0);  // what the decoder will hold here
                        continue;
                    }
                    if (hebap <= 7) {
                        // One index for all six blocks of this bin.
                        const int index = aht_vector_quantize(values, hebap);
                        writer.add_raw(static_cast<std::uint32_t>(index),
                                       aht_bin_bits(hebap));
                        continue;
                    }
                    const int hebap_mantissa_bits = aht_mantissa_bits(hebap);
                    for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
                        const auto code =
                            aht_quantize_mantissa(values[j], hebap_mantissa_bits,
                                                  plan.aht_gain[at]);
                        writer.add_raw(code.code, code.bits);
                        if (code.escape_bits > 0) {
                            writer.add_raw(code.escape, code.escape_bits);
                        }
                        values[j] = code.recon;
                    }
                }
                return;
            }
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

    // --- 9. Spectral extension coordinates ----------------------------------
    // Last, because the gains have to be measured against what the DECODER
    // will hold, not against what the encoder started with. The copy source is
    // the baseband this function has just quantized, and at low rates a good
    // part of that baseband has bap 0 and reconstructs to exactly zero - so
    // measuring against the original coefficients would ask for gains that
    // scale silence. Nothing about the frame's SIZE depends on these values,
    // only on how many there are, so computing them here is free.
    if (spx.in_use) {
        const auto spx_nbnd = static_cast<std::size_t>(spx.bands.count);
        std::vector<double> recon(static_cast<std::size_t>(spx.startmant), 0.0);
        std::vector<double> gains(spx_nbnd, 0.0);
        std::vector<double> synth(static_cast<std::size_t>(spx.endmant - spx.startmant), 0.0);
        std::vector<double> band_rms(spx_nbnd, 0.0);
        // The decoder's own reconstruction: quantize, dequantize, undo the
        // exponent. bap 0 with dither off is exactly zero, which is the case
        // that matters.
        const auto rebuild = [&](int s, int blk, int from, int to) {
            const auto& plan = payload.chans[static_cast<std::size_t>(s)];
            if (plan.aht) {
                // The AHT path already holds its reconstructed coefficients,
                // quantized by step 8; undoing the DCT and the exponent gives
                // the same bins the scalar path produces.
                for (int bin = from; bin < to; ++bin) {
                    std::array<double, kBlocksPerFrameSize> blocks{};
                    aht_inverse(plan.aht_coeffs[static_cast<std::size_t>(bin)], blocks);
                    recon[static_cast<std::size_t>(bin)] =
                        std::ldexp(blocks[static_cast<std::size_t>(blk)],
                                   -plan.decoded[static_cast<std::size_t>(bin)]);
                }
                return;
            }
            const auto& block = fixed_at(s, blk);
            for (int bin = from; bin < to; ++bin) {
                const int bap = plan.bap[static_cast<std::size_t>(bin)];
                if (bap == 0) {
                    // No bits, and dithflag is 0, so the decoder holds exactly
                    // zero here. This is the case that makes the whole
                    // reconstruction worth doing rather than reusing the
                    // encoder's own coefficients.
                    recon[static_cast<std::size_t>(bin)] = 0.0;
                    continue;
                }
                const int exp = plan.decoded[static_cast<std::size_t>(bin)];
                const auto mantissa = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(block[static_cast<std::size_t>(bin)]) << exp);
                recon[static_cast<std::size_t>(bin)] =
                    std::ldexp(dequantize_mantissa(quantize_mantissa(mantissa, bap), bap),
                               -exp);
            }
        };

        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto at = coord_slot(blk, ch);
                if (!spx.send[static_cast<std::size_t>(blk)]) {
                    spx.blend[at] = spx.blend[coord_slot(blk - 1, ch)];
                    spx.master[at] = spx.master[coord_slot(blk - 1, ch)];
                    for (std::size_t bnd = 0; bnd < spx_nbnd; ++bnd) {
                        spx.coords[at * spx_nbnd + bnd] =
                            spx.coords[coord_slot(blk - 1, ch) * spx_nbnd + bnd];
                    }
                    continue;
                }
                // The blend factor is settled first: the gains below have to
                // know how much of each band will be noise.
                spx.blend[at] = spx_blend(
                    std::span{coeffs_at(ch, blk)}
                        .subspan(static_cast<std::size_t>(spx.startmant),
                                 static_cast<std::size_t>(spx.endmant - spx.startmant)));
                rebuild(ch, blk, 0, payload.chans[static_cast<std::size_t>(ch)].endmant);
                // With coupling below the extension region, part of the copy
                // source is not this channel's own coded data at all - it is
                // the shared channel scaled by this channel's coordinate.
                if (cpl.in_use) {
                    rebuild(cpl_stream, blk, cpl.strtmant, cpl.endmant);
                    for (int bnd = 0; bnd < cpl.bands.count; ++bnd) {
                        const double coord = coupling::decode_coordinate(
                            cpl.coords[at * nbnd + static_cast<std::size_t>(bnd)],
                            cpl.master[at]);
                        const int low = cpl.bands.start[static_cast<std::size_t>(bnd)];
                        const int high = low + cpl.bands.size[static_cast<std::size_t>(bnd)];
                        for (int bin = low; bin < high; ++bin) {
                            recon[static_cast<std::size_t>(bin)] *= coord * 8.0;
                        }
                    }
                }

                // §E3.6.4.1: copy bands up from the source region, wrapping
                // back to its start whenever the next band would run past its
                // end. The decoder does exactly this, so the encoder measures
                // the energy of exactly the coefficients the decoder will get.
                // The translated band is materialised rather than just summed,
                // because the notch below has to be applied to it before its
                // energy means anything.
                int copyindex = spx.copystart;
                for (int bnd = 0; bnd < spx.bands.count; ++bnd) {
                    const int size = spx.bands.size[static_cast<std::size_t>(bnd)];
                    spx.wrapflag[static_cast<std::size_t>(bnd)] = false;
                    if (copyindex + size > spx.startmant) {
                        copyindex = spx.copystart;
                        spx.wrapflag[static_cast<std::size_t>(bnd)] = true;
                    }
                    double accum = 0.0;
                    const int low = spx.bands.start[static_cast<std::size_t>(bnd)];
                    for (int i = 0; i < size; ++i) {
                        if (copyindex == spx.startmant) {
                            copyindex = spx.copystart;
                        }
                        const double value = recon[static_cast<std::size_t>(copyindex++)];
                        synth[static_cast<std::size_t>(low - spx.startmant + i)] = value;
                        accum += value * value;
                    }
                    // §E3.6.4.2.2's banded RMS, taken BEFORE the notch - the
                    // noise is scaled by it, so the notch does not quieten the
                    // noise the way it quietens the copied signal.
                    band_rms[static_cast<std::size_t>(bnd)] =
                        std::sqrt(accum / size);
                }

                // §E3.6.4.2.3, after the banded RMS and before the blend.
                spx_apply_notch(synth, spx.startmant, spx.bands,
                                std::span{spx.wrapflag},
                                spx.atten ? spx.attencod[static_cast<std::size_t>(ch)]
                                          : -1);

                for (int bnd = 0; bnd < spx.bands.count; ++bnd) {
                    const int size = spx.bands.size[static_cast<std::size_t>(bnd)];
                    const int low = spx.bands.start[static_cast<std::size_t>(bnd)];
                    double target = 0.0;
                    for (int bin = low; bin < low + size; ++bin) {
                        const double value =
                            coeffs_at(ch, blk)[static_cast<std::size_t>(bin)];
                        target += value * value;
                    }
                    // What the decoder will actually hold once it has blended
                    // noise in. Without the notch this reduces to the
                    // translated band's own energy, because the noise carries
                    // that band's RMS and the two factors are complementary -
                    // but the notch quietens the signal side only, so once it
                    // is in play the blend has to be modelled outright.
                    const double ratio = spx_noise_ratio(spx, bnd, spx.blend[at]);
                    double blended = 0.0;
                    for (int i = 0; i < size; ++i) {
                        const double value =
                            synth[static_cast<std::size_t>(low - spx.startmant + i)];
                        blended += value * value * (1.0 - ratio);
                    }
                    blended += size * band_rms[static_cast<std::size_t>(bnd)] *
                               band_rms[static_cast<std::size_t>(bnd)] * ratio;
                    // The decoder applies the coordinate as spxco * 32.
                    gains[static_cast<std::size_t>(bnd)] =
                        blended > 0.0 ? std::sqrt(target / blended) / 32.0 : 0.0;
                }
                const int chosen = coupling::choose_master(gains);
                spx.master[at] = chosen;
                for (std::size_t bnd = 0; bnd < spx_nbnd; ++bnd) {
                    spx.coords[at * spx_nbnd + bnd] = coupling::quantize_coordinate(
                        gains[bnd], chosen, coupling::kSpxMantissaBits);
                }
            }
        }
    }

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
        // DRC is a property of the programme, not of a substream, so a
        // dependent carries the same profile whether or not the caller said
        // so - otherwise its channels would sit outside the compression its
        // siblings are inside. The words themselves come from one measurement;
        // this only settles whether the FIELDS are written.
        dep.drc = config.independent.drc;
        // Heavy compression never travels on a dependent (§E3.8.5), so clear
        // it rather than let validate() reject a config the caller could not
        // reasonably have known was illegal.
        dep.heavy = std::nullopt;
        out.push_back(dep);
    }
    for (const auto& sub : out) {
        if (const auto ok = validate(sub); !ok) {
            return std::unexpected(ok.error());
        }
    }
    // §E3.8.2 caps a single programme at 16 rendered channels. Each
    // substream's own chanmap-vs-acmod/lfeon agreement is checked above; this
    // is the aggregate the per-substream check cannot see, mirroring the
    // decoder's own union-and-count at decode time (eac3_decoder.cpp).
    std::uint16_t occupied = 0;
    for (const auto& sub : out) {
        occupied = static_cast<std::uint16_t>(
            occupied | (sub.chanmap ? *sub.chanmap : chanmap::acmod_map(sub.acmod, sub.lfe)));
    }
    if (chanmap::expand(occupied).count > 16) {
        return std::unexpected(FrameError::kTooManyChannels);
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
    // The substreams have controllers of their own, but this class always
    // supplies the words explicitly, so those never advance. These are the
    // ones that run.
    const bool dual_mono = config_.independent.acmod == Acmod::kDualMono;
    if (config_.independent.drc) {
        range_.emplace(*config_.independent.drc, config_.independent.sample_rate);
        if (dual_mono) {
            range2_.emplace(*config_.independent.drc, config_.independent.sample_rate);
        }
    }
    if (config_.independent.heavy) {
        heavy_.emplace(*config_.independent.heavy, config_.independent.sample_rate);
        if (dual_mono) {
            heavy2_.emplace(*config_.independent.heavy, config_.independent.sample_rate);
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

    // One measurement for the whole access unit, taken on the independent
    // substream's channels - they come first, and they are a self-sufficient
    // rendering of the programme.
    const auto independent_count =
        static_cast<std::size_t>(substreams_.front().channel_count());
    const auto independent_fbw =
        static_cast<std::size_t>(fullbw_channel_count(config_.independent.acmod));
    const FrameMetadata metadata =
        derive_metadata(config_.independent, std::span{tail_}.first(independent_fbw),
                        channels.first(independent_count), range_, heavy_, &range2_, &heavy2_);
    for (std::size_t ch = 0; ch < independent_fbw; ++ch) {
        for (int n = 0; n < kSamplesPerBlock; ++n) {
            tail_[ch][static_cast<std::size_t>(n)] = static_cast<double>(
                channels[ch][static_cast<std::size_t>(kSamplesPerFrame - kSamplesPerBlock + n)]);
        }
    }

    AccessUnit unit;
    std::size_t taken = 0;
    for (auto& sub : substreams_) {
        const auto count = static_cast<std::size_t>(sub.channel_count());
        // §8.2: the object metadata rides in the LAST substream of the access
        // unit, so a decoder has the whole programme in hand before it reads it.
        const bool carries_aux = &sub == &substreams_.back();
        const auto frame = sub.encode_frame(channels.subspan(taken, count), metadata,
                                            carries_aux ? aux : AuxPayload{});
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
