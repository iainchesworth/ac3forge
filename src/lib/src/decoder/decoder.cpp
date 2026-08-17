#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/meta/drc.hpp"
#include "gain.hpp"

namespace ac3 {

// Each case says what is wrong with the stream, except kUnsupported, which
// says what is wrong with this decoder — the difference decides whether the
// caller should reach for another file or another tool.
std::string_view describe(DecodeError error) {
    switch (error) {
        case DecodeError::kTruncated: return "the stream ends part-way through a frame";
        case DecodeError::kBadSyncWord:
            return "no 0x0B77 sync word where a frame should begin";
        case DecodeError::kBadCrc: return "the frame's CRC does not check out";
        case DecodeError::kReservedValue: return "a header field holds a value A/52 reserves";
        case DecodeError::kUnsupported:
            return "valid AC-3 this decoder does not implement (bsid > 8)";
        case DecodeError::kInvalidStream:
            return "the frame contradicts a constraint A/52 requires of it";
    }
    return "unknown decode error";
}

namespace {

// Byte length of the syncframe at `offset`, whichever generation it is.
// AC-3 and E-AC-3 both put bsid at bit 40 - deliberately, so that a reader
// can tell the two apart before committing to a layout - but they express the
// size completely differently: AC-3 looks frmsizecod up in Table 5.18, while
// E-AC-3 states the word count outright in the 11-bit frmsiz.
std::expected<std::size_t, DecodeError> syncframe_bytes(std::span<const std::byte> stream,
                                                        std::size_t offset) {
    if (offset + 6 > stream.size()) {
        return std::unexpected(DecodeError::kTruncated);
    }
    const auto byte = [&](std::size_t i) {
        return std::to_integer<std::uint32_t>(stream[offset + i]);
    };
    if (byte(0) != 0x0B || byte(1) != 0x77) {
        return std::unexpected(DecodeError::kBadSyncWord);
    }
    const auto bsid = byte(5) >> 3;
    if (bsid >= eac3::kMinDecodableBsid && bsid <= eac3::kBsid) {
        const std::size_t frmsiz = ((byte(2) & 0x07) << 8) | byte(3);
        const std::size_t bytes = (frmsiz + 1) * 2;
        // frmsiz is free to say anything, including a size that does not even
        // cover the six header bytes just read. Callers index into the spans
        // this hands back, so a self-contradictory size is rejected here
        // rather than becoming a short span someone reads past.
        if (bytes < 6) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        return bytes;
    }
    if (bsid > 8) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    const auto fscod = byte(4) >> 6;
    const auto frmsizecod = byte(4) & 0x3F;
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    const std::uint32_t kbps = kBitratesKbps[frmsizecod >> 1];
    // kbps came from the very table frame_size_bytes -> bitrate_index
    // searches for an exact match, so the lookup inside it always succeeds.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *frame_size_bytes(static_cast<SampleRate>(fscod), kbps, (frmsizecod & 1) != 0);
}

}  // namespace

std::expected<int, DecodeError> stream_bsid(std::span<const std::byte> frame) {
    if (frame.size() < 6) {
        return std::unexpected(DecodeError::kTruncated);
    }
    return static_cast<int>(std::to_integer<std::uint32_t>(frame[5]) >> 3);
}

std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_frames(
    std::span<const std::byte> stream) {
    std::vector<std::span<const std::byte>> frames;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const auto bytes = syncframe_bytes(stream, offset);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (offset + *bytes > stream.size()) {
            return std::unexpected(DecodeError::kTruncated);
        }
        frames.push_back(stream.subspan(offset, *bytes));
        offset += *bytes;
    }
    return frames;
}

std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_access_units(
    std::span<const std::byte> stream) {
    const auto frames = split_frames(stream);
    if (!frames) {
        return std::unexpected(frames.error());
    }
    // An access unit is its substreams concatenated, so it is delimited rather
    // than framed: a new one starts wherever an independent substream does,
    // and everything up to the next one belongs to it.
    std::vector<std::span<const std::byte>> units;
    std::size_t start = 0;
    std::size_t offset = 0;
    for (const auto& frame : *frames) {
        const auto strmtyp = static_cast<eac3::StreamType>(
            std::to_integer<std::uint32_t>(frame[2]) >> 6);
        const bool begins_unit = strmtyp == eac3::StreamType::kIndependent ||
                                 strmtyp == eac3::StreamType::kConvertible;
        if (begins_unit && offset != start) {
            units.push_back(stream.subspan(start, offset - start));
            start = offset;
        }
        offset += frame.size();
    }
    if (offset != start) {
        units.push_back(stream.subspan(start, offset - start));
    }
    // A stream whose very first syncframe is a dependent has lost its parent;
    // its channels have nothing to extend.
    if (!units.empty() &&
        (std::to_integer<std::uint32_t>(units.front()[2]) >> 6) ==
            static_cast<std::uint32_t>(eac3::StreamType::kDependent)) {
        return std::unexpected(DecodeError::kInvalidStream);
    }
    return units;
}

std::expected<DecodedFrame, DecodeError> FrameDecoder::decode_frame(
    std::span<const std::byte> frame) {
    // Before the first early return, for the same reason FrameEncoder resets
    // its own: a caller reusing one trace across a file must never read a
    // previous frame's state out of a call that decoded nothing.
    if (config_.trace != nullptr) {
        config_.trace->reset();
    }
    if (frame.size() < 6) {
        return std::unexpected(DecodeError::kTruncated);
    }
    // bsid sits at bit 40 in both generations precisely so it can be read
    // before anything else is interpreted; every field below means something
    // different in an Annex E frame, so check it first rather than letting the
    // AC-3 reading of frmsizecod fail in some incidental way.
    if (*stream_bsid(frame) > 8) {
        return std::unexpected(DecodeError::kUnsupported);
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
    std::optional<std::uint8_t> compr;
    if (r.read(1) != 0) {  // compre (§5.4.2.9)
        compr = static_cast<std::uint8_t>(r.read(8));
    }
    if (r.read(1) != 0) r.skip(8);   // langcode/langcod
    if (r.read(1) != 0) r.skip(7);   // audprodie: mixlevel + roomtyp
    std::optional<int> dialnorm2;
    std::optional<std::uint8_t> compr2;
    if (acmod == Acmod::kDualMono) {
        dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            compr2 = static_cast<std::uint8_t>(r.read(8));
        }
        if (r.read(1) != 0) r.skip(8);  // langcod2e/langcod2
        if (r.read(1) != 0) r.skip(7);  // audprodi2e: mixlevel2 + roomtyp2
    }
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
    out.compr = compr;
    out.dynrng.fill(meta::kDynrngUnity);
    out.dialnorm2 = dialnorm2;
    out.compr2 = compr2;
    out.dynrng2.fill(meta::kDynrngUnity);
    out.blksw.assign(static_cast<std::size_t>(nfchans), {});
    out.channels.assign(static_cast<std::size_t>(nchans),
                        std::vector<float>(kSamplesPerFrame, 0.0f));

    // §7.7.1.2: an absent word inherits from the previous BLOCK, and block 0
    // without one is unity — never the previous frame's value, which is what
    // lets a decoder join a stream mid-programme at the right level.
    std::uint8_t dynrng_word = meta::kDynrngUnity;
    std::uint8_t dynrng2_word = meta::kDynrngUnity;

    // Decode state persisting across blocks. Streams are the fbw channels,
    // then the LFE, then - when coupling is in use - the coupling channel as
    // one more stream, exactly as the encoder lays them out.
    const std::size_t max_streams = static_cast<std::size_t>(nchans) + 1;
    std::vector<int> endmant(max_streams, kLfeEndmant);
    std::vector<std::vector<std::uint8_t>> exps(max_streams);
    BitAllocCodes base_codes{};
    std::vector<int> fgaincod(max_streams, base_codes.fgaincod);
    int csnroffst = 0;
    std::vector<int> fsnroffst(max_streams, 0);
    // §7.2.2.6: per-stream delta bit allocation, reset to "no segments" at the
    // start of every syncframe (the spec's own recommended initialization),
    // then persisting block to block exactly like base_codes/fsnroffst above
    // until re-transmitted or explicitly cleared.
    std::vector<DeltaSegments> delta(max_streams);
    std::array<bool, 4> rematflg{};

    // Coupling state (§7.4). All of it persists until re-transmitted.
    const int cpl_stream = nchans;
    bool cplinu = false;
    bool phsflginu = false;
    int cplbegf = 0;
    int cplstrtmant = 0;
    int ncplsubnd = 0;
    int cplfleak = 0;
    int cplsleak = 0;
    std::vector<bool> chincpl(static_cast<std::size_t>(nfchans), false);
    // Which coupling band each sub-band belongs to (cplbndstrc expansion).
    std::vector<int> subband_band;
    int ncplbnd = 0;
    // [channel][sub-band] - already expanded from bands to sub-bands.
    std::vector<std::vector<double>> cplco(static_cast<std::size_t>(nfchans));
    std::vector<bool> phsflg;

    // The self-check's decoder-side view (ac3/verify/mirror.hpp). nfchans and
    // nchans are frame-wide, so the shape is settled here; the per-block half
    // is filled twice below - the block-boundary bit position on entry, the
    // per-stream state once the allocation for the block exists - so that a
    // frame this decoder ends up REFUSING still leaves behind the offsets it
    // reached, which is what localises a desync to a block.
    if (config_.trace != nullptr) {
        config_.trace->fbw_channels = nfchans;
        config_.trace->coded_channels = nchans;
    }

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        if (config_.trace != nullptr) {
            auto& trace = config_.trace->blocks[static_cast<std::size_t>(block)];
            trace.entered = true;
            trace.bit_offset = r.bit_position();
        }
        std::array<bool, 5> blksw{};  // AC-3's widest acmod (3/2) has 5 fbw channels
        for (int ch = 0; ch < nfchans; ++ch) {
            blksw[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            out.blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(block)] =
                blksw[static_cast<std::size_t>(ch)];
        }
        // §5.4.3.2/§7.3.4: per-channel, read fresh every block (unlike
        // E-AC-3's frame-gated dithflage). Reconstruction is done in
        // read_stream/the decoupling loop below; a coupled channel's own
        // bit still gates dither for its shared high band, applied there
        // after extraction rather than here.
        std::array<bool, 5> dithflag{};
        for (int ch = 0; ch < nfchans; ++ch) {
            dithflag[static_cast<std::size_t>(ch)] = r.read(1) != 0;
        }
        if (r.read(1) != 0) {  // dynrnge
            dynrng_word = static_cast<std::uint8_t>(r.read(8));
        }
        out.dynrng[static_cast<std::size_t>(block)] = dynrng_word;
        if (acmod == Acmod::kDualMono) {
            if (r.read(1) != 0) {  // dynrng2e
                dynrng2_word = static_cast<std::uint8_t>(r.read(8));
            }
            out.dynrng2[static_cast<std::size_t>(block)] = dynrng2_word;
        }

        // --- coupling strategy (§5.3.3) ---
        if (r.read(1) != 0) {  // cplstre: a new strategy, else the prior one stands
            cplinu = r.read(1) != 0;
            if (cplinu) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    chincpl[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                }
                phsflginu = acmod == Acmod::k2_0 && r.read(1) != 0;
                cplbegf = static_cast<int>(r.read(4));
                const int cplendf = static_cast<int>(r.read(4));
                ncplsubnd = coupling::sub_band_count(cplbegf, cplendf);
                if (ncplsubnd < 1) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                cplstrtmant = coupling::start_mant(cplbegf);
                const int cplendmant = coupling::end_mant(cplendf);
                if (cplendmant > 253 || cplstrtmant >= cplendmant) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                endmant[static_cast<std::size_t>(cpl_stream)] = cplendmant;

                // cplbndstrc: a 1 folds this sub-band into the previous
                // coupling band, so coordinates are per band and duplicated
                // back out across the sub-bands they cover.
                subband_band.assign(static_cast<std::size_t>(ncplsubnd), 0);
                ncplbnd = 1;
                for (int bnd = 1; bnd < ncplsubnd; ++bnd) {
                    const bool merged = r.read(1) != 0;
                    if (!merged) {
                        ++ncplbnd;
                    }
                    subband_band[static_cast<std::size_t>(bnd)] = ncplbnd - 1;
                }
                // Coordinates survive a re-sent strategy: cplcoe == 0 in this
                // very block legally means "reuse the previous coordinates"
                // (§5.4.3.14), so clearing them here would silence the
                // coupled high band. Only a change in geometry forces a
                // resize, and then only the new entries start at zero.
                for (auto& channel : cplco) {
                    channel.resize(static_cast<std::size_t>(ncplsubnd), 0.0);
                }
                phsflg.resize(static_cast<std::size_t>(ncplbnd), false);
                // Coupled channels stop carrying their own coefficients here.
                for (int ch = 0; ch < nfchans; ++ch) {
                    if (chincpl[static_cast<std::size_t>(ch)]) {
                        endmant[static_cast<std::size_t>(ch)] = cplstrtmant;
                    }
                }
            }
        }

        // --- coupling coordinates (§7.4.3) ---
        if (cplinu) {
            bool any_new = false;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                if (r.read(1) == 0) {  // cplcoe: reuse the previous values
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
                for (int bnd = 0; bnd < ncplsubnd; ++bnd) {
                    cplco[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bnd)] =
                        band_values[static_cast<std::size_t>(
                            subband_band[static_cast<std::size_t>(bnd)])];
                }
            }
            if (phsflginu && any_new) {
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    phsflg[static_cast<std::size_t>(bnd)] = r.read(1) != 0;
                }
            }
        }

        if (acmod == Acmod::k2_0) {
            if (r.read(1) != 0) {  // rematstr: new flags; else prior flags persist
                // §7.5.2: coupling limits how many rematrix bands exist.
                const int nrematbd = !cplinu ? 4 : (cplbegf > 2 ? 4 : (cplbegf > 0 ? 3 : 2));
                rematflg.fill(false);
                for (int band = 0; band < nrematbd; ++band) {
                    rematflg[static_cast<std::size_t>(band)] = r.read(1) != 0;
                }
            }
        }

        // §5.3.3 exponent strategies: coupling channel first, then fbw, then LFE.
        std::vector<ExpStrategy> strategy(max_streams, ExpStrategy::kReuse);
        if (cplinu) {
            strategy[static_cast<std::size_t>(cpl_stream)] =
                static_cast<ExpStrategy>(r.read(2));
        }
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
        // chbwcod exists only for fbw channels that are NOT coupled.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (strategy[static_cast<std::size_t>(ch)] != ExpStrategy::kReuse &&
                !(cplinu && chincpl[static_cast<std::size_t>(ch)])) {
                const auto chbwcod = r.read(6);
                if (chbwcod > 60) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                endmant[static_cast<std::size_t>(ch)] =
                    ((static_cast<int>(chbwcod) + 12) * 3) + 37;
            }
        }

        // Exponents, in the same order. The coupling channel's set is offset
        // to its own start bin and uses the even-valued absolute reference.
        if (cplinu && strategy[static_cast<std::size_t>(cpl_stream)] != ExpStrategy::kReuse) {
            const auto strat = strategy[static_cast<std::size_t>(cpl_stream)];
            const int end = endmant[static_cast<std::size_t>(cpl_stream)];
            const int span = end - cplstrtmant;
            const int group_size = exponent_group_size(strat);
            if (group_size == 0 || span % (3 * group_size) != 0) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            const int ngrps = span / (3 * group_size);
            const auto cplabsexp = static_cast<std::uint8_t>(r.read(4));
            std::vector<std::uint8_t> groups(static_cast<std::size_t>(ngrps));
            for (auto& g : groups) {
                g = static_cast<std::uint8_t>(r.read(7));
                // §7.10.2 error condition 17: a grouped value above 124 is
                // not a legal triple of mapped values.
                if (g > 124) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            auto& target = exps[static_cast<std::size_t>(cpl_stream)];
            target.assign(static_cast<std::size_t>(end), kMaxExponent);
            decode_coupling_exponents(
                cplabsexp, groups, strat,
                std::span{target}.subspan(static_cast<std::size_t>(cplstrtmant)));
            // §7.2.2.2: exponents are 0..24. A malformed differential chain
            // can walk outside that, and the reconstruction shifts by the
            // exponent - undefined behaviour, not merely wrong audio.
            for (std::size_t bin = static_cast<std::size_t>(cplstrtmant); bin < target.size();
                 ++bin) {
                if (target[bin] > kMaxExponent) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
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
                if (g > 124) {  // §7.10.2 error condition 17
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            exps[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(end), 0);
            decode_exponents(absolute, groups, strat, exps[static_cast<std::size_t>(ch)]);
            // §7.2.2.2: exponents must stay within 0..24; the mantissa
            // reconstruction shifts by them.
            for (const auto exp : exps[static_cast<std::size_t>(ch)]) {
                if (exp > kMaxExponent) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
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
            if (cplinu) {
                fsnroffst[static_cast<std::size_t>(cpl_stream)] = static_cast<int>(r.read(4));
                fgaincod[static_cast<std::size_t>(cpl_stream)] = static_cast<int>(r.read(3));
            }
            for (int ch = 0; ch < nchans; ++ch) {
                fsnroffst[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(4));
                fgaincod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(3));
            }
        } else if (block == 0) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        if (cplinu) {
            if (r.read(1) != 0) {  // cplleake
                cplfleak = static_cast<int>(r.read(3));
                cplsleak = static_cast<int>(r.read(3));
            }
        }
        // Held in a named variable rather than tested inline: the self-check
        // records it, and what §5.4.3.47 makes of a CLEAR one (keep the
        // previous block's state, except in block 0) is exactly the rule an
        // encoder can get wrong invisibly.
        const bool deltbaie = r.read(1) != 0;
        if (deltbaie) {
            // §5.4.3.48-57: a segment set is (deltnseg+1) triples of
            // (deltoffst, deltlen, deltba); bounds are checked here, before
            // compute_bit_allocation ever sees them, since deltoffst/deltlen
            // are attacker-controlled and mask[] is exactly 50 bands wide.
            const auto parse_segments =
                [&r]() -> std::expected<DeltaSegments, DecodeError> {
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
                return segs;
            };
            // §5.4.3.48-49's own syntax table reads every stream's 2-bit
            // cpldeltbae/deltbae[ch] code FIRST, then every stream's segment
            // data - the two are not interleaved per stream, so the codes
            // must all be read (and validated) before any segment parsing.
            // Table 5.16: 00 reuse, 01 new info follows, 10 no delta, 11
            // reserved. cplcode stays at the "reuse" value when coupling is
            // not in use, so the fbw loop below never touches delta[cpl_stream].
            int cplcode = 0;
            if (cplinu) {
                cplcode = static_cast<int>(r.read(2));
                if (cplcode == 3) {
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (block == 0 && cplcode == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            // deltbae[ch]: fbw channels only (§5.4.3.49) - the LFE channel has
            // no delta bit allocation field at all. AC-3's widest acmod (3/2)
            // codes 5 full-bandwidth channels.
            std::array<int, 5> chcodes{};
            for (int ch = 0; ch < nfchans; ++ch) {
                chcodes[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
                if (chcodes[static_cast<std::size_t>(ch)] == 3) {
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (block == 0 && chcodes[static_cast<std::size_t>(ch)] == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            if (cplinu && cplcode == 1) {
                auto segs = parse_segments();
                if (!segs) {
                    return std::unexpected(segs.error());
                }
                delta[static_cast<std::size_t>(cpl_stream)] = *segs;
            } else if (cplinu && cplcode == 2) {
                delta[static_cast<std::size_t>(cpl_stream)] = {};
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const int chcode = chcodes[static_cast<std::size_t>(ch)];
                if (chcode == 1) {
                    auto segs = parse_segments();
                    if (!segs) {
                        return std::unexpected(segs.error());
                    }
                    delta[static_cast<std::size_t>(ch)] = *segs;
                } else if (chcode == 2) {
                    delta[static_cast<std::size_t>(ch)] = {};
                }
            }
        } else if (block == 0) {
            // §5.4.3.47: deltbaie == 0 in block 0 forces "no delta alloc" for
            // the coupling channel (if any) and every fbw channel.
            if (cplinu) {
                delta[static_cast<std::size_t>(cpl_stream)] = {};
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                delta[static_cast<std::size_t>(ch)] = {};
            }
        }
        if (r.read(1) != 0) {  // skiple
            const auto skipl = r.read(9);
            r.skip(skipl * 8);
        }

        // Bit allocation (recomputed every block: parameters are stored
        // state, so the result matches the decoder-update rule of §7.2.1).
        const int streams = nchans + (cplinu ? 1 : 0);
        // §7.2.2.1.1 is frame-wide: csnroffst plus EVERY channel's fine
        // offset, including the coupling channel's and the LFE's.
        bool snr_all_zero = csnroffst == 0;
        for (int s = 0; s < streams && snr_all_zero; ++s) {
            snr_all_zero = fsnroffst[static_cast<std::size_t>(s)] == 0;
        }
        std::vector<std::vector<std::uint8_t>> bap(max_streams);
        for (int s = 0; s < streams; ++s) {
            const bool is_cpl = s == cpl_stream;
            const int end = endmant[static_cast<std::size_t>(s)];
            if (static_cast<int>(exps[static_cast<std::size_t>(s)].size()) != end) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            BitAllocCodes codes = base_codes;
            codes.fgaincod = fgaincod[static_cast<std::size_t>(s)];
            const BitAllocRegion region{.start = is_cpl ? cplstrtmant : 0,
                                        .coupling = is_cpl,
                                        .cplfleak = cplfleak,
                                        .cplsleak = cplsleak,
                                        .snr_all_zero = snr_all_zero,
                                        .delta = delta[static_cast<std::size_t>(s)]};
            bap[static_cast<std::size_t>(s)].assign(static_cast<std::size_t>(end), 0);
            compute_bit_allocation(exps[static_cast<std::size_t>(s)], sample_rate, codes,
                                   csnroffst, fsnroffst[static_cast<std::size_t>(s)],
                                   bap[static_cast<std::size_t>(s)], region);
        }

        // The other half of the self-check's per-block record: everything the
        // encoder's own model claims about this block, as this side derived it
        // from the wire. Placed after the allocation rather than after the
        // audio, so a block whose mantissas turn out to be unreadable still
        // reports the state that decided how wide they were.
        if (config_.trace != nullptr) {
            auto& trace = config_.trace->blocks[static_cast<std::size_t>(block)];
            trace.deltbaie = deltbaie;
            trace.streams.resize(static_cast<std::size_t>(streams));
            for (int s = 0; s < streams; ++s) {
                auto& stream = trace.streams[static_cast<std::size_t>(s)];
                stream.exponents = exps[static_cast<std::size_t>(s)];
                stream.bap = bap[static_cast<std::size_t>(s)];
                stream.delta = delta[static_cast<std::size_t>(s)];
            }
            trace.allocated = true;
        }

        // Mantissas -> coefficients. §5.3.3 orders them by fbw channel, with
        // the coupling channel inserted after the FIRST coupled one, then the
        // LFE. Everything is unpacked before any reconstruction, because
        // decoupling and the rematrix undo both need whole channels.
        MantissaBlockReader mantissa_reader;
        std::vector<std::array<double, 256>> coeffs(max_streams);
        // §7.3.4: dither is substituted at a bap-0 bin only for a stream that
        // has its OWN dithflag - a full-bandwidth channel's own spectrum
        // (s < nfchans). The LFE has no dithflag bit at all (§5.4.3.2's loop
        // is bounded by nfchans) and always reconstructs as zero; the shared
        // coupling stream (s == cpl_stream) is deliberately left silent HERE
        // too, because §7.3.4 requires dither to be "applied after the
        // individual channels are extracted from the coupling channel" so
        // that "the dither applied to each channel's upper frequencies is
        // uncorrelated" - see the decoupling loop below for that half.
        const auto read_stream = [&](int s) {
            const int begin = s == cpl_stream ? cplstrtmant : 0;
            const int end = endmant[static_cast<std::size_t>(s)];
            const bool dither_eligible =
                s < nfchans && dithflag[static_cast<std::size_t>(s)];
            for (int bin = begin; bin < end; ++bin) {
                const int bap_value =
                    bap[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)];
                const int exp =
                    exps[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    coeffs[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)] =
                        dither_eligible ? dither_.next() / static_cast<double>(1u << exp) : 0.0;
                    continue;
                }
                const auto code = mantissa_reader.read(r, bap_value);
                coeffs[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa(code, bap_value) / static_cast<double>(1u << exp);
            }
        };
        bool read_coupling = false;
        for (int ch = 0; ch < nfchans; ++ch) {
            read_stream(ch);
            if (cplinu && chincpl[static_cast<std::size_t>(ch)] && !read_coupling) {
                read_stream(cpl_stream);
                read_coupling = true;
            }
        }
        if (lfe) {
            read_stream(nfchans);
        }

        // §7.4.3 decoupling: each coupled channel's high band is the shared
        // channel scaled by that channel's coordinate, times 8 - undoing the
        // encoder's /8 headroom scaling.
        if (cplinu) {
            const auto& shared = coeffs[static_cast<std::size_t>(cpl_stream)];
            const auto& cpl_bap = bap[static_cast<std::size_t>(cpl_stream)];
            const auto& cpl_exps = exps[static_cast<std::size_t>(cpl_stream)];
            const int cplendmant = endmant[static_cast<std::size_t>(cpl_stream)];
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& target = coeffs[static_cast<std::size_t>(ch)];
                const bool ch_dither = dithflag[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < ncplsubnd; ++bnd) {
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
                        // §7.3.4: a zero-bap shared bin is dither-substituted
                        // per RECEIVING channel, independently - reusing one
                        // dithered coupling-domain sample for every coupled
                        // channel would make their noise correlated (just
                        // scaled differently), which is exactly what "applied
                        // after the individual channels are extracted ...
                        // uncorrelated" rules out. Each channel draws its own
                        // sample and runs it through the same extraction
                        // formula a real coupling coefficient would use.
                        const double coeff =
                            (cpl_bap[ubin] == 0 && ch_dither)
                                ? dither_.next() / static_cast<double>(1u << cpl_exps[ubin])
                                : shared[ubin];
                        target[ubin] = coeff * coordinate * 8.0 * sign;
                    }
                }
            }
        }
        if (acmod == Acmod::k2_0) {
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, applied up
            // to the lower bandwidth of the two channels.
            const int cap = std::min(endmant[0], endmant[1]) - 1;
            for (std::size_t band = 0; band < kRematrixBands.size(); ++band) {
                if (!rematflg[band]) {
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
        // §7.7 gain, applied to the COEFFICIENTS rather than to the output
        // samples: the overlap-add window then cross-fades one block's gain
        // into the next, which is what keeps a per-block gain change from
        // clicking. Scaling the 256 output samples instead would step.
        // Applied to every coded channel including the LFE: §7.7.1 describes a
        // gain change to the audio block, not to a subset of its channels. The
        // coupling channel is skipped because decoupling has already spread it
        // into the channels above. Dual mono's two channels are independent
        // programmes, so Ch2 gets its own gain from its own words rather than
        // sharing Ch1's - applying one programme's compression to the other
        // would be exactly the cross-talk 1+1 exists to avoid.
        for (int ch = 0; ch < nchans; ++ch) {
            const bool second_programme = acmod == Acmod::kDualMono && ch == 1;
            const double drc = second_programme
                                    ? internal::block_gain(config_, dynrng2_word, compr2)
                                    : internal::block_gain(config_, dynrng_word, compr);
            if (drc != 1.0) {
                for (auto& value : coeffs[static_cast<std::size_t>(ch)]) {
                    value *= drc;
                }
            }
        }
        for (int ch = 0; ch < nchans; ++ch) {
            std::array<double, 512> x{};
            if (ch < nfchans && blksw[static_cast<std::size_t>(ch)]) {
                imdct256_pair_windowed(coeffs[static_cast<std::size_t>(ch)], x);
            } else {
                imdct512_windowed(coeffs[static_cast<std::size_t>(ch)], x);
            }
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
