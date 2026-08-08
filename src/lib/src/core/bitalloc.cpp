#include "ac3/core/bitalloc.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>

#include "ac3/core/bitalloc_tables.hpp"

namespace ac3 {

namespace {

using namespace tables;

// §7.2.2.3: log-addition of two banded PSD values.
int logadd(int a, int b) {
    const int c = a - b;
    const int address = std::min(std::abs(c) >> 1, 255);
    return (c >= 0 ? a : b) + kLogAdd[static_cast<std::size_t>(address)];
}

// §7.2.2.4 calc_lowcomp. The spec pseudocode carries a known erratum (a
// stray semicolon after `if ((b0 + 256) == b1)` in the bin < 7 branch); the
// universally implemented intent mirrors the bin < 20 branch's structure.
int calc_lowcomp(int a, int b0, int b1, int bin) {
    if (bin < 7) {
        if (b0 + 256 == b1) {
            a = 384;
        } else if (b0 > b1) {
            a = std::max(0, a - 64);
        }
    } else if (bin < 20) {
        if (b0 + 256 == b1) {
            a = 320;
        } else if (b0 > b1) {
            a = std::max(0, a - 64);
        }
    } else {
        a = std::max(0, a - 128);
    }
    return a;
}

}  // namespace

void compute_bit_allocation(std::span<const std::uint8_t> exps, SampleRate sample_rate,
                            const BitAllocCodes& codes, int csnroffst, int fsnroffst,
                            std::span<std::uint8_t> bap, const BitAllocRegion& region) {
    assert(exps.size() == bap.size());
    const int end = static_cast<int>(exps.size());
    assert(end >= 1 && end <= 253);
    assert(region.start >= 0 && region.start < end);

    // §7.2.2.1.1 special case: all-zero SNR offsets -> all-zero bap.
    if (csnroffst == 0 && fsnroffst == 0) {
        std::ranges::fill(bap, std::uint8_t{0});
        return;
    }

    const int sdecay = kSlowDec[static_cast<std::size_t>(codes.sdcycod)];
    const int fdecay = kFastDec[static_cast<std::size_t>(codes.fdcycod)];
    const int sgain = kSlowGain[static_cast<std::size_t>(codes.sgaincod)];
    const int dbknee = kDbPerBit[static_cast<std::size_t>(codes.dbpbcod)];
    int floor = kFloor[static_cast<std::size_t>(codes.floorcod)];
    if (floor >= 0x8000) {
        floor -= 0x10000;  // 0xf800 is a negative 16-bit value (-2048)
    }
    const int fgain = kFastGain[static_cast<std::size_t>(codes.fgaincod)];
    const int snroffset = snr_offset(csnroffst, fsnroffst);
    const int kStart = region.start;

    // §7.2.2.2: exponents -> 13-bit signed log PSD.
    std::array<int, 253> psd{};
    for (int bin = kStart; bin < end; ++bin) {
        psd[static_cast<std::size_t>(bin)] = 3072 - (exps[static_cast<std::size_t>(bin)] << 7);
    }

    // §7.2.2.3: banded integration via log-addition.
    std::array<int, 50> bndpsd{};
    {
        int j = kStart;
        int k = kMaskTab[kStart];
        int lastbin = 0;
        do {
            lastbin = std::min(kBandStart[static_cast<std::size_t>(k)] +
                                   kBandSize[static_cast<std::size_t>(k)],
                               end);
            bndpsd[static_cast<std::size_t>(k)] = psd[static_cast<std::size_t>(j)];
            ++j;
            for (int i = j; i < lastbin; ++i) {
                bndpsd[static_cast<std::size_t>(k)] =
                    logadd(bndpsd[static_cast<std::size_t>(k)], psd[static_cast<std::size_t>(j)]);
                ++j;
            }
            ++k;
        } while (end > lastbin);
    }

    // §7.2.2.4: excitation function. Two shapes: fbw/LFE channels start at
    // band 0 and run the lowcomp low-frequency compensation, the coupling
    // channel starts higher and seeds its leaks instead.
    const int bndstrt = kMaskTab[kStart];
    const int bndend = kMaskTab[static_cast<std::size_t>(end - 1)] + 1;
    std::array<int, 50> excite{};
    int lowcomp = 0;
    int fastleak = 0;
    int slowleak = 0;
    int begin_band = bndstrt;
    if (region.coupling) {
        // §7.2.2.1 / §7.2.2.4: the coupling channel starts above the
        // low-frequency region entirely, so it skips the lowcomp machinery
        // and instead seeds the leak state from the transmitted cplfleak /
        // cplsleak, continuing the decay from wherever the fbw channels left
        // off below the coupling frequency.
        fastleak = (region.cplfleak << 8) + 768;
        slowleak = (region.cplsleak << 8) + 768;
    } else {
        assert(bndstrt == 0);
        // §7.2.2.4: for the LFE channel (bndend == 7), calc_lowcomp and the
        // monotone-rise break check are skipped for the last band (bin 6) —
        // bndpsd[7] does not exist there.
        const auto not_lfe_last = [bndend](int bin) { return bndend != 7 || bin != 6; };
        lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0);
        excite[0] = bndpsd[0] - fgain - lowcomp;
        lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1);
        excite[1] = bndpsd[1] - fgain - lowcomp;
        int begin = 7;
        for (int bin = 2; bin < 7; ++bin) {
            if (not_lfe_last(bin)) {
                lowcomp = calc_lowcomp(lowcomp, bndpsd[static_cast<std::size_t>(bin)],
                                       bndpsd[static_cast<std::size_t>(bin) + 1], bin);
            }
            fastleak = bndpsd[static_cast<std::size_t>(bin)] - fgain;
            slowleak = bndpsd[static_cast<std::size_t>(bin)] - sgain;
            excite[static_cast<std::size_t>(bin)] = fastleak - lowcomp;
            if (not_lfe_last(bin) &&
                bndpsd[static_cast<std::size_t>(bin)] <= bndpsd[static_cast<std::size_t>(bin) + 1]) {
                begin = bin + 1;
                break;
            }
        }
        for (int bin = begin; bin < std::min(bndend, 22); ++bin) {
            if (not_lfe_last(bin)) {
                lowcomp = calc_lowcomp(lowcomp, bndpsd[static_cast<std::size_t>(bin)],
                                       bndpsd[static_cast<std::size_t>(bin) + 1], bin);
            }
            fastleak -= fdecay;
            fastleak = std::max(fastleak, bndpsd[static_cast<std::size_t>(bin)] - fgain);
            slowleak -= sdecay;
            slowleak = std::max(slowleak, bndpsd[static_cast<std::size_t>(bin)] - sgain);
            excite[static_cast<std::size_t>(bin)] = std::max(fastleak - lowcomp, slowleak);
        }
        begin_band = 22;
    }

    // The common upper region: no lowcomp, plain dual-leak decay. For fbw
    // channels this picks up at band 22; for the coupling channel it is the
    // whole range.
    for (int bin = begin_band; bin < bndend; ++bin) {
        fastleak -= fdecay;
        fastleak = std::max(fastleak, bndpsd[static_cast<std::size_t>(bin)] - fgain);
        slowleak -= sdecay;
        slowleak = std::max(slowleak, bndpsd[static_cast<std::size_t>(bin)] - sgain);
        excite[static_cast<std::size_t>(bin)] = std::max(fastleak, slowleak);
    }

    // §7.2.2.5: masking curve (excitation, dB knee boost, hearing threshold).
    std::array<int, 50> mask{};
    const auto& hth = *kHearingThreshold[static_cast<std::size_t>(sample_rate)];
    for (int bin = bndstrt; bin < bndend; ++bin) {
        if (bndpsd[static_cast<std::size_t>(bin)] < dbknee) {
            excite[static_cast<std::size_t>(bin)] +=
                (dbknee - bndpsd[static_cast<std::size_t>(bin)]) >> 2;
        }
        mask[static_cast<std::size_t>(bin)] =
            std::max(excite[static_cast<std::size_t>(bin)], hth[static_cast<std::size_t>(bin)]);
    }

    // §7.2.2.6: delta bit allocation not in use (deltbaie = 0).

    // §7.2.2.7: bap computation. The snroffset/floor/truncation order is
    // normative: subtract snroffset, subtract floor, clamp at zero, truncate
    // with & 0x1fe0, re-add floor.
    {
        int i = kStart;
        int j = kMaskTab[kStart];
        int lastbin = 0;
        do {
            lastbin = std::min(kBandStart[static_cast<std::size_t>(j)] +
                                   kBandSize[static_cast<std::size_t>(j)],
                               end);
            int m = mask[static_cast<std::size_t>(j)];
            m -= snroffset;
            m -= floor;
            if (m < 0) {
                m = 0;
            }
            m &= 0x1fe0;
            m += floor;
            for (int k = i; k < lastbin; ++k) {
                int address = (psd[static_cast<std::size_t>(i)] - m) >> 5;
                address = std::min(63, std::max(0, address));
                bap[static_cast<std::size_t>(i)] = kBapTab[static_cast<std::size_t>(address)];
                ++i;
            }
            ++j;
        } while (end > lastbin);
    }
}

}  // namespace ac3
