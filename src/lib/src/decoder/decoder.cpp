#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/meta/drc.hpp"

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
            return "valid AC-3 this decoder does not implement (bsid > 8, dual mono, or delta "
                   "bit allocation)";
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

// The §7.7 gain for one block, resolving which of the two control signals
// applies. §7.7.2.1: a decoder told to use compr falls back on dynrng for any
// syncframe with no compr word, so heavy compression is a preference and not a
// mode switch.
double block_gain(const DecoderConfig& config, std::uint8_t dynrng_word,
                  std::optional<std::uint8_t> compr) {
    if (config.heavy_compression && compr) {
        // §7.7.2 states no partial-compression scaling: compr's whole purpose
        // is a hard ceiling, and a decoder that applied a fraction of it would
        // be promising a ceiling it does not deliver.
        return meta::compr_gain(*compr);
    }
    if (config.drc_scale == 0.0 || dynrng_word == meta::kDynrngUnity) {
        return 1.0;
    }
    const double gain = meta::dynrng_gain(dynrng_word);
    // §7.7.1's "Partial Compression" scales the word as a signed fraction of
    // dB, which in the linear domain is exactly raising the gain to that
    // power. Doing it here rather than on the bits avoids re-quantising.
    return config.drc_scale == 1.0 ? gain : std::pow(gain, config.drc_scale);
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
    std::optional<std::uint8_t> compr;
    if (r.read(1) != 0) {  // compre (§5.4.2.9)
        compr = static_cast<std::uint8_t>(r.read(8));
    }
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
    out.compr = compr;
    out.dynrng.fill(meta::kDynrngUnity);
    out.blksw.assign(static_cast<std::size_t>(nfchans), {});
    out.channels.assign(static_cast<std::size_t>(nchans),
                        std::vector<float>(kSamplesPerFrame, 0.0f));

    // §7.7.1.2: an absent word inherits from the previous BLOCK, and block 0
    // without one is unity — never the previous frame's value, which is what
    // lets a decoder join a stream mid-programme at the right level.
    std::uint8_t dynrng_word = meta::kDynrngUnity;

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

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        std::array<bool, 5> blksw{};  // AC-3's widest acmod (3/2) has 5 fbw channels
        for (int ch = 0; ch < nfchans; ++ch) {
            blksw[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            out.blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(block)] =
                blksw[static_cast<std::size_t>(ch)];
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            (void)r.read(1);  // dithflag: bap-0 bins reconstruct as zero either way
        }
        if (r.read(1) != 0) {  // dynrnge
            dynrng_word = static_cast<std::uint8_t>(r.read(8));
        }
        out.dynrng[static_cast<std::size_t>(block)] = dynrng_word;

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
        if (r.read(1) != 0) {  // deltbaie
            return std::unexpected(DecodeError::kUnsupported);
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
                                        .snr_all_zero = snr_all_zero};
            bap[static_cast<std::size_t>(s)].assign(static_cast<std::size_t>(end), 0);
            compute_bit_allocation(exps[static_cast<std::size_t>(s)], sample_rate, codes,
                                   csnroffst, fsnroffst[static_cast<std::size_t>(s)],
                                   bap[static_cast<std::size_t>(s)], region);
        }

        // Mantissas -> coefficients. §5.3.3 orders them by fbw channel, with
        // the coupling channel inserted after the FIRST coupled one, then the
        // LFE. Everything is unpacked before any reconstruction, because
        // decoupling and the rematrix undo both need whole channels.
        MantissaBlockReader mantissa_reader;
        std::vector<std::array<double, 256>> coeffs(max_streams);
        const auto read_stream = [&](int s) {
            const int begin = s == cpl_stream ? cplstrtmant : 0;
            const int end = endmant[static_cast<std::size_t>(s)];
            for (int bin = begin; bin < end; ++bin) {
                const int bap_value =
                    bap[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    continue;  // silence (dither substitution not implemented)
                }
                const auto code = mantissa_reader.read(r, bap_value);
                const int exp =
                    exps[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)];
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
            const int cplendmant = endmant[static_cast<std::size_t>(cpl_stream)];
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& target = coeffs[static_cast<std::size_t>(ch)];
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
                        target[static_cast<std::size_t>(bin)] =
                            shared[static_cast<std::size_t>(bin)] * coordinate * 8.0 * sign;
                    }
                }
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
        // §7.7 gain, applied to the COEFFICIENTS rather than to the output
        // samples: the overlap-add window then cross-fades one block's gain
        // into the next, which is what keeps a per-block gain change from
        // clicking. Scaling the 256 output samples instead would step.
        // Applied to every coded channel including the LFE: §7.7.1 describes a
        // gain change to the audio block, not to a subset of its channels. The
        // coupling channel is skipped because decoupling has already spread it
        // into the channels above.
        const double drc = block_gain(config_, dynrng_word, compr);
        if (drc != 1.0) {
            for (int ch = 0; ch < nchans; ++ch) {
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
