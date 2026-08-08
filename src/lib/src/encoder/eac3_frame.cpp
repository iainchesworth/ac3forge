#include "ac3/encoder/eac3_frame.hpp"

#include <algorithm>
#include <cassert>

#include "ac3/core/bitwriter.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"

namespace ac3::eac3 {

namespace {

// Frame-level strategy flags for this minimal profile. Every advanced tool
// is switched off; what remains is the AC-3 coding path inside an E-AC-3
// container.
constexpr int kExpstre = 1;        // per-block exponent strategies, listed in audfrm
constexpr int kAhte = 0;           // no adaptive hybrid transform
constexpr int kSnroffststr = 1;    // one coarse + one shared fine offset
constexpr int kTransproce = 0;     // no transient pre-noise processing
constexpr int kBlkswe = 0;         // long blocks only, so no per-block flags
constexpr int kDithflage = 1;      // sent explicitly: the DEFAULT when absent is
                                   // dither ON, which would fill every zero-bit
                                   // bin with noise and make "silence" audible
constexpr int kBamode = 0;         // default allocation parameters, zero bits
constexpr int kFrmfgaincode = 0;   // fgaincod defaults to 0x4, matching AC-3
constexpr int kDbaflde = 0;        // no delta bit allocation
// AC-3 has to push its padding through in-block skip fields, because §5.5
// confines the aux field to the final 3/8 of the frame - a rule that exists
// to protect the crc1-at-5/8 checkpoint. E-AC-3 has no crc1 and Annex E
// states no equivalent constraint, so auxbits can absorb the whole
// remainder. That is both simpler and, measurably, what decoders expect:
// padding routed through a block-0 skip field came back as audible data.
constexpr int kSkipflde = 0;
constexpr int kSpxattene = 0;      // no spectral extension attenuation

constexpr int kTailBits = 18;  // auxdatae + crcrsv + crc2

}  // namespace

std::expected<std::vector<std::byte>, FrameError> build_silent_frame(
    const FrameConfig& config) {
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

    const int nfchans = fullbw_channel_count(config.acmod);
    const int endmant = ((config.chbwcod + 12) * 3) + 37;
    const std::uint32_t words = frame_words(config.sample_rate, config.bitrate_kbps);
    const std::uint32_t total_bytes = words * 2;
    const std::uint32_t total_bits = total_bytes * 8;

    // Exponents: the same all-quiet ramp the AC-3 silent frame uses, so the
    // decoder's own allocation returns zero everywhere.
    std::vector<std::uint8_t> quiet(static_cast<std::size_t>(endmant), kMaxExponent);
    const auto fbw_exps = encode_exponents(quiet, ExpStrategy::kD15);
    std::vector<std::uint8_t> lfe_quiet(7, kMaxExponent);
    const auto lfe_exps = encode_exponents(lfe_quiet, ExpStrategy::kD15);

    // Everything except the padding and the tail. Written twice: once to
    // measure, once for real.
    const auto emit = [&](BitWriter& w) {
        w.put(kSyncWord, 16);

        // --- bsi (Table E1.2) ---
        w.put(0, 2);  // strmtyp: independent
        w.put(0, 3);  // substreamid
        w.put(words - 1, 11);  // frmsiz is words - 1
        w.put(static_cast<std::uint32_t>(config.sample_rate), 2);  // fscod (not 0x3)
        w.put(3, 2);  // numblkscod: six blocks per syncframe
        w.put(static_cast<std::uint32_t>(config.acmod), 3);
        w.put(config.lfe ? 1 : 0, 1);
        w.put(kBsid, 5);
        w.put(static_cast<std::uint32_t>(config.dialnorm), 5);
        w.put(0, 1);  // compre
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
            w.put(0, 1);  // cplinu[0]: coupling off (cplstre[0] is implied 1)
            for (int blk = 1; blk < kBlocksPerFrame; ++blk) {
                w.put(0, 1);  // cplstre[blk] = 0, so cplinu inherits 0
            }
        }
        // expstre == 1: every block's strategy is listed here, not in the block.
        for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
            for (int ch = 0; ch < nfchans; ++ch) {
                w.put(static_cast<std::uint32_t>(blk == 0 ? ExpStrategy::kD15
                                                          : ExpStrategy::kReuse),
                      2);
            }
        }
        if (config.lfe) {
            for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
                w.put(blk == 0 ? 1 : 0, 1);  // lfeexpstr
            }
        }
        // strmtyp == 0 and numblkscod == 0x3, so convexpstre is implied 1 and
        // the converter strategies always follow. They describe how an AC-3
        // converter would code this frame; mirroring the real strategy is the
        // honest value.
        for (int ch = 0; ch < nfchans; ++ch) {
            w.put(0, 5);  // convexpstr[ch]
        }
        // ahte == 0, snroffststr != 0, transproce == 0, spxattene == 0 all
        // contribute nothing - but audfrm still ends with the block-start
        // info flag whenever numblkscod != 0. Omitting this one bit shifts
        // every audio block by a bit, which a decoder reads as spectral
        // extension being switched on.
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
                    w.put(fbw_exps.absolute, 4);
                    for (const auto group : fbw_exps.groups) {
                        w.put(group, 7);
                    }
                    w.put(0, 2);  // gainrng
                }
                if (config.lfe) {
                    w.put(lfe_exps.absolute, 4);
                    assert(lfe_exps.groups.size() == 2);
                    for (const auto group : lfe_exps.groups) {
                        w.put(group, 7);
                    }
                }
            }

            // bamode == 0: the allocation parameters take their defaults.
            // snroffststr == 1: block 0 always carries the offsets.
            if (first) {
                w.put(0, 6);  // csnroffst
                w.put(0, 4);  // blkfsnroffst - zero everywhere, so every bap is 0
            } else {
                w.put(0, 1);  // snroffste: reuse
            }
            // frmfgaincode == 0, so fgaincod defaults to 0x4 for every channel.
            w.put(0, 1);  // convsnroffste (strmtyp == 0)
            // cplinu == 0: no coupling leak. dbaflde == 0: no delta allocation.

            // skipflde == 0: no skip field in any block.
            // Every bap is zero, so no mantissa data follows.
        }
    };

    // Measure the syntax, then let auxbits absorb the whole remainder.
    BitWriter probe;
    emit(probe);
    const auto content_bits = static_cast<std::uint32_t>(probe.bit_count());
    if (content_bits + kTailBits > total_bits) {
        return std::unexpected(FrameError::kInvalidBitrate);
    }
    const std::uint32_t spare = total_bits - content_bits - kTailBits;

    BitWriter w;
    emit(w);
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

}  // namespace ac3::eac3
