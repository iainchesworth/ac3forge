#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

#include "ac3/core/aht_tables.hpp"
#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/decoder/transient_prenoise.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/encoder/eac3_tools.hpp"

// E-AC-3 syncframe decoding, ATSC A/52:2018 Annex E Tables E1.2, E1.3 and E1.4.
//
// Annex E is not a variant of the AC-3 frame; it is a different container for
// the same coding tools. syncinfo is only the sync word, the frame size is
// stated outright, and everything that AC-3 decides per block - exponent
// strategies, coupling-in-use, the SNR offsets - can be hoisted into a
// frame-level audfrm element, which then makes several audblk fields
// conditional. That hoisting is why this cannot share decode_frame's loop:
// the two syntaxes agree only on the payload underneath.

namespace ac3 {

namespace {

using eac3::StreamType;

// A substream codes at most 3/2 plus LFE (Table 5.8).
constexpr int kMaxSubstreamChannels = 6;

// One more slot past the real channels for the shared coupling channel,
// mirroring how it rides alongside the fbw channels and LFE in the coded
// stream (§5.3.3). Channel indices never reach this far (nchans <= 6), so a
// single fixed slot at index kMaxSubstreamChannels never collides with a
// real channel, unlike AC-3's decoder which sizes its arrays dynamically.
constexpr int kCplStream = kMaxSubstreamChannels;
constexpr int kMaxSubstreamStreams = kMaxSubstreamChannels + 1;

// Table E1.4, the else-branch of if(bamode): with bamode == 0 the allocation
// parameters take THESE values. They are not the §8.2.12 basic-encoder
// recommendations AC-3 uses - floorcod is 0x7 here against §8.2.12's 4, which
// is what BitAllocCodes defaults to. floorcod sets the masking floor, so the
// wrong one changes every bap and therefore every block's mantissa bit count:
// block 1 onwards lands at the wrong bit offset and the frame decodes as
// noise. Silence cannot expose it, since zero SNR offsets make §7.2.2.1.1
// zero the allocation before floorcod is ever consulted.
constexpr BitAllocCodes kBamode0Codes{.sdcycod = 2,
                                      .fdcycod = 1,
                                      .sgaincod = 1,
                                      .dbpbcod = 2,
                                      .floorcod = 7,
                                      .fgaincod = 4};

struct Bsi {
    StreamType strmtyp = StreamType::kIndependent;
    int substreamid = 0;
    std::uint32_t words = 0;  // frmsiz + 1
    SampleRate sample_rate = SampleRate::k48000;
    int numblkscod = 3;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    bool compre = false;
    // Only ever set for an independent/convertible substream - see
    // parse_bsi's own comment on why a dependent's compre bit does not mean
    // this.
    std::optional<std::uint8_t> compr;
    std::optional<std::uint16_t> chanmap;
    // Ch2's own dialnorm/compr, present only when acmod is kDualMono (1+1).
    std::optional<int> dialnorm2;
    std::optional<std::uint8_t> compr2;
};

// Table E1.2's mixing-metadata payload. None of it changes how the audio is
// coded, but every field still has to be walked exactly: one bit out of place
// shifts audfrm along and the rest of the frame decodes as a different stream.
// The two strmtyp gates here are the point - an independent substream carries
// the program-scaling and mixing-configuration block that a dependent, which
// is only ever part of someone else's program, does not.
void skip_mixing_metadata(BitReader& r, const Bsi& bsi, int nblks) {
    const auto acmod = static_cast<std::uint8_t>(bsi.acmod);
    if (acmod > 0x2) {
        r.skip(2);  // dmixmod
    }
    if ((acmod & 0x1) != 0 && acmod > 0x2) {
        r.skip(3 + 3);  // ltrtcmixlev, lorocmixlev
    }
    if ((acmod & 0x4) != 0) {
        r.skip(3 + 3);  // ltrtsurmixlev, lorosurmixlev
    }
    if (bsi.lfe && r.read(1) != 0) {
        r.skip(5);  // lfemixlevcod
    }
    if (bsi.strmtyp != StreamType::kDependent) {
        if (r.read(1) != 0) r.skip(6);  // pgmscl
        if (acmod == 0x0 && r.read(1) != 0) r.skip(6);  // pgmscl2
        if (r.read(1) != 0) r.skip(6);  // extpgmscl
        switch (r.read(2)) {            // mixdef
            case 0x1: r.skip(1 + 1 + 3); break;  // premixcmpsel, drcsrc, premixcmpscl
            case 0x2: r.skip(12); break;         // mixdata
            case 0x3: {
                // mixdeflen sizes the WHOLE remaining element, sub-fields and
                // byte-alignment padding included, so it can be skipped whole
                // without walking mixdata2e/mixdata3e.
                const auto mixdeflen = r.read(5);
                r.skip((mixdeflen + 2) * 8);
                break;
            }
            default: break;
        }
        if (acmod < 0x2) {
            if (r.read(1) != 0) r.skip(8 + 6);  // panmean, paninfo
            if (acmod == 0x0 && r.read(1) != 0) r.skip(8 + 6);
        }
        if (r.read(1) != 0) {  // frmmixcfginfoe
            if (bsi.numblkscod == 0x0) {
                r.skip(5);  // blkmixcfginfo[0]
            } else {
                for (int blk = 0; blk < nblks; ++blk) {
                    if (r.read(1) != 0) r.skip(5);  // blkmixcfginfo[blk]
                }
            }
        }
    }
}

// Table E1.2's informational-metadata payload: bsmod and the production notes.
void skip_informational_metadata(BitReader& r, const Bsi& bsi) {
    const auto acmod = static_cast<std::uint8_t>(bsi.acmod);
    r.skip(3 + 1 + 1);  // bsmod, copyrightb, origbs
    if (acmod == 0x2) {
        r.skip(2 + 2);  // dsurmod, dheadphonmod
    }
    if (acmod >= 0x6) {
        r.skip(2);  // dsurexmod
    }
    if (r.read(1) != 0) r.skip(5 + 2 + 1);  // mixlevel, roomtyp, adconvtyp
    if (acmod == 0x0 && r.read(1) != 0) r.skip(5 + 2 + 1);
    // §E2.3.2.6: sourcefscod is present only when fscod != 0x3 - a fscod2
    // frame never carries it at all.
    if (!is_reduced_rate(bsi.sample_rate)) {
        r.skip(1);
    }
}

std::expected<Bsi, DecodeError> parse_bsi(BitReader& r, std::size_t frame_bytes) {
    Bsi bsi;
    if (r.read(16) != kSyncWord) {
        return std::unexpected(DecodeError::kBadSyncWord);
    }
    const auto strmtyp = r.read(2);
    if (strmtyp == static_cast<std::uint32_t>(StreamType::kReserved)) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    bsi.strmtyp = static_cast<StreamType>(strmtyp);
    bsi.substreamid = static_cast<int>(r.read(3));
    bsi.words = r.read(11) + 1;  // frmsiz counts words minus one
    if (bsi.words * 2 != frame_bytes) {
        return std::unexpected(DecodeError::kTruncated);
    }
    const auto fscod = r.read(2);
    if (fscod == 0x3) {
        // §E2.3.1.3: fscod2 replaces numblkscod outright when it is used - a
        // reduced-rate frame is implicitly always six blocks, so numblkscod's
        // bits are never sent. Modelling that as numblkscod == 0x3 (rather
        // than adding a parallel "six blocks, no field" flag) means every
        // downstream numblkscod check below - which is really asking "is this
        // the always-six-blocks case?" - keeps working unmodified.
        const auto fscod2 = r.read(2);
        const auto rate = sample_rate_from_fscod2(fscod2);
        if (!rate) {
            return std::unexpected(DecodeError::kReservedValue);
        }
        bsi.sample_rate = *rate;
        bsi.numblkscod = 0x3;
    } else {
        bsi.sample_rate = static_cast<SampleRate>(fscod);
        // Table E2.4. Fewer than six blocks shortens the syncframe and flips
        // four of Table E1.2/E1.3's implied values, all of which fall out of
        // nblks below. Nothing in this repo emits it and neither does
        // FFmpeg's encoder, so unlike the six-block path it is spec-derived
        // rather than measured.
        bsi.numblkscod = static_cast<int>(r.read(2));
    }
    bsi.acmod = static_cast<Acmod>(r.read(3));
    bsi.lfe = r.read(1) != 0;
    const auto bsid = static_cast<int>(r.read(5));
    if (bsid < eac3::kMinDecodableBsid || bsid > eac3::kBsid) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    bsi.dialnorm = static_cast<int>(r.read(5));
    // §E3.8.5: in a DEPENDENT substream compre marks the last dependent of the
    // program rather than announcing a compression word - though it still
    // drags one in. Either way the 8 bits have to be consumed; only stored
    // into bsi.compr when this substream is independent/convertible, where
    // the word is actually what it says it is.
    bsi.compre = r.read(1) != 0;
    if (bsi.compre) {
        const auto compr = static_cast<std::uint8_t>(r.read(8));
        if (bsi.strmtyp != StreamType::kDependent) {
            bsi.compr = compr;
        }
    }
    // Annex E Table E1.2: unconditional on strmtyp, mirroring the encoder's
    // own write side - even a dependent substream coding 1+1 would carry it,
    // though nothing in this repo ever builds one.
    if (bsi.acmod == Acmod::kDualMono) {
        bsi.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            bsi.compr2 = static_cast<std::uint8_t>(r.read(8));
        }
    }
    if (bsi.strmtyp == StreamType::kDependent && r.read(1) != 0) {  // chanmape
        bsi.chanmap = static_cast<std::uint16_t>(r.read(16));
    }
    const int nblks = eac3::blocks_per_syncframe(bsi.numblkscod);
    if (r.read(1) != 0) {  // mixmdate
        skip_mixing_metadata(r, bsi, nblks);
    }
    if (r.read(1) != 0) {  // infomdate
        skip_informational_metadata(r, bsi);
    }
    if (bsi.strmtyp == StreamType::kIndependent && bsi.numblkscod != 0x3) {
        r.skip(1);  // convsync
    }
    if (bsi.strmtyp == StreamType::kConvertible) {
        const bool blkid = bsi.numblkscod == 0x3 || r.read(1) != 0;
        if (blkid) {
            r.skip(6);  // frmsizecod, describing the AC-3 frame this came from
        }
    }
    if (r.read(1) != 0) {  // addbsie
        const auto addbsil = r.read(6);
        r.skip((addbsil + 1) * 8);
    }
    return bsi;
}

struct AudFrm {
    bool ahte = false;
    int snroffststr = 0;
    bool transproce = false;
    bool blkswe = false;
    bool dithflage = false;
    bool bamode = false;
    bool frmfgaincode = false;
    bool dbaflde = false;
    bool skipflde = false;
    int frmcsnroffst = 0;
    int frmfsnroffst = 0;
    // [block][channel]; the LFE's is a separate one-bit strategy.
    std::array<std::array<ExpStrategy, kMaxSubstreamChannels>, kBlocksPerFrame> chexpstr{};
    std::array<ExpStrategy, kBlocksPerFrame> lfeexpstr{};
    // cplstre[blk]: whether THIS block resends the coupling strategy (true
    // for block 0's implied strategy). cplinu[blk]: the strategy in effect
    // for that block, valid whether resent here or carried over from an
    // earlier one. Both are decided in audfrm, ahead of any block's payload.
    std::array<bool, kBlocksPerFrame> cplstre{};
    std::array<bool, kBlocksPerFrame> cplinu{};
    // The coupling channel's own exponent strategy, same Table E2.10 shape
    // as chexpstr, only present where cplinu[blk] holds.
    std::array<ExpStrategy, kBlocksPerFrame> cplexpstr{};
    // §E3.6.4.2.3's per-channel notch code, frame-constant. -1 means this
    // channel does not attenuate (chinspxatten[ch] clear, or spxattene clear
    // for the whole frame).
    std::array<int, kMaxSubstreamChannels> spxattencod{};
    // §E2.2.3: which streams are AHT-coded this frame - cplahtinu at
    // kCplStream, chahtinu[ch] at [0, nfchans), lfeahtinu at [nfchans].
    // Frame-constant, like everything else AHT touches (it needs exactly one
    // exponent set for the whole frame, which rules out anything per-block).
    std::array<bool, kMaxSubstreamStreams> ahtinu{};
    // §3.7: per full-bandwidth channel, only meaningful where chintransproc
    // is set (which itself is only meaningful when transproce is). Location
    // is already in samples (multiplied by 4 at parse time), not the raw
    // 10-bit field.
    std::array<bool, kMaxSubstreamChannels> chintransproc{};
    std::array<int, kMaxSubstreamChannels> transprocloc{};
    std::array<int, kMaxSubstreamChannels> transproclen{};
};

std::expected<AudFrm, DecodeError> parse_audfrm(BitReader& r, const Bsi& bsi, int nblks) {
    const int nfchans = fullbw_channel_count(bsi.acmod);
    AudFrm frm;
    frm.spxattencod.fill(-1);
    // Only a six-block syncframe can hoist its exponent strategies; a shorter
    // one always carries them per block and never uses AHT.
    bool expstre = true;
    if (bsi.numblkscod == 0x3) {
        expstre = r.read(1) != 0;
        frm.ahte = r.read(1) != 0;
    }
    frm.snroffststr = static_cast<int>(r.read(2));
    if (frm.snroffststr == 0x3) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    frm.transproce = r.read(1) != 0;
    frm.blkswe = r.read(1) != 0;
    frm.dithflage = r.read(1) != 0;
    frm.bamode = r.read(1) != 0;
    frm.frmfgaincode = r.read(1) != 0;
    frm.dbaflde = r.read(1) != 0;
    frm.skipflde = r.read(1) != 0;
    const bool spxattene = r.read(1) != 0;

    // Coupling-in-use for every block is decided here, ahead of the blocks:
    // cplstre[0] is an implied 1 (block 0 always states a strategy), and
    // later blocks either resend one (cplstre[blk]) or inherit the last.
    if (static_cast<std::uint8_t>(bsi.acmod) > 0x1) {
        bool cplinu = r.read(1) != 0;
        frm.cplstre[0] = true;
        frm.cplinu[0] = cplinu;
        for (int blk = 1; blk < nblks; ++blk) {
            const bool resent = r.read(1) != 0;  // cplstre[blk]
            frm.cplstre[static_cast<std::size_t>(blk)] = resent;
            if (resent) {
                cplinu = r.read(1) != 0;
            }
            frm.cplinu[static_cast<std::size_t>(blk)] = cplinu;
        }
    }

    if (expstre) {
        // Per-block explicit strategies. This project's own encoder always
        // hoists (kExpstre == 0, the `else` branch below), so this path is
        // spec-derived generality rather than something measured against a
        // real stream - matching the project's existing stance on syntax
        // this encoder never exercises (e.g. numblkscod != 3).
        for (int blk = 0; blk < nblks; ++blk) {
            if (frm.cplinu[static_cast<std::size_t>(blk)]) {
                frm.cplexpstr[static_cast<std::size_t>(blk)] =
                    static_cast<ExpStrategy>(r.read(2));  // cplexpstr[blk]
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                frm.chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] =
                    static_cast<ExpStrategy>(r.read(2));
            }
        }
    } else {
        // Table E2.10: one 5-bit code per channel expands to all six blocks.
        // frmcplexpstr precedes the per-channel codes, and is present only
        // when some block in the frame actually couples.
        const bool cpl_active =
            std::find(frm.cplinu.begin(), frm.cplinu.begin() + nblks, true) !=
            frm.cplinu.begin() + nblks;
        if (cpl_active) {
            const auto code = static_cast<int>(r.read(5));  // frmcplexpstr
            for (int blk = 0; blk < nblks; ++blk) {
                frm.cplexpstr[static_cast<std::size_t>(blk)] =
                    eac3::frame_exp_strategy(code, blk);
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto code = static_cast<int>(r.read(5));
            for (int blk = 0; blk < nblks; ++blk) {
                frm.chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] =
                    eac3::frame_exp_strategy(code, blk);
            }
        }
    }
    if (bsi.lfe) {
        for (int blk = 0; blk < nblks; ++blk) {
            frm.lfeexpstr[static_cast<std::size_t>(blk)] =
                r.read(1) != 0 ? ExpStrategy::kD15 : ExpStrategy::kReuse;
        }
    }
    // The whole converter-exponent element is gated on strmtyp == 0x0: only an
    // independent substream can be converted back to AC-3, so a dependent
    // sends none of it. These strategies describe how such a converter would
    // code the frame and have no bearing on decoding it.
    if (bsi.strmtyp != StreamType::kDependent) {
        const bool convexpstre = bsi.numblkscod == 0x3 || r.read(1) != 0;
        if (convexpstre) {
            r.skip(static_cast<std::size_t>(nfchans) * 5);  // convexpstr[ch]
        }
    }
    if (frm.ahte) {
        // §E2.2.3: cplahtinu, then chahtinu[ch] per fbw channel, then
        // lfeahtinu - exactly which streams re-code their six blocks of
        // mantissas as one gain-adaptively-quantized set instead of the
        // ordinary per-block grouped format. cplahtinu is gated the same way
        // frmcplexpstr was above: present only when some block actually
        // couples this frame.
        const bool cpl_active =
            std::find(frm.cplinu.begin(), frm.cplinu.begin() + nblks, true) !=
            frm.cplinu.begin() + nblks;
        if (cpl_active) {
            frm.ahtinu[static_cast<std::size_t>(kCplStream)] = r.read(1) != 0;  // cplahtinu
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            frm.ahtinu[static_cast<std::size_t>(ch)] = r.read(1) != 0;  // chahtinu[ch]
        }
        if (bsi.lfe) {
            frm.ahtinu[static_cast<std::size_t>(nfchans)] = r.read(1) != 0;  // lfeahtinu
        }
    }
    if (frm.snroffststr == 0x0) {
        frm.frmcsnroffst = static_cast<int>(r.read(6));
        frm.frmfsnroffst = static_cast<int>(r.read(4));
    }
    if (frm.transproce) {
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto uch = static_cast<std::size_t>(ch);
            frm.chintransproc[uch] = r.read(1) != 0;
            if (frm.chintransproc[uch]) {
                // §2.3.2.22/.23: transprocloc has 4-sample resolution -
                // multiplied out here so every other place this is used
                // works in plain sample counts, matching §3.7.2's own
                // pseudocode (which is written in samples throughout).
                frm.transprocloc[uch] = static_cast<int>(r.read(10)) * 4;
                frm.transproclen[uch] = static_cast<int>(r.read(8));
            }
        }
    }
    if (spxattene) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {  // chinspxatten[ch]
                frm.spxattencod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(5));
            }
        }
    }
    if (bsi.numblkscod != 0x0 && r.read(1) != 0) {  // blkstrtinfoe
        r.skip(static_cast<std::size_t>(eac3::block_start_info_bits(nblks, bsi.words)));
    }
    return frm;
}

}  // namespace

std::expected<std::optional<DecodedSubstream>, DecodeError> Eac3Decoder::decode_substream(
    std::span<const std::byte> frame) {
    if (frame.size() < 8) {
        return std::unexpected(DecodeError::kTruncated);
    }
    // There is no crc1 in E-AC-3 and no 5/8 checkpoint to protect, so crc2 is
    // the whole error check: the register reads zero over the frame past the
    // sync word, its own two bytes included.
    if (crc16(frame.subspan(2)) != 0) {
        return std::unexpected(DecodeError::kBadCrc);
    }

    BitReader r{frame};
    const auto bsi = parse_bsi(r, frame.size());
    if (!bsi) {
        return std::unexpected(bsi.error());
    }
    const int nblks = eac3::blocks_per_syncframe(bsi->numblkscod);
    const int nfchans = fullbw_channel_count(bsi->acmod);
    const int nchans = nfchans + (bsi->lfe ? 1 : 0);

    const auto frm = parse_audfrm(r, *bsi, nblks);
    if (!frm) {
        return std::unexpected(frm.error());
    }
    // §E2.3.1.8: a chanmap that does not account for exactly the channels
    // acmod and lfeon code would put audio in the wrong speakers rather than
    // fail to parse, so it has to be caught explicitly.
    if (bsi->chanmap && eac3::chanmap::channel_count(*bsi->chanmap) != nchans) {
        return std::unexpected(DecodeError::kInvalidStream);
    }

    DecodedSubstream out;
    out.strmtyp = bsi->strmtyp;
    out.substreamid = bsi->substreamid;
    out.sample_rate = bsi->sample_rate;
    out.acmod = bsi->acmod;
    out.lfe = bsi->lfe;
    out.dialnorm = bsi->dialnorm;
    out.compr = bsi->compr;
    out.dialnorm2 = bsi->dialnorm2;
    out.compr2 = bsi->compr2;
    out.numblkscod = bsi->numblkscod;
    out.chanmap = bsi->chanmap;
    out.last_dependent = bsi->strmtyp == StreamType::kDependent && bsi->compre;
    out.blksw.assign(static_cast<std::size_t>(nfchans), {});
    out.channels.assign(static_cast<std::size_t>(nchans),
                        std::vector<float>(static_cast<std::size_t>(nblks * kSamplesPerBlock),
                                           0.0f));

    // §E2.3.1.2: a dependent's substreamid starts again at 0 in its own space,
    // so identity - and hence which overlap-add history belongs to this frame
    // - is the pair, never the id alone.
    auto& delay = delay_[static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid];

    std::array<int, kMaxSubstreamStreams> endmant{};
    std::array<std::vector<std::uint8_t>, kMaxSubstreamStreams> exps;
    std::array<std::vector<std::uint8_t>, kMaxSubstreamStreams> bap;
    BitAllocCodes codes = kBamode0Codes;
    std::array<int, kMaxSubstreamStreams> fgaincod{};
    fgaincod.fill(kBamode0Codes.fgaincod);
    std::array<int, kMaxSubstreamStreams> fsnroffst{};
    int csnroffst = 0;
    std::array<bool, 4> rematflg{};
    // §7.2.2.6, reset to "no segments" at the start of every syncframe like
    // fsnroffst/codes above, then persisting block to block until
    // re-transmitted or cleared. Only the per-fbw-channel deltbae[ch] exists
    // (§5.4.3.49/E2.3.2.9 bound their loop by nfchans) - no coupling-channel
    // or LFE slot.
    std::array<DeltaSegments, kMaxSubstreamChannels> delta{};

    // Coupling state (Annex E variant of §7.4). All of it persists until
    // re-transmitted - this encoder only ever (re)sends geometry in block 0,
    // but a general stream could resend it on any block whose cplstre is set.
    bool phsflginu = false;
    int cplbegf = 0;
    int ecplbegf = 0;  // persists like cplbegf; needed again by nrematbd below
    int cplstrtmant = 0;
    int cplendmant = 0;
    int ncplbnd = 0;
    int cplfleak = 0;
    int cplsleak = 0;
    std::vector<bool> chincpl(static_cast<std::size_t>(nfchans), false);
    // Which coupling band each sub-band belongs to (cplbndstrc expansion).
    std::vector<int> subband_band;
    // [channel][sub-band] - already expanded from bands to sub-bands.
    std::vector<std::vector<double>> cplco(static_cast<std::size_t>(nfchans));
    std::vector<bool> phsflg;

    // Spectral extension state (§3.6). Persists until re-transmitted, same as
    // coupling above - this encoder only ever (re)sends geometry in block 0.
    // Unlike coupling's per-sub-band coordinates, spx coordinates are one per
    // BAND already (no sub-band duplication step), so spx_bands/spxco are
    // indexed by band directly throughout.
    bool spxinu = false;
    std::vector<bool> chinspx(static_cast<std::size_t>(nfchans), false);
    int spxstrtf = 0;
    int spxbegf = 0;
    int spx_startmant = 0;  // spx_band_start(spx_begin_subbnd) - extension begins here
    int spx_endmant = 0;    // spx_band_start(spx_end_subbnd) - one past the last bin
    int spx_copystart = 0;  // spx_band_start(spxstrtf) - copy-up wraps back to here
    eac3::BandLayout spx_bands{};
    // [channel][band]
    std::vector<std::vector<double>> spxco(static_cast<std::size_t>(nfchans));
    std::vector<int> spxblnd(static_cast<std::size_t>(nfchans), 0);
    eac3::SpxNoise spx_noise;

    // AHT-decoded coefficients (§3.4), one array of all six blocks per
    // stream. Unlike every other per-stream array above, these are produced
    // once - at block 0, since an AHT stream's mantissas exist only there -
    // rather than block by block, so they need their own frame-lifetime
    // buffer distinct from the per-block-local `coeffs` below.
    std::vector<std::array<std::array<double, 256>, kBlocksPerFrame>> aht_coeffs(
        static_cast<std::size_t>(kMaxSubstreamStreams));

    // Enhanced coupling state (§E3.5), parallel to the standard-coupling
    // state above and mutually exclusive with it per block (ecplinu picks
    // one or the other). Persistence follows the same convention: geometry
    // and coordinates persist until re-transmitted.
    bool ecplinu_now = false;
    int ecpl_begin_subbnd = 0;
    int ecpl_end_subbnd = 0;
    // Indexed absolutely by sub-band, same convention as kDefaultEcplBandStructure.
    // std::array rather than vector<bool>: it needs to convert to
    // std::span<const bool> at the call sites below, which vector<bool>'s
    // bitset specialization cannot do.
    std::array<bool, eac3::kEcplSubBands> ecpl_structure{};
    bool ecplangleintrp = false;
    // [channel][band] - the raw transmitted indices, persisted until
    // re-sent. Kept as indices (not decoded values) because decoding
    // depends on a channel's role this block (is-first-channel, ecpltrans),
    // which can only be resolved once chincpl for THIS block is known.
    std::vector<std::vector<int>> ecplamp_raw(static_cast<std::size_t>(nfchans));
    std::vector<std::vector<int>> ecplangle_raw(static_cast<std::size_t>(nfchans));
    std::vector<std::vector<int>> ecplchaos_raw(static_cast<std::size_t>(nfchans));
    std::vector<bool> ecpltrans_persist(static_cast<std::size_t>(nfchans), false);
    eac3::EcplNoise ecpl_noise;
    // Every block's enhanced coupling channel raw mantissas (§3.5.5.1's
    // XCURR), stashed as each block is parsed so the second pass below can
    // look at any block's neighbors freely - a block whose neighbor did not
    // use enhanced coupling substitutes zero there (`ecpl_active`), exactly
    // the rule §3.5.5.1 itself specifies. This also covers this syncframe's
    // own first/last block, whose true neighbor lives in an adjacent
    // syncframe this call was not given: a real, documented approximation,
    // not a bug - every interior block reconstructs with its true
    // neighbors.
    // Heap-allocated like aht_coeffs above (PREfast's C6262, alert #63): a
    // fixed std::array here was the single largest contributor to
    // decode_substream's oversized stack frame, and - same as aht_coeffs -
    // it's produced once per call, not per (block, channel) iteration, so
    // there's no hot-loop allocation cost to heap-allocating it.
    std::vector<std::array<double, 256>> ecpl_all_coeffs(static_cast<std::size_t>(kBlocksPerFrame));
    std::array<bool, kBlocksPerFrame> ecpl_active{};

    // Everything the second pass below (spx synthesis, rematrixing, IMDCT
    // and PCM write) needs from this block, captured once bitstream parsing
    // for it is done. A second pass is unavoidable rather than finishing
    // each block inline as AC-3's decoder and this decoder's OTHER tools
    // all do: enhanced coupling's channel reconstruction (§3.5.5.1) needs
    // the block AFTER the one it reconstructs, which is not available until
    // that later block has itself been parsed - and IMDCT's overlap-add
    // delay line is strictly sequential across blocks, so finishing block N
    // before block N-1 is not an option either. Capturing every block's
    // state (not only the enhanced-coupling ones) keeps that sequencing
    // simple: one pass parses everything in the bitstream's own order, the
    // next finishes every block, still in order 0..nblks-1.
    struct BlockTail {
        std::vector<std::array<double, 256>> coeffs;  // per stream; decoupled where standard
        std::vector<bool> chincpl;
        bool cplinu = false;
        bool ecplinu_now = false;
        // Standard coupling (valid when cplinu && !ecplinu_now): decoupling
        // already ran inline in pass one, so `coeffs` is final for these
        // channels and nothing further is needed here.
        //
        // Enhanced coupling (valid when cplinu && ecplinu_now):
        int firstchincpl = -1;
        int ecpl_begin_subbnd = 0;
        int ecpl_end_subbnd = 0;
        std::array<bool, eac3::kEcplSubBands> ecpl_structure{};
        std::vector<std::vector<int>> ecplamp_raw;    // [ch][band]
        std::vector<std::vector<int>> ecplangle_raw;  // [ch][band]
        std::vector<std::vector<int>> ecplchaos_raw;  // [ch][band]
        std::vector<bool> ecpltrans;                  // [ch]
        int cplstrtmant = 0;
        int cplendmant = 0;
        // spx (§3.6)
        bool spxinu = false;
        std::vector<bool> chinspx;
        eac3::BandLayout spx_bands{};
        std::vector<std::vector<double>> spxco;
        std::vector<int> spxblnd;
        int spx_startmant = 0;
        int spx_endmant = 0;
        int spx_copystart = 0;
        // rematrixing (§7.5.4, 2/0 only) and block switching
        std::array<bool, 4> rematflg{};
        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> blksw{};
        std::array<int, kMaxSubstreamStreams> endmant{};
    };
    std::vector<BlockTail> tails(static_cast<std::size_t>(nblks));

    for (int blk = 0; blk < nblks; ++blk) {
        const auto strategy = [&](int ch) {
            return ch < nfchans
                       ? frm->chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)]
                       : frm->lfeexpstr[static_cast<std::size_t>(blk)];
        };

        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> blksw{};
        if (frm->blkswe) {
            for (int ch = 0; ch < nfchans; ++ch) {
                blksw[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            out.blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(blk)] =
                blksw[static_cast<std::size_t>(ch)];
        }
        // Annex E Table E1.4's own audblk() syntax: full per-channel
        // dithflag[ch] syntax when dithflage is set, else every channel
        // defaults to "dithflag[ch] = 1 /* dither on */" for the block -
        // NOT off. Reconstruction happens in read_stream/the decoupling loop
        // below, the same split AC-3's own dithflag[ch] uses.
        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> dithflag{};
        if (frm->dithflage) {
            for (int ch = 0; ch < nfchans; ++ch) {
                dithflag[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            }
        } else {
            for (int ch = 0; ch < nfchans; ++ch) {
                dithflag[static_cast<std::size_t>(ch)] = true;
            }
        }
        if (r.read(1) != 0) {
            r.skip(8);  // dynrng: parsed, not applied
        }
        if (bsi->acmod == Acmod::kDualMono && r.read(1) != 0) {
            r.skip(8);  // dynrng2e -> dynrng2: parsed, not applied - same as dynrng
        }

        // --- spectral extension strategy + geometry (§E2.3.3, §3.6) ---
        // Block 0's strategy is implied rather than sent; a later block only
        // resends it (spxstre) if the strategy actually changes - this
        // encoder never does, so everything below persists from block 0.
        const bool spxstre = blk == 0 || r.read(1) != 0;
        if (spxstre) {
            spxinu = r.read(1) != 0;
            if (spxinu) {
                // 1/0 is the one mode where chinspx is not transmitted: the
                // only channel there is always the one extended.
                if (bsi->acmod != Acmod::k1_0) {
                    for (int ch = 0; ch < nfchans; ++ch) {
                        chinspx[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                    }
                } else {
                    chinspx[0] = true;
                }
                spxstrtf = static_cast<int>(r.read(2));
                spxbegf = static_cast<int>(r.read(3));
                const int spxendf = static_cast<int>(r.read(3));
                const int begin_subbnd = eac3::spx_begin_subbnd(spxbegf);
                const int end_subbnd = eac3::spx_end_subbnd(spxendf);
                if (end_subbnd <= begin_subbnd || end_subbnd > eac3::kSpxSubBands) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                spx_startmant = eac3::spx_band_start(begin_subbnd);
                spx_endmant = eac3::spx_band_start(end_subbnd);
                spx_copystart = eac3::spx_band_start(spxstrtf);
                const int subband_count = end_subbnd - begin_subbnd;
                // spxbndstrc, relative to the region's first sub-band (as
                // group_bands wants it); the default table (kDefaultSpxBand-
                // Structure) is ABSOLUTE-indexed, so it is sliced at
                // begin_subbnd rather than used as-is.
                std::array<bool, eac3::kSpxSubBands> structure{};
                if (r.read(1) != 0) {  // spxbndstrce
                    for (int i = 1; i < subband_count; ++i) {
                        structure[static_cast<std::size_t>(i)] = r.read(1) != 0;
                    }
                } else {
                    // Unlike coupling's default table, spx's Table E2.11 is
                    // unambiguous (absolute-sub-band-indexed, verified
                    // against the spec text directly), so it is implemented
                    // for real rather than refused.
                    for (int i = 0; i < subband_count; ++i) {
                        structure[static_cast<std::size_t>(i)] =
                            eac3::kDefaultSpxBandStructure[static_cast<std::size_t>(
                                begin_subbnd + i)];
                    }
                }
                spx_bands = eac3::group_bands(spx_startmant, subband_count,
                                              eac3::kSpxBinsPerSubBand,
                                              std::span{structure}.first(
                                                  static_cast<std::size_t>(subband_count)));
                for (auto& channel : spxco) {
                    channel.assign(static_cast<std::size_t>(spx_bands.count), 0.0);
                }
            }
        }

        // --- spectral extension coordinates (§E3.3, block-0 spxcoe implied) ---
        if (spxinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chinspx[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                // firstspxcos[ch] starts at 1: block 0's spxcoe is implied.
                const bool send = blk == 0 || r.read(1) != 0;
                if (!send) {
                    continue;
                }
                spxblnd[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(5));
                const int master = static_cast<int>(r.read(2));
                auto& co = spxco[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < spx_bands.count; ++bnd) {
                    const auto exp = static_cast<std::uint8_t>(r.read(4));
                    const auto mant = static_cast<std::uint8_t>(r.read(2));
                    co[static_cast<std::size_t>(bnd)] = coupling::decode_coordinate(
                        {.exp = exp, .mant = mant}, master, coupling::kSpxMantissaBits);
                }
            }
        }

        // --- coupling strategy + geometry (§E2.3.3, Table E1.4) ---
        // cplinu itself was already decided for this block in audfrm
        // (frm->cplinu[blk]); only the GEOMETRY - which channels couple, the
        // coupled region, and how its sub-bands group into bands - is
        // per-block, and only present when this block resends the strategy
        // (frm->cplstre[blk]: true for block 0's implied strategy, and for
        // any later block that changes it - this encoder never does, so
        // everything below persists unchanged from block 0 onward).
        if (frm->cplstre[static_cast<std::size_t>(blk)] &&
            frm->cplinu[static_cast<std::size_t>(blk)]) {
            ecplinu_now = r.read(1) != 0;  // ecplinu: enhanced coupling
            // 2/0 is the one mode where chincpl is not transmitted: both
            // channels are coupled by definition. Every other mode sends
            // chincpl per channel. This part is common to both standard and
            // enhanced coupling; only what follows it differs.
            if (bsi->acmod == Acmod::k2_0) {
                chincpl[0] = chincpl[1] = true;
            } else {
                for (int ch = 0; ch < nfchans; ++ch) {
                    chincpl[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                }
            }
            if (!ecplinu_now) {
                // --- standard coupling geometry (§7.4.2/§5.4.3.12-13) ---
                // phsflginu exists only for standard coupling - enhanced
                // coupling carries its own per-channel angle instead and has
                // no separate phase-restoration flag.
                phsflginu = bsi->acmod == Acmod::k2_0 && r.read(1) != 0;
                cplbegf = static_cast<int>(r.read(4));
                // §E3.3.1: with spectral extension active this block, cplendf is
                // derived from spxbegf instead of transmitted, so the coupling
                // region ends exactly where synthesis begins.
                const int cplendf =
                    spxinu ? eac3::derived_cplendf(spxbegf) : static_cast<int>(r.read(4));
                const int subband_count = coupling::sub_band_count(cplbegf, cplendf);
                if (subband_count < 1) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                cplstrtmant = coupling::start_mant(cplbegf);
                cplendmant = coupling::end_mant(cplendf);
                if (cplendmant > 253 || cplstrtmant >= cplendmant) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                const bool cplbndstrce = r.read(1) != 0;
                // cplbndstrc: a 1 folds this sub-band into the previous coupling
                // band, so coordinates are per band and duplicated back out
                // across the sub-bands they cover. When cplbndstrce is 0 this
                // block doesn't transmit cplbndstrc and falls back to Table
                // E2.12 instead - but that table is indexed absolutely from
                // cplbegf == 0, not relative to this block's actual cplbegf,
                // so the slice consulted starts at kDefaultCplBandStructure[cplbegf].
                subband_band.assign(static_cast<std::size_t>(subband_count), 0);
                ncplbnd = 1;
                for (int bnd = 1; bnd < subband_count; ++bnd) {
                    const bool merged =
                        cplbndstrce ? r.read(1) != 0
                                    : eac3::kDefaultCplBandStructure[static_cast<std::size_t>(
                                          cplbegf + bnd)];
                    if (!merged) {
                        ++ncplbnd;
                    }
                    subband_band[static_cast<std::size_t>(bnd)] = ncplbnd - 1;
                }
                // Coordinates survive a re-sent strategy: cplcoe == 0 in this
                // very block legally means "reuse the previous coordinates", so
                // clearing them here would silence the coupled high band. Only a
                // change in geometry forces a resize, and only the new entries
                // start at zero.
                for (auto& channel : cplco) {
                    channel.assign(static_cast<std::size_t>(subband_count), 0.0);
                }
                phsflg.assign(static_cast<std::size_t>(ncplbnd), false);
            } else {
                // --- enhanced coupling geometry (§E2.3.3.16-19, §E3.5.2) ---
                phsflginu = false;
                ecplbegf = static_cast<int>(r.read(4));
                ecpl_begin_subbnd = eac3::ecpl_begin_subbnd(ecplbegf);
                if (spxinu) {
                    ecpl_end_subbnd = eac3::ecpl_end_subbnd_from_spx(spxbegf);
                } else {
                    const auto ecplendf = static_cast<int>(r.read(4));
                    ecpl_end_subbnd = eac3::ecpl_end_subbnd(ecplendf);
                }
                if (ecpl_end_subbnd <= ecpl_begin_subbnd ||
                    ecpl_end_subbnd > eac3::kEcplSubBands) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                cplstrtmant = eac3::kEcplSubBandTab[static_cast<std::size_t>(ecpl_begin_subbnd)];
                cplendmant = eac3::kEcplSubBandTab[static_cast<std::size_t>(ecpl_end_subbnd)];
                if (r.read(1) == 0) {  // ecplbndstrce
                    // Table E2.13's default table has an unambiguous absolute
                    // sub-band index (verified against the spec text
                    // directly, unlike standard coupling's Table E2.12) - so
                    // it is used for real here rather than refused.
                    ecpl_structure = eac3::kDefaultEcplBandStructure;
                } else {
                    ecpl_structure.fill(false);
                    const int first = std::max(9, ecpl_begin_subbnd + 1);
                    for (int sbnd = first; sbnd < ecpl_end_subbnd; ++sbnd) {
                        ecpl_structure[static_cast<std::size_t>(sbnd)] = r.read(1) != 0;
                    }
                }
                // Coordinates survive a re-sent strategy, same reasoning as
                // standard coupling above - only a geometry change forces a
                // resize.
                const auto band_count = static_cast<std::size_t>(
                    eac3::ecpl_group_bands(ecpl_begin_subbnd, ecpl_end_subbnd, ecpl_structure)
                        .count);
                for (auto& channel : ecplamp_raw) {
                    channel.assign(band_count, 0);
                }
                for (auto& channel : ecplangle_raw) {
                    channel.assign(band_count, 0);
                }
                for (auto& channel : ecplchaos_raw) {
                    channel.assign(band_count, 0);
                }
            }
        }

        // --- coupling coordinates (§7.4.3 shape, block-0 cplcoe implied) ---
        if (frm->cplinu[static_cast<std::size_t>(blk)] && !ecplinu_now) {
            bool any_new = false;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                // firstcplcos[ch] starts at 1: block 0's cplcoe is implied
                // rather than transmitted, unlike AC-3 which always sends it.
                const bool send = blk == 0 || r.read(1) != 0;
                if (!send) {
                    continue;
                }
                any_new = true;
                const int master = static_cast<int>(r.read(2));
                std::vector<double> band_values(static_cast<std::size_t>(ncplbnd));
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    const auto exp = static_cast<std::uint8_t>(r.read(4));
                    const auto mant = static_cast<std::uint8_t>(r.read(4));
                    band_values[static_cast<std::size_t>(bnd)] =
                        coupling::decode_coordinate({.exp = exp, .mant = mant}, master);
                }
                auto& channel = cplco[static_cast<std::size_t>(ch)];
                for (std::size_t bnd = 0; bnd < channel.size(); ++bnd) {
                    channel[bnd] = band_values[static_cast<std::size_t>(
                        subband_band[bnd])];
                }
            }
            if (phsflginu && any_new) {
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    phsflg[static_cast<std::size_t>(bnd)] = r.read(1) != 0;
                }
            }
        } else if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            // --- enhanced coupling coordinates (§E2.3.3.20-26, §3.5.4) ---
            ecplangleintrp = r.read(1) != 0;
            if (ecplangleintrp) {
                // Legal syntax this decoder does not implement - see
                // ecpl_angles' own doc comment. No stream this project's own
                // encoder produces sets this flag.
                return std::unexpected(DecodeError::kUnsupported);
            }
            int firstchincpl = -1;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                if (firstchincpl == -1) {
                    firstchincpl = ch;
                }
                // firstcplcos[ch] is simplified to "blk == 0", the same
                // approximation the standard-coupling coordinates above make
                // and for the same reason: this encoder never lets a channel
                // drop out of coupling and rejoin mid-frame.
                bool ecplparam1e;
                bool ecplparam2e;
                if (blk == 0) {
                    ecplparam1e = true;
                    ecplparam2e = ch > firstchincpl;
                } else {
                    ecplparam1e = r.read(1) != 0;
                    ecplparam2e = ch > firstchincpl && r.read(1) != 0;
                }
                const auto uch = static_cast<std::size_t>(ch);
                if (ecplparam1e) {
                    for (auto& v : ecplamp_raw[uch]) {
                        v = static_cast<int>(r.read(5));
                    }
                }
                if (ecplparam2e) {
                    for (std::size_t bnd = 0; bnd < ecplangle_raw[uch].size(); ++bnd) {
                        ecplangle_raw[uch][bnd] = static_cast<int>(r.read(6));
                        ecplchaos_raw[uch][bnd] = static_cast<int>(r.read(3));
                    }
                }
                // ecpltrans[ch] is read every block, unconditionally, for
                // every channel past the first - never gated by the exist
                // flags above and never persisted from a previous block.
                ecpltrans_persist[uch] = ch > firstchincpl && r.read(1) != 0;
            }
        }

        // A coupled or extended channel stops carrying its own coefficients
        // at whichever tool takes over first; coupling always wins the
        // channels it shares with spx, since the two are contiguous and
        // coupling sits below (§E3.3.1's whole point). Runs every block,
        // using the persistent geometry above, so a channel whose tool
        // membership never changes keeps the same cutoff without needing to
        // be re-derived only on the blocks that resend a strategy.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (frm->cplinu[static_cast<std::size_t>(blk)] &&
                chincpl[static_cast<std::size_t>(ch)]) {
                endmant[static_cast<std::size_t>(ch)] = cplstrtmant;
            } else if (spxinu && chinspx[static_cast<std::size_t>(ch)]) {
                endmant[static_cast<std::size_t>(ch)] = spx_startmant;
            }
        }

        if (bsi->acmod == Acmod::k2_0) {
            // Unlike AC-3, block 0's rematstr is IMPLIED 1 rather than
            // transmitted; only later blocks carry the bit.
            if (blk == 0 || r.read(1) != 0) {
                // §3.3.2 / §7.5.2: the rematrixing bands cannot reach above
                // whichever tool takes over the spectrum first, so their
                // count depends on where that tool starts. Coupling always
                // wins the comparison when both are active, since it is
                // always the lower of the two (§E3.3.1). Enhanced coupling
                // gets its own table (§3.3.2), keyed off ecplbegf rather than
                // cplbegf - a distinct formula, not a parameter substitution,
                // since its sub-band table starts at a different frequency.
                const int nrematbd =
                    frm->cplinu[static_cast<std::size_t>(blk)]
                        ? (ecplinu_now
                               ? (ecplbegf == 0   ? 0
                                  : ecplbegf == 1 ? 1
                                  : ecplbegf == 2 ? 2
                                  : ecplbegf < 5  ? 3
                                                  : 4)
                               : (cplbegf > 2 ? 4 : (cplbegf > 0 ? 3 : 2)))
                    : spxinu ? (spxbegf < 2 ? 3 : 4)
                             : 4;
                rematflg.fill(false);
                for (int band = 0; band < nrematbd; ++band) {
                    rematflg[static_cast<std::size_t>(band)] = r.read(1) != 0;
                }
            }
        }

        // chbwcod accompanies a fresh strategy, but only for a channel
        // carrying its own high band: a coupled or extended channel's
        // bandwidth is fixed by whichever tool takes over, and sending
        // chbwcod anyway would both waste the bits and desynchronise the
        // block.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (strategy(ch) == ExpStrategy::kReuse) {
                continue;
            }
            if (frm->cplinu[static_cast<std::size_t>(blk)] &&
                chincpl[static_cast<std::size_t>(ch)]) {
                continue;
            }
            if (spxinu && chinspx[static_cast<std::size_t>(ch)]) {
                continue;
            }
            const auto chbwcod = r.read(6);
            if (chbwcod > 60) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            endmant[static_cast<std::size_t>(ch)] = ((static_cast<int>(chbwcod) + 12) * 3) + 37;
        }

        // Coupling channel exponents, ahead of the fbw/LFE channels (§5.3.3
        // order: coupling channel first). Offset to its own start bin and
        // using the even-valued absolute reference, same as AC-3.
        if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            const auto strat = frm->cplexpstr[static_cast<std::size_t>(blk)];
            if (strat == ExpStrategy::kReuse) {
                if (blk == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            } else {
                const int span = cplendmant - cplstrtmant;
                const int group_size = exponent_group_size(strat);
                if (group_size == 0 || span % (3 * group_size) != 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                const int ngrps = span / (3 * group_size);
                const auto cplabsexp = static_cast<std::uint8_t>(r.read(4));
                std::vector<std::uint8_t> groups(static_cast<std::size_t>(ngrps));
                for (auto& g : groups) {
                    g = static_cast<std::uint8_t>(r.read(7));
                    if (g > 124) {  // §7.10.2 error condition 17
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                auto& target = exps[static_cast<std::size_t>(kCplStream)];
                target.assign(static_cast<std::size_t>(cplendmant), kMaxExponent);
                decode_coupling_exponents(
                    cplabsexp, groups, strat,
                    std::span{target}.subspan(static_cast<std::size_t>(cplstrtmant)));
                // §7.2.2.2: exponents are 0..24, and the reconstruction
                // shifts by them - out of range is undefined behaviour.
                for (std::size_t bin = static_cast<std::size_t>(cplstrtmant);
                     bin < target.size(); ++bin) {
                    if (target[bin] > kMaxExponent) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                endmant[static_cast<std::size_t>(kCplStream)] = cplendmant;
            }
        }

        for (int ch = 0; ch < nchans; ++ch) {
            const auto strat = strategy(ch);
            if (strat == ExpStrategy::kReuse) {
                if (blk == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                continue;
            }
            const int end = ch < nfchans ? endmant[static_cast<std::size_t>(ch)] : kLfeEndmant;
            endmant[static_cast<std::size_t>(ch)] = end;
            const int ngrps = ch < nfchans ? exponent_group_count(strat, end) : 2;
            const auto absolute = static_cast<std::uint8_t>(r.read(4));
            std::vector<std::uint8_t> groups(static_cast<std::size_t>(ngrps));
            for (auto& g : groups) {
                g = static_cast<std::uint8_t>(r.read(7));
                if (g > 124) {  // §7.10.2 error condition 17
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            auto& target = exps[static_cast<std::size_t>(ch)];
            target.assign(static_cast<std::size_t>(end), 0);
            decode_exponents(absolute, groups, strat, target);
            // §7.2.2.2: exponents are 0..24, and the reconstruction shifts by
            // them - out of range is undefined behaviour, not wrong audio.
            if (std::ranges::any_of(target, [](auto e) { return e > kMaxExponent; })) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            if (ch < nfchans) {
                r.skip(2);  // gainrng
            }
        }

        if (frm->bamode && r.read(1) != 0) {  // baie
            codes.sdcycod = static_cast<int>(r.read(2));
            codes.fdcycod = static_cast<int>(r.read(2));
            codes.sgaincod = static_cast<int>(r.read(2));
            codes.dbpbcod = static_cast<int>(r.read(2));
            codes.floorcod = static_cast<int>(r.read(3));
        }
        if (frm->snroffststr == 0x0) {
            // Strategy 1: the frame's pair applies to every channel of every
            // block, the LFE included.
            csnroffst = frm->frmcsnroffst;
            fsnroffst.fill(frm->frmfsnroffst);
        } else if (blk == 0 || r.read(1) != 0) {  // snroffste
            csnroffst = static_cast<int>(r.read(6));
            if (frm->snroffststr == 0x1) {
                fsnroffst.fill(static_cast<int>(r.read(4)));
            } else {
                for (int ch = 0; ch < nchans; ++ch) {
                    fsnroffst[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(4));
                }
            }
        }
        // fgaincode is only ever sent when the frame said it might be; absent,
        // every channel's fast gain reverts to 0x4 for this block.
        if (frm->frmfgaincode && r.read(1) != 0) {
            for (int ch = 0; ch < nchans; ++ch) {
                fgaincod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(3));
            }
        } else {
            fgaincod.fill(kBamode0Codes.fgaincod);
        }
        if (bsi->strmtyp != StreamType::kDependent && r.read(1) != 0) {  // convsnroffste
            r.skip(10);  // convsnroffst: for a converter's allocation, not ours
        }
        // Coupling leak seeds. firstcplleak starts at 1: block 0's seeds are
        // mandatory (no cplleake bit ahead of them), unlike AC-3 where the
        // gating bit is always present; later blocks send an explicit
        // cplleake bit and may choose to keep block 0's seeds instead.
        if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            if (blk == 0 || r.read(1) != 0) {  // cplleake
                cplfleak = static_cast<int>(r.read(3));
                cplsleak = static_cast<int>(r.read(3));
            }
        }
        if (frm->dbaflde && r.read(1) != 0) {  // deltbaie
            // §E2.3.2.9/§5.4.3.49-57: deltbae[ch] per fbw channel only - no
            // cpldeltbae, since coupling already errors before this point.
            // The syntax table reads every channel's 2-bit deltbae[ch] code
            // FIRST, then every channel's segment data - not interleaved per
            // channel - so all codes are read and validated up front. Bounds
            // are checked here, before compute_bit_allocation ever sees them,
            // since deltoffst/deltlen are attacker-controlled and mask[] is
            // exactly 50 bands wide.
            std::array<int, eac3::chanmap::kMaxSubstreamFullbw> chcodes{};
            for (int ch = 0; ch < nfchans; ++ch) {
                chcodes[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
                if (chcodes[static_cast<std::size_t>(ch)] == 3) {  // Table 5.16: reserved
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (blk == 0 && chcodes[static_cast<std::size_t>(ch)] == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);  // shall not reuse in block 0
                }
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const int chcode = chcodes[static_cast<std::size_t>(ch)];
                if (chcode == 1) {  // new info follows
                    DeltaSegments segs;
                    segs.deltnseg = static_cast<int>(r.read(3)) + 1;
                    int band = 0;
                    for (int seg = 0; seg < segs.deltnseg; ++seg) {
                        segs.deltoffst[static_cast<std::size_t>(seg)] =
                            static_cast<std::uint8_t>(r.read(5));
                        segs.deltlen[static_cast<std::size_t>(seg)] =
                            static_cast<std::uint8_t>(r.read(4));
                        segs.deltba[static_cast<std::size_t>(seg)] =
                            static_cast<std::uint8_t>(r.read(3));
                        band += segs.deltoffst[static_cast<std::size_t>(seg)];
                        const int len = segs.deltlen[static_cast<std::size_t>(seg)];
                        if (band < 0 || band + len > 50) {
                            return std::unexpected(DecodeError::kInvalidStream);
                        }
                        band += len;
                    }
                    delta[static_cast<std::size_t>(ch)] = segs;
                } else if (chcode == 2) {  // perform no delta alloc
                    delta[static_cast<std::size_t>(ch)] = {};
                }
                // chcode == 0 (reuse): leave delta[ch] exactly as it was.
            }
        } else if (blk == 0) {
            // §5.4.3.47: deltbaie == 0 in block 0 forces "no delta alloc" for
            // every fbw channel. Reached both when dbaflde is clear (delta[]
            // is already {} from the frame-start reset, so this is a no-op)
            // and when dbaflde is set but this frame's first block's deltbaie
            // reads 0 (where it is the rule that actually matters).
            for (int ch = 0; ch < nfchans; ++ch) {
                delta[static_cast<std::size_t>(ch)] = {};
            }
        }
        if (frm->skipflde && r.read(1) != 0) {  // skiple
            const auto skipl = r.read(9);
            r.skip(skipl * 8);
        }

        // §7.2.2.1.1 is frame-wide: csnroffst together with EVERY channel's
        // fine offset. Deciding it per channel would zero one allocation while
        // the others allocate normally, desynchronising the shared mantissa
        // stream.
        bool snr_all_zero = csnroffst == 0;
        for (int ch = 0; ch < nchans && snr_all_zero; ++ch) {
            snr_all_zero = fsnroffst[static_cast<std::size_t>(ch)] == 0;
        }
        if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            const auto s = static_cast<std::size_t>(kCplStream);
            const int end = endmant[s];
            if (static_cast<int>(exps[s].size()) != end) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            BitAllocCodes cpl_codes = codes;
            cpl_codes.fgaincod = fgaincod[s];
            bap[s].assign(static_cast<std::size_t>(end), 0);
            compute_bit_allocation(exps[s], bsi->sample_rate, cpl_codes, csnroffst,
                                   fsnroffst[s], bap[s],
                                   {.start = cplstrtmant,
                                    .coupling = true,
                                    .cplfleak = cplfleak,
                                    .cplsleak = cplsleak,
                                    .snr_all_zero = snr_all_zero,
                                    .high_efficiency = frm->ahtinu[s]});
        }
        for (int ch = 0; ch < nchans; ++ch) {
            const int end = endmant[static_cast<std::size_t>(ch)];
            if (static_cast<int>(exps[static_cast<std::size_t>(ch)].size()) != end) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            BitAllocCodes channel_codes = codes;
            channel_codes.fgaincod = fgaincod[static_cast<std::size_t>(ch)];
            bap[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(end), 0);
            // delta[ch] for ch == LFE's index is always {} (never written -
            // §5.4.3.49/E2.3.2.9 bound their deltbae[ch] loop by nfchans, so
            // the LFE channel has no delta bit allocation field at all).
            compute_bit_allocation(exps[static_cast<std::size_t>(ch)], bsi->sample_rate,
                                   channel_codes, csnroffst,
                                   fsnroffst[static_cast<std::size_t>(ch)],
                                   bap[static_cast<std::size_t>(ch)],
                                   {.snr_all_zero = snr_all_zero,
                                    .high_efficiency = frm->ahtinu[static_cast<std::size_t>(ch)],
                                    .delta = delta[static_cast<std::size_t>(ch)]});
        }

        // Mantissas, in coded order: fbw channels (the first coupled one
        // pulling in the shared coupling channel right after it, same as
        // AC-3), then the LFE.
        MantissaBlockReader mantissa_reader;
        // Heap-backed, matching decoder.cpp's own per-block coeffs: at
        // kMaxSubstreamStreams * 256 doubles, a stack std::array here is the
        // single largest contributor to this function's frame size.
        std::vector<std::array<double, 256>> coeffs(kMaxSubstreamStreams);
        // §7.3.4, same split as decoder.cpp's own read_stream: only a stream
        // with its OWN dithflag (a full-bandwidth channel, s < nfchans)
        // dithers here. The LFE has no dithflag and always reconstructs as
        // zero; kCplStream's shared bins stay silent here too and are
        // dithered per receiving channel in the decoupling loop below
        // instead, per §7.3.4's "applied after the individual channels are
        // extracted ... uncorrelated" requirement.
        const auto read_stream = [&](int s, int begin) {
            const auto index = static_cast<std::size_t>(s);
            const bool dither_eligible = s < nfchans && dithflag[static_cast<std::size_t>(s)];
            for (int bin = begin; bin < endmant[index]; ++bin) {
                const int bap_value = bap[index][static_cast<std::size_t>(bin)];
                const int exp = exps[index][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    coeffs[index][static_cast<std::size_t>(bin)] =
                        dither_eligible ? dither_.next() / static_cast<double>(1u << exp) : 0.0;
                    continue;
                }
                const auto code = mantissa_reader.read(r, bap_value);
                coeffs[index][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa(code, bap_value) / static_cast<double>(1u << exp);
            }
        };

        // §3.4.4 + §3.4.5: an AHT stream's mantissas exist only in block 0 -
        // one gaqmod, its gain words, then per bin a VQ index (hebap 1-7) or
        // six gain-adaptively-quantized codewords (hebap 8-19), covering all
        // six blocks at once. `bap[s]` already holds hebap, not ordinary bap,
        // because its BitAllocRegion was built with high_efficiency=true.
        const auto decode_aht_stream = [&](int s, int begin) -> std::expected<void, DecodeError> {
            const auto us = static_cast<std::size_t>(s);
            const int end = endmant[us];
            const auto& hebap = bap[us];

            const auto gaqmod = static_cast<int>(r.read(2));
            std::vector<int> gain(static_cast<std::size_t>(end), 1);  // default Gk=1
            std::vector<int> gain_carrying_bins;
            for (int bin = begin; bin < end; ++bin) {
                if (eac3::aht_gaq_has_gain(hebap[static_cast<std::size_t>(bin)], gaqmod)) {
                    gain_carrying_bins.push_back(bin);
                }
            }
            if (gaqmod == 3) {
                // Table E3.4, base-3 unpacked: three three-state gains to a
                // 5-bit word, most significant first - the mirror image of
                // the encoder's packing.
                for (std::size_t i = 0; i < gain_carrying_bins.size(); i += 3) {
                    const auto packed = r.read(5);
                    const std::array<std::uint32_t, 3> mapped = {
                        packed / 9, (packed % 9) / 3, (packed % 9) % 3};
                    for (std::size_t t = 0;
                         t < 3 && i + t < gain_carrying_bins.size(); ++t) {
                        gain[static_cast<std::size_t>(gain_carrying_bins[i + t])] =
                            eac3::aht_gaq_gain_from_mapped(static_cast<int>(mapped[t]));
                    }
                }
            } else if (gaqmod != 0) {
                const int alt = gaqmod == 1 ? 2 : 4;
                for (const int bin : gain_carrying_bins) {
                    gain[static_cast<std::size_t>(bin)] = r.read(1) != 0 ? alt : 1;
                }
            }

            for (int bin = begin; bin < end; ++bin) {
                const auto ubin = static_cast<std::size_t>(bin);
                const int hb = hebap[ubin];
                std::array<double, kBlocksPerFrame> mantissas{};
                if (hb >= 1 && hb <= 7) {
                    const auto book = tables::aht_vq_table(hb);
                    const auto index = r.read(eac3::aht_bin_bits(hb));
                    if (index >= book.size()) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        mantissas[j] = static_cast<double>(book[index][j]) / 32768.0;
                    }
                } else if (hb >= 8) {
                    const int mantissa_bits = eac3::aht_mantissa_bits(hb);
                    // hebap is clamped to kHeBapTab's 0..19 range inside
                    // compute_bit_allocation, so this always holds for
                    // hb >= 8 - matching the invariant aht_quantize_mantissa
                    // (the encode direction) already asserts on the same
                    // grounds, rather than a second, redundant runtime check.
                    // The assert alone does not satisfy the static analyzer
                    // in a build where it compiles out (NDEBUG), hence the
                    // NOLINT below on the same proven-safe grounds.
                    assert(mantissa_bits >= 3);
                    const int g = gain[ubin];
                    const int small_bits =
                        g == 1 ? mantissa_bits : (g == 2 ? mantissa_bits - 1 : mantissa_bits - 2);
                    const int large_bits = g == 2 ? mantissa_bits - 1 : mantissa_bits;
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        const auto raw = r.read(small_bits);
                        bool has_escape = false;
                        std::uint32_t escape = 0;
                        // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift)
                        if (g != 1 && raw == (1u << (small_bits - 1))) {
                            has_escape = true;
                            escape = r.read(large_bits);
                        }
                        mantissas[j] = eac3::aht_dequantize_mantissa(raw, escape, has_escape,
                                                                     mantissa_bits, g);
                    }
                }
                // hb == 0: mantissas stays all-zero.
                std::array<double, kBlocksPerFrame> blocks{};
                eac3::aht_inverse(mantissas, blocks);
                const int exp = exps[us][ubin];
                for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                    aht_coeffs[us][j][ubin] = std::ldexp(blocks[j], -exp);
                }
            }
            return {};
        };
        const auto read_stream_dispatch = [&](int s, int begin) -> std::expected<void, DecodeError> {
            const auto us = static_cast<std::size_t>(s);
            if (frm->ahtinu[us]) {
                if (blk == 0) {
                    if (const auto result = decode_aht_stream(s, begin); !result) {
                        return result;
                    }
                }
                coeffs[us] = aht_coeffs[us][static_cast<std::size_t>(blk)];
                return {};
            }
            read_stream(s, begin);
            return {};
        };

        bool read_coupling = false;
        for (int ch = 0; ch < nfchans; ++ch) {
            if (const auto result = read_stream_dispatch(ch, 0); !result) {
                return std::unexpected(result.error());
            }
            if (frm->cplinu[static_cast<std::size_t>(blk)] &&
                chincpl[static_cast<std::size_t>(ch)] && !read_coupling) {
                if (const auto result = read_stream_dispatch(kCplStream, cplstrtmant); !result) {
                    return std::unexpected(result.error());
                }
                read_coupling = true;
            }
        }
        if (bsi->lfe) {
            if (const auto result = read_stream_dispatch(nfchans, 0); !result) {
                return std::unexpected(result.error());
            }
        }

        // §7.4.3 decoupling: each coupled channel's high band is the shared
        // channel scaled by that channel's coordinate, times 8 - undoing the
        // encoder's /8 headroom scaling. Standard coupling only: it has no
        // neighbor-block dependency, so it finishes right here, same as
        // every other tool. Enhanced coupling's own reconstruction (§3.5.5)
        // needs the block AFTER this one, which the bitstream has not
        // reached yet - so it, and everything that runs after decoupling
        // for EVERY block (spx synthesis, rematrixing, IMDCT), is deferred
        // to a second pass below that runs once every block has been parsed
        // and can therefore look at any block's neighbors freely, still in
        // strict block order (IMDCT's overlap-add delay line requires that
        // regardless of coupling mode).
        if (frm->cplinu[static_cast<std::size_t>(blk)] && !ecplinu_now) {
            const auto& shared = coeffs[static_cast<std::size_t>(kCplStream)];
            const auto& cpl_bap = bap[static_cast<std::size_t>(kCplStream)];
            const auto& cpl_exps = exps[static_cast<std::size_t>(kCplStream)];
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& target = coeffs[static_cast<std::size_t>(ch)];
                const bool ch_dither = dithflag[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < static_cast<int>(subband_band.size()); ++bnd) {
                    const double coordinate =
                        cplco[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bnd)];
                    // §7.4.1: a set phase flag negates the right channel of a
                    // 2/0 pair across that band, restoring the phase the
                    // coupling sum discarded.
                    const double sign =
                        (phsflginu && ch == 1 &&
                         phsflg[static_cast<std::size_t>(
                             subband_band[static_cast<std::size_t>(bnd)])])
                            ? -1.0
                            : 1.0;
                    const int low = cplstrtmant + bnd * coupling::kBinsPerSubBand;
                    const int high = std::min(low + coupling::kBinsPerSubBand, cplendmant);
                    for (int bin = low; bin < high; ++bin) {
                        const std::size_t ubin = static_cast<std::size_t>(bin);
                        // §7.3.4: independent per-channel dither for a
                        // zero-bap shared bin, run through the same
                        // extraction formula a real coupling coefficient
                        // uses - see decoder.cpp's own copy of this comment
                        // for why reusing one dithered coupling-domain
                        // sample across channels would be wrong.
                        const double coeff =
                            (cpl_bap[ubin] == 0 && ch_dither)
                                ? dither_.next() / static_cast<double>(1u << cpl_exps[ubin])
                                : shared[ubin];
                        target[ubin] = coeff * coordinate * 8.0 * sign;
                    }
                }
            }
        }

        // Stash this block's raw enhanced coupling channel mantissas
        // regardless of mode - a NEIGHBORING block that used enhanced
        // coupling needs them even when this block did not (§3.5.5.1's own
        // zero-substitution rule reads them via ecpl_active below).
        if (frm->cplinu[static_cast<std::size_t>(blk)] && ecplinu_now) {
            ecpl_all_coeffs[static_cast<std::size_t>(blk)] =
                coeffs[static_cast<std::size_t>(kCplStream)];
            ecpl_active[static_cast<std::size_t>(blk)] = true;
        }

        // Snapshot everything the second pass needs to finish this block.
        auto& tail = tails[static_cast<std::size_t>(blk)];
        tail.coeffs = std::move(coeffs);
        tail.chincpl = chincpl;
        tail.cplinu = frm->cplinu[static_cast<std::size_t>(blk)];
        tail.ecplinu_now = ecplinu_now;
        if (tail.cplinu && tail.ecplinu_now) {
            tail.firstchincpl = -1;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (chincpl[static_cast<std::size_t>(ch)]) {
                    tail.firstchincpl = ch;
                    break;
                }
            }
            tail.ecpl_begin_subbnd = ecpl_begin_subbnd;
            tail.ecpl_end_subbnd = ecpl_end_subbnd;
            tail.ecpl_structure = ecpl_structure;
            tail.ecplamp_raw = ecplamp_raw;
            tail.ecplangle_raw = ecplangle_raw;
            tail.ecplchaos_raw = ecplchaos_raw;
            tail.ecpltrans = ecpltrans_persist;
        }
        tail.cplstrtmant = cplstrtmant;
        tail.cplendmant = cplendmant;
        tail.spxinu = spxinu;
        tail.chinspx = chinspx;
        tail.spx_bands = spx_bands;
        tail.spxco = spxco;
        tail.spxblnd = spxblnd;
        tail.spx_startmant = spx_startmant;
        tail.spx_endmant = spx_endmant;
        tail.spx_copystart = spx_copystart;
        tail.rematflg = rematflg;
        tail.blksw = blksw;
        tail.endmant = endmant;

        if (r.overflowed()) {
            return std::unexpected(DecodeError::kTruncated);
        }
    }

    // Second pass: finish every block in order. Standard-coupled, plain and
    // AHT channels already carry their final coefficients from pass one
    // above; only enhanced coupling's own reconstruction happens here, right
    // before the spx/rematrix/IMDCT tail every block goes through.
    for (int blk = 0; blk < nblks; ++blk) {
        auto& tail = tails[static_cast<std::size_t>(blk)];
        auto& coeffs = tail.coeffs;

        if (tail.cplinu && tail.ecplinu_now) {
            // §3.5.5: reconstruct each coupled channel from the enhanced
            // coupling channel, using this block's neighbors. A neighbor is
            // zero when the adjacent block did not use enhanced coupling
            // (§3.5.5.1's own rule) - which includes this syncframe's first
            // and last block, whose true neighbor lives in an adjacent
            // syncframe this call was not given (see this function's
            // comment on `prev_ecpl_coeffs` above).
            static constexpr std::array<double, 256> kZero{};
            const auto& prev = (blk > 0 && ecpl_active[static_cast<std::size_t>(blk - 1)])
                                   ? ecpl_all_coeffs[static_cast<std::size_t>(blk - 1)]
                                   : kZero;
            const auto& next =
                (blk + 1 < nblks && ecpl_active[static_cast<std::size_t>(blk + 1)])
                    ? ecpl_all_coeffs[static_cast<std::size_t>(blk + 1)]
                    : kZero;
            auto& zr = ecpl_spectrum_real_;
            auto& zi = ecpl_spectrum_imag_;
            eac3::ecpl_channel_spectrum(
                prev, ecpl_all_coeffs[static_cast<std::size_t>(blk)], next, zr, zi);

            const int bins = tail.cplendmant - tail.cplstrtmant;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!tail.chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                const auto uch = static_cast<std::size_t>(ch);
                const bool is_first = ch == tail.firstchincpl;
                std::vector<double> amp_bin(static_cast<std::size_t>(bins));
                std::vector<double> angle_bin(static_cast<std::size_t>(bins));
                eac3::ecpl_amplitudes(tail.ecplamp_raw[uch], tail.ecplchaos_raw[uch],
                                      tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                      tail.ecpl_end_subbnd, tail.ecpl_structure, amp_bin);
                eac3::ecpl_angles(ch, tail.ecplangle_raw[uch], tail.ecplchaos_raw[uch],
                                  tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                  tail.ecpl_end_subbnd, tail.ecpl_structure, ecpl_noise,
                                  angle_bin);
                eac3::ecpl_channel_coefficients(zr, zi, amp_bin, angle_bin, tail.cplstrtmant,
                                                tail.cplendmant, coeffs[uch]);
            }
        }

        // §3.6.4 spectral extension synthesis: translate the low band up,
        // notch the seams, blend with noise to approximate the original
        // band's coarse energy, then scale by the transmitted coordinate.
        // Runs after decoupling, so a channel that is both coupled and
        // extended already has its coupling-restored content in place below
        // spx_startmant to copy from - coupling always ends exactly where
        // spx begins (§E3.3.1), so there is no gap and nothing to reconcile.
        if (tail.spxinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!tail.chinspx[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& tc = coeffs[static_cast<std::size_t>(ch)];

                // §3.6.4.1 Transform Coefficient Translation: copy low-band
                // coefficients up into the extension region, banded, wrapping
                // the copy source back to spx_copystart whenever a band would
                // run past spx_startmant. copyindex never leaves
                // [spx_copystart, spx_startmant) - strictly below the region
                // this loop writes into - so mutating tc in place is safe.
                std::array<bool, eac3::kMaxSubBands> wrapflag{};
                std::array<double, eac3::kMaxSubBands> band_rms{};
                int copyindex = tail.spx_copystart;
                for (int bnd = 0; bnd < tail.spx_bands.count; ++bnd) {
                    const auto ubnd = static_cast<std::size_t>(bnd);
                    const int size = tail.spx_bands.size[ubnd];
                    const int low = tail.spx_bands.start[ubnd];
                    if (copyindex + size > tail.spx_startmant) {
                        copyindex = tail.spx_copystart;
                        wrapflag[ubnd] = true;
                    }
                    double accum = 0.0;
                    for (int i = 0; i < size; ++i) {
                        if (copyindex == tail.spx_startmant) {
                            copyindex = tail.spx_copystart;
                        }
                        const double value = tc[static_cast<std::size_t>(copyindex++)];
                        tc[static_cast<std::size_t>(low + i)] = value;
                        accum += value * value;
                    }
                    band_rms[ubnd] = std::sqrt(accum / size);
                }

                // §3.6.4.2.3 Band Border Filtering: the notch runs on the
                // already-translated, not-yet-blended region, using RMS
                // measured before it (matching the encoder's own order).
                eac3::spx_apply_notch(
                    std::span{tc}.subspan(
                        static_cast<std::size_t>(tail.spx_startmant),
                        static_cast<std::size_t>(tail.spx_endmant - tail.spx_startmant)),
                    tail.spx_startmant, tail.spx_bands, wrapflag,
                    frm->spxattencod[static_cast<std::size_t>(ch)]);

                // §3.6.4.2.4 Noise Scaling and Blending, then §3.6.4.3
                // Blended Transform Coefficient Scaling.
                const int blend = tail.spxblnd[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < tail.spx_bands.count; ++bnd) {
                    const auto ubnd = static_cast<std::size_t>(bnd);
                    const int size = tail.spx_bands.size[ubnd];
                    const int low = tail.spx_bands.start[ubnd];
                    const double nratio =
                        eac3::spx_noise_ratio(low, size, tail.spx_endmant, blend);
                    const double nscale = band_rms[ubnd] * std::sqrt(nratio);
                    const double sscale = std::sqrt(1.0 - nratio);
                    const double coordinate = tail.spxco[static_cast<std::size_t>(ch)][ubnd] * 32.0;
                    for (int i = 0; i < size; ++i) {
                        const auto at = static_cast<std::size_t>(low + i);
                        tc[at] = (tc[at] * sscale + spx_noise.next() * nscale) * coordinate;
                    }
                }
            }
        }

        if (bsi->acmod == Acmod::k2_0) {
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, up to the
            // lower bandwidth of the two channels.
            const int cap = std::min(tail.endmant[0], tail.endmant[1]) - 1;
            for (std::size_t band = 0; band < kRematrixBands.size(); ++band) {
                if (!tail.rematflg[band]) {
                    continue;
                }
                const int high = std::min(kRematrixBands[band][1], cap);
                for (int bin = kRematrixBands[band][0]; bin <= high; ++bin) {
                    const double l = coeffs[0][static_cast<std::size_t>(bin)];
                    const double rr = coeffs[1][static_cast<std::size_t>(bin)];
                    coeffs[0][static_cast<std::size_t>(bin)] = l + rr;
                    coeffs[1][static_cast<std::size_t>(bin)] = l - rr;
                }
            }
        }

        for (int ch = 0; ch < nchans; ++ch) {
            const auto index = static_cast<std::size_t>(ch);
            auto& x = imdct_scratch_;
            if (ch < nfchans && tail.blksw[static_cast<std::size_t>(ch)]) {
                imdct256_pair_windowed(coeffs[index], x);
            } else {
                imdct512_windowed(coeffs[index], x);
            }
            auto& history = delay[index];
            auto& pcm = out.channels[index];
            for (int n = 0; n < kSamplesPerBlock; ++n) {
                pcm[static_cast<std::size_t>(blk * kSamplesPerBlock + n)] =
                    static_cast<float>(2.0 * (x[static_cast<std::size_t>(n)] +
                                              history[static_cast<std::size_t>(n)]));
                history[static_cast<std::size_t>(n)] = x[static_cast<std::size_t>(256 + n)];
            }
        }
    }

    // §3.7: apply any transient pre-noise correction THIS frame's fields
    // specify, against this frame's own head plus whatever the previous
    // frame (still held back in `pending_`, if any) contributed as its
    // tail - the only combination a correction can ever need, because
    // transprocloc is relative to this frame's own first sample and this
    // decoder keeps exactly one frame of lookback (see decode_substream's
    // own doc comment; a stream needing more is refused rather than read
    // out of bounds).
    const int key = static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid;
    auto pending_it = pending_.find(key);
    if (frm->transproce) {
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto uch = static_cast<std::size_t>(ch);
            if (!frm->chintransproc[uch]) {
                continue;
            }
            const int transloc = frm->transprocloc[uch];
            const int translen = frm->transproclen[uch];
            const auto range = transient_prenoise_range(transloc, translen);
            if (range.first < -kSamplesPerFrame || range.last > kSamplesPerFrame) {
                // Reaches further back or forward than the one frame of
                // history/lookahead this decoder buffers - recognised,
                // refused, not misdecoded (same stance as every other
                // syntax this project's own encoder does not exercise).
                return std::unexpected(DecodeError::kUnsupported);
            }
            std::vector<float> combined(static_cast<std::size_t>(kSamplesPerFrame) * 2);
            if (pending_it != pending_.end()) {
                std::ranges::copy(pending_it->second.channels[uch], combined.begin());
            }
            std::ranges::copy(out.channels[uch], combined.begin() + kSamplesPerFrame);
            apply_transient_prenoise(combined, kSamplesPerFrame + transloc, translen);
            if (pending_it != pending_.end()) {
                std::ranges::copy(combined.begin(), combined.begin() + kSamplesPerFrame,
                                  pending_it->second.channels[uch].begin());
            }
            std::ranges::copy(combined.begin() + kSamplesPerFrame, combined.end(),
                              out.channels[uch].begin());
        }
    }

    if (pending_it != pending_.end()) {
        DecodedSubstream ready = std::move(pending_it->second);
        pending_it->second = std::move(out);
        return std::optional<DecodedSubstream>(std::move(ready));
    }
    if (frm->transproce) {
        // First frame to use the tool for this substream identity: hold it
        // back, nothing is ready to return yet - see decode_substream's own
        // doc comment.
        pending_.emplace(key, std::move(out));
        return std::optional<DecodedSubstream>(std::nullopt);
    }
    return std::optional<DecodedSubstream>(std::move(out));
}

std::vector<DecodedSubstream> Eac3Decoder::flush() {
    std::vector<DecodedSubstream> ready;
    ready.reserve(pending_.size() + pending_au_parts_.size());
    for (auto& [key, substream] : pending_) {
        ready.push_back(std::move(substream));
    }
    pending_.clear();
    // decode_access_unit's own assembly cache: whatever is left here is one
    // or more substreams whose sibling(s) never caught up before the stream
    // ended, so there is no complete DecodedAccessUnit to hand back for
    // them - the raw substreams, oldest first, are the best this can do (see
    // flush()'s own doc comment).
    for (auto& [key, queue] : pending_au_parts_) {
        for (auto& substream : queue) {
            ready.push_back(std::move(substream));
        }
    }
    pending_au_parts_.clear();
    return ready;
}

std::expected<std::optional<DecodedAccessUnit>, DecodeError> Eac3Decoder::decode_access_unit(
    std::span<const std::byte> unit) {
    const auto frames = split_frames(unit);
    if (!frames) {
        return std::unexpected(frames.error());
    }
    if (frames->empty()) {
        return std::unexpected(DecodeError::kInvalidStream);
    }

    // §3.7: each frame's substream identity is needed below regardless of
    // whether decode_substream releases it or holds it back this call - a
    // held-back frame has no DecodedSubstream to read strmtyp/substreamid
    // from, so bsi is parsed here too. This is the same parse
    // decode_substream itself does a moment later; cheap enough that
    // duplicating it beats threading the key back out through decode_substream's
    // own return type.
    std::vector<int> keys;
    keys.reserve(frames->size());
    for (const auto& frame : *frames) {
        BitReader peek{frame};
        const auto bsi = parse_bsi(peek, frame.size());
        if (!bsi) {
            return std::unexpected(bsi.error());
        }
        keys.push_back(static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid);

        auto decoded = decode_substream(frame);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (decoded->has_value()) {
            pending_au_parts_[keys.back()].push_back(std::move(**decoded));
        }
        // A held-back frame adds nothing to this identity's queue - whatever
        // it already holds (if anything, from an earlier call) is still
        // waiting in order, and remains what completes the assembly below
        // once every other identity also has one queued.
    }

    // Every identity this call's frames named must have at least one queued,
    // released result before there is a complete access unit to assemble. A
    // stream that never uses transient pre-noise processing always does:
    // every substream releases every call, so this is never false for it.
    for (const int key : keys) {
        const auto it = pending_au_parts_.find(key);
        if (it == pending_au_parts_.end() || it->second.empty()) {
            return std::optional<DecodedAccessUnit>(std::nullopt);
        }
    }
    std::vector<DecodedSubstream> substreams;
    substreams.reserve(keys.size());
    for (const int key : keys) {
        auto& queue = pending_au_parts_.at(key);
        substreams.push_back(std::move(queue.front()));
        queue.pop_front();
    }
    const auto& lead = substreams.front();
    if (lead.strmtyp == StreamType::kDependent) {
        return std::unexpected(DecodeError::kInvalidStream);
    }
    for (std::size_t i = 1; i < substreams.size(); ++i) {
        // Every substream of a program codes the same samples of the same
        // audio, so a dependent that disagrees with its parent about the rate
        // or the block count desynchronises the program silently rather than
        // failing to parse - which is exactly why it is checked here.
        const auto& sub = substreams[i];
        if (sub.strmtyp != StreamType::kDependent || sub.sample_rate != lead.sample_rate ||
            sub.numblkscod != lead.numblkscod) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
    }

    DecodedAccessUnit out;
    out.sample_rate = lead.sample_rate;
    out.acmod = lead.acmod;
    out.dialnorm = lead.dialnorm;
    out.compr = lead.compr;
    out.substream_count = static_cast<int>(substreams.size());

    // Dual mono has no Table E2.5 location - Ch1 and Ch2 are unrelated
    // programmes, not directions - and it has no bed/dependent split to make:
    // 1+1 is always this one lone independent substream. acmod_map() has a
    // placeholder L/R entry for it purely so channel-count bookkeeping
    // elsewhere still adds up; consulting
    // it here would mislabel Ch2 as a right channel, which is exactly the
    // "not a pair" distinction dual mono exists to preserve. So: pass the
    // substream's own two channels straight through in coded order, and leave
    // `layout` empty to say plainly that there is no spatial layout to report.
    if (lead.acmod == Acmod::kDualMono) {
        out.channels = lead.channels;
        return std::optional<DecodedAccessUnit>(std::move(out));
    }

    // §E3.8.2: the bed's locations, then every dependent's unioned in. A
    // dependent's channels that correspond to the independent's REPLACE them;
    // the rest extend the layout.
    std::uint16_t occupied = 0;
    for (const auto& sub : substreams) {
        occupied = static_cast<std::uint16_t>(occupied | sub.location_map());
    }
    out.layout = eac3::chanmap::expand(occupied);
    // §E3.8.2 caps a single program at 16 rendered channels.
    if (out.layout.count > 16) {
        return std::unexpected(DecodeError::kInvalidStream);
    }
    const std::size_t samples = lead.channels.empty() ? 0 : lead.channels.front().size();
    out.channels.assign(static_cast<std::size_t>(out.layout.count),
                        std::vector<float>(samples, 0.0f));

    // Transmission order is overwrite order, so a later dependent wins the
    // locations it shares with an earlier substream.
    for (const auto& sub : substreams) {
        const auto locations = eac3::chanmap::expand(sub.location_map());
        if (static_cast<std::size_t>(locations.count) != sub.channels.size()) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        for (int i = 0; i < locations.count; ++i) {
            const int slot = out.layout.index_of(locations[i]);
            if (slot < 0) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            out.channels[static_cast<std::size_t>(slot)] =
                sub.channels[static_cast<std::size_t>(i)];
        }
    }
    return std::optional<DecodedAccessUnit>(std::move(out));
}

}  // namespace ac3
