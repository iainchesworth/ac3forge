#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <array>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"

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
    // drags one in. Either way the 8 bits have to be consumed.
    bsi.compre = r.read(1) != 0;
    if (bsi.compre) {
        r.skip(8);  // compr
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
};

std::expected<AudFrm, DecodeError> parse_audfrm(BitReader& r, const Bsi& bsi, int nblks) {
    const int nfchans = fullbw_channel_count(bsi.acmod);
    AudFrm frm;
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

    // Coupling-in-use for every block is decided here, ahead of the blocks.
    if (static_cast<std::uint8_t>(bsi.acmod) > 0x1) {
        bool cplinu = r.read(1) != 0;  // cplstre[0] is an implied 1
        bool any = cplinu;
        for (int blk = 1; blk < nblks; ++blk) {
            if (r.read(1) != 0) {  // cplstre[blk]
                cplinu = r.read(1) != 0;
            }
            any = any || cplinu;
        }
        if (any) {
            return std::unexpected(DecodeError::kUnsupported);
        }
    }

    if (expstre) {
        for (int blk = 0; blk < nblks; ++blk) {
            for (int ch = 0; ch < nfchans; ++ch) {
                frm.chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] =
                    static_cast<ExpStrategy>(r.read(2));
            }
        }
    } else {
        // Table E2.10: one 5-bit code per channel expands to all six blocks.
        // frmcplexpstr is absent because no block couples - it is conditional
        // on ncplblks > 0, and coupling was refused above.
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
        // The adaptive hybrid transform re-codes six blocks of mantissas as
        // one 1536-point transform with its own gain-adaptive quantizer. It is
        // a different mantissa format, not a variation on this one.
        return std::unexpected(DecodeError::kUnsupported);
    }
    if (frm.snroffststr == 0x0) {
        frm.frmcsnroffst = static_cast<int>(r.read(6));
        frm.frmfsnroffst = static_cast<int>(r.read(4));
    }
    if (frm.transproce) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {  // chintransproc[ch]
                r.skip(10 + 8);    // transprocloc, transproclen
            }
        }
        return std::unexpected(DecodeError::kUnsupported);
    }
    if (spxattene) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {  // chinspxatten[ch]
                r.skip(5);         // spxattencod[ch]
            }
        }
    }
    if (bsi.numblkscod != 0x0 && r.read(1) != 0) {  // blkstrtinfoe
        r.skip(static_cast<std::size_t>(eac3::block_start_info_bits(nblks, bsi.words)));
    }
    return frm;
}

}  // namespace

std::expected<DecodedSubstream, DecodeError> Eac3Decoder::decode_substream(
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
    out.dialnorm2 = bsi->dialnorm2;
    out.compr2 = bsi->compr2;
    out.numblkscod = bsi->numblkscod;
    out.chanmap = bsi->chanmap;
    out.last_dependent = bsi->strmtyp == StreamType::kDependent && bsi->compre;
    out.channels.assign(static_cast<std::size_t>(nchans),
                        std::vector<float>(static_cast<std::size_t>(nblks * kSamplesPerBlock),
                                           0.0f));

    // §E2.3.1.2: a dependent's substreamid starts again at 0 in its own space,
    // so identity - and hence which overlap-add history belongs to this frame
    // - is the pair, never the id alone.
    auto& delay = delay_[static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid];

    std::array<int, kMaxSubstreamChannels> endmant{};
    std::array<std::vector<std::uint8_t>, kMaxSubstreamChannels> exps;
    std::array<std::vector<std::uint8_t>, kMaxSubstreamChannels> bap;
    BitAllocCodes codes = kBamode0Codes;
    std::array<int, kMaxSubstreamChannels> fgaincod{};
    fgaincod.fill(kBamode0Codes.fgaincod);
    std::array<int, kMaxSubstreamChannels> fsnroffst{};
    int csnroffst = 0;
    std::array<bool, 4> rematflg{};
    // §7.2.2.6, reset to "no segments" at the start of every syncframe like
    // fsnroffst/codes above, then persisting block to block until
    // re-transmitted or cleared. Coupling is unsupported here (spxinu/cplinu
    // already error before this point), so unlike the AC-3 decoder there is
    // no coupling-channel slot to carry - only the per-fbw-channel deltbae[ch].
    std::array<DeltaSegments, kMaxSubstreamChannels> delta{};

    for (int blk = 0; blk < nblks; ++blk) {
        const auto strategy = [&](int ch) {
            return ch < nfchans
                       ? frm->chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)]
                       : frm->lfeexpstr[static_cast<std::size_t>(blk)];
        };

        if (frm->blkswe) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (r.read(1) != 0) {  // blksw[ch]
                    return std::unexpected(DecodeError::kUnsupported);
                }
            }
        }
        if (frm->dithflage) {
            // bap-0 bins reconstruct as zero whatever this says, matching the
            // AC-3 path: §7.3.4 lets the dither sequence be "any reasonably
            // random sequence", so zeros are what keeps decode deterministic.
            r.skip(static_cast<std::size_t>(nfchans));
        }
        if (r.read(1) != 0) {
            r.skip(8);  // dynrng: parsed, not applied
        }
        if (bsi->acmod == Acmod::kDualMono && r.read(1) != 0) {
            r.skip(8);  // dynrng2e -> dynrng2: parsed, not applied - same as dynrng
        }

        // Spectral extension. Block 0's strategy is implied rather than sent.
        const bool spxstre = blk == 0 || r.read(1) != 0;
        if (spxstre && r.read(1) != 0) {  // spxinu
            return std::unexpected(DecodeError::kUnsupported);
        }
        // cplstre[blk] carries no payload with cplinu clear, and coupling was
        // refused in audfrm, so nothing is read here.

        if (bsi->acmod == Acmod::k2_0) {
            // Unlike AC-3, block 0's rematstr is IMPLIED 1 rather than
            // transmitted; only later blocks carry the bit.
            if (blk == 0 || r.read(1) != 0) {
                // nrematbd is 4 with neither coupling nor spectral extension.
                for (auto& flag : rematflg) {
                    flag = r.read(1) != 0;
                }
            }
        }

        // chbwcod accompanies a fresh strategy; the strategies came in audfrm.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (strategy(ch) == ExpStrategy::kReuse) {
                continue;
            }
            const auto chbwcod = r.read(6);
            if (chbwcod > 60) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            endmant[static_cast<std::size_t>(ch)] = ((static_cast<int>(chbwcod) + 12) * 3) + 37;
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
        // cplleake is gated on cplinu, which is clear.
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
                                    .delta = delta[static_cast<std::size_t>(ch)]});
        }

        // Mantissas, in coded order: the full-bandwidth channels and then the
        // LFE, with no coupling channel to interleave.
        MantissaBlockReader mantissa_reader;
        // Heap-backed, matching decoder.cpp's own per-block coeffs: at
        // kMaxSubstreamChannels * 256 doubles, a stack std::array here is the
        // single largest contributor to this function's frame size.
        std::vector<std::array<double, 256>> coeffs(kMaxSubstreamChannels);
        for (int ch = 0; ch < nchans; ++ch) {
            const auto index = static_cast<std::size_t>(ch);
            for (int bin = 0; bin < endmant[index]; ++bin) {
                const int bap_value = bap[index][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    continue;  // silence (dither substitution not implemented)
                }
                const auto code = mantissa_reader.read(r, bap_value);
                const int exp = exps[index][static_cast<std::size_t>(bin)];
                coeffs[index][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa(code, bap_value) / static_cast<double>(1u << exp);
            }
        }

        if (bsi->acmod == Acmod::k2_0) {
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, up to the
            // lower bandwidth of the two channels.
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
            const auto index = static_cast<std::size_t>(ch);
            std::array<double, 512> x{};
            imdct512_windowed(coeffs[index], x);
            auto& history = delay[index];
            auto& pcm = out.channels[index];
            for (int n = 0; n < kSamplesPerBlock; ++n) {
                pcm[static_cast<std::size_t>(blk * kSamplesPerBlock + n)] =
                    static_cast<float>(2.0 * (x[static_cast<std::size_t>(n)] +
                                              history[static_cast<std::size_t>(n)]));
                history[static_cast<std::size_t>(n)] = x[static_cast<std::size_t>(256 + n)];
            }
        }
        if (r.overflowed()) {
            return std::unexpected(DecodeError::kTruncated);
        }
    }
    return out;
}

std::expected<DecodedAccessUnit, DecodeError> Eac3Decoder::decode_access_unit(
    std::span<const std::byte> unit) {
    const auto frames = split_frames(unit);
    if (!frames) {
        return std::unexpected(frames.error());
    }
    if (frames->empty()) {
        return std::unexpected(DecodeError::kInvalidStream);
    }

    std::vector<DecodedSubstream> substreams;
    substreams.reserve(frames->size());
    for (const auto& frame : *frames) {
        auto decoded = decode_substream(frame);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        substreams.push_back(std::move(*decoded));
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
        return out;
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
    return out;
}

}  // namespace ac3
