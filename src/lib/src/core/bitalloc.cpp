#include "ac3/core/bitalloc.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

#include "ac3/core/aht_tables.hpp"
#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/internal/profiling.hpp"

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

// §7.2.2.3: bands psd[] (indexed from bin `start` through `end`) into a
// 50-wide per-band array via log-addition. Factored out so the encoder-only
// real-coefficient curve (choose_delta_segments, below) is computed with
// arithmetic identical to the exponent-derived one compute_bit_allocation
// uses, which is what keeps the two directly comparable in the same units.
std::array<int, 50> band_psd(std::span<const int> psd, int start, int end) {
    std::array<int, 50> bndpsd{};
    int j = start;
    int k = kMaskTab[static_cast<std::size_t>(start)];
    int lastbin = 0;
    do {
        lastbin = std::min(
            kBandStart[static_cast<std::size_t>(k)] + kBandSize[static_cast<std::size_t>(k)], end);
        bndpsd[static_cast<std::size_t>(k)] = psd[static_cast<std::size_t>(j)];
        ++j;
        for (int i = j; i < lastbin; ++i) {
            bndpsd[static_cast<std::size_t>(k)] =
                logadd(bndpsd[static_cast<std::size_t>(k)], psd[static_cast<std::size_t>(j)]);
            ++j;
        }
        ++k;
    } while (end > lastbin);
    return bndpsd;
}

// A run of consecutive absolute bands sharing one Table 5.17 deltba code.
struct DeltaRun {
    int band = 0;
    int length = 0;
    int code = 0;
};

// The inverse of §7.2.2.6's `delta = (code>=4 ? code-3 : code-4) << 7`: the
// nearest code correcting `diff` units of mask[], or nullopt when |diff| is
// under half a step - not worth spending a segment on.
std::optional<int> delta_code_for(int diff) {
    // Require a full Table 5.17 step (128 units, 6 dB) before spending a
    // segment, rather than rounding to the nearest one: a "nearest step"
    // threshold fires on roughly half of all bins even for ordinary
    // quantization residue, which measurably narrowed coupling's usual cost
    // advantage over independent per-channel coding without a matching
    // quality win to show for it.
    if (diff >= 128) {
        return std::clamp(diff / 128 + 3, 4, 7);
    }
    if (diff <= -128) {
        return std::clamp(4 - (-diff) / 128, 0, 3);
    }
    return std::nullopt;
}

}  // namespace

int fast_gain(int fgaincod) {
    return kFastGain[static_cast<std::size_t>(std::clamp(fgaincod, 0, 7))];
}

int slow_gain(int sgaincod) {
    return kSlowGain[static_cast<std::size_t>(std::clamp(sgaincod, 0, 3))];
}

void compute_bit_allocation(std::span<const std::uint8_t> exps, SampleRate sample_rate,
                            const BitAllocCodes& codes, int csnroffst, int fsnroffst,
                            std::span<std::uint8_t> bap, const BitAllocRegion& region) {
    AC3_ZONE_SCOPED_N("compute_bit_allocation");
    assert(exps.size() == bap.size());
    const int end = static_cast<int>(exps.size());
    assert(end >= 1 && end <= 253);
    assert(region.start >= 0 && region.start < end);

    // §7.2.2.1.1 special case: when EVERY SNR offset in the block is zero,
    // the whole bap array is zero and no allocation runs. The condition spans
    // all channels, so the caller supplies it.
    if (region.snr_all_zero) {
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
    const std::array<int, 50> bndpsd = band_psd(psd, kStart, end);

    // §7.2.2.4: excitation function. Two shapes: fbw/LFE channels start at
    // band 0 and run the lowcomp low-frequency compensation, the coupling
    // channel starts higher and seeds its leaks instead.
    const int bndstrt = kMaskTab[static_cast<std::size_t>(kStart)];
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
    const auto& hth = *kHearingThreshold[static_cast<std::size_t>(fscod_family(sample_rate))];
    for (int bin = bndstrt; bin < bndend; ++bin) {
        if (bndpsd[static_cast<std::size_t>(bin)] < dbknee) {
            excite[static_cast<std::size_t>(bin)] +=
                (dbknee - bndpsd[static_cast<std::size_t>(bin)]) >> 2;
        }
        mask[static_cast<std::size_t>(bin)] =
            std::max(excite[static_cast<std::size_t>(bin)], hth[static_cast<std::size_t>(bin)]);
    }

    // §7.2.2.6: delta bit allocation. mask[]/psd[] units are 128 per exponent
    // step, which is exactly one Table 5.17 6 dB step, so `delta` below is
    // added directly with no unit conversion. `region.delta.deltnseg == 0`
    // (the default) makes this a no-op, matching the spec's own recommended
    // reset state.
    {
        int band = 0;
        for (int seg = 0; seg < region.delta.deltnseg; ++seg) {
            band += region.delta.deltoffst[static_cast<std::size_t>(seg)];
            const int code = region.delta.deltba[static_cast<std::size_t>(seg)];
            const int delta = (code >= 4 ? code - 3 : code - 4) << 7;
            const int len = region.delta.deltlen[static_cast<std::size_t>(seg)];
            assert(band >= 0 && band + len <= 50);
            for (int k = 0; k < len; ++k) {
                mask[static_cast<std::size_t>(band)] += delta;
                ++band;
            }
        }
    }

    // §7.2.2.7: bap computation. The snroffset/floor/truncation order is
    // normative: subtract snroffset, subtract floor, clamp at zero, truncate
    // with & 0x1fe0, re-add floor.
    {
        int i = kStart;
        int j = kMaskTab[static_cast<std::size_t>(kStart)];
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
                bap[static_cast<std::size_t>(i)] =
                    region.high_efficiency
                        ? kHeBapTab[static_cast<std::size_t>(address)]
                        : kBapTab[static_cast<std::size_t>(address)];
                ++i;
            }
            ++j;
        } while (end > lastbin);
    }
}

DeltaSegments choose_delta_segments(std::span<const double> coefficients,
                                    std::span<const std::uint8_t> exps, int start) {
    assert(coefficients.size() == exps.size());
    const int end = static_cast<int>(exps.size());
    assert(end >= 1 && end <= 253);
    assert(start >= 0 && start < end);

    // Two psd curves in identical units: the flat one compute_bit_allocation
    // itself would build from exps alone, and one built from the real
    // pre-quantization coefficient magnitude. Table 5.17's 128-units-per-6dB
    // step is exactly one exponent step (§7.2.2.2's psd = 3072 - exp<<7), so
    // exponent = -1 - log2(|c|) gives real_psd = 3200 + 128*log2(|c|).
    std::array<int, 253> psd{};
    std::array<int, 253> real_psd{};
    for (int bin = start; bin < end; ++bin) {
        const auto i = static_cast<std::size_t>(bin);
        psd[i] = 3072 - (exps[i] << 7);
        const double magnitude = std::abs(static_cast<double>(coefficients[i]));
        real_psd[i] = magnitude > 0.0
                          ? static_cast<int>(std::lround(3200.0 + 128.0 * std::log2(magnitude)))
                          : psd[i];  // silence: nothing to correct
    }

    const std::array<int, 50> bndpsd = band_psd(psd, start, end);
    const std::array<int, 50> real_bndpsd = band_psd(real_psd, start, end);
    const int bndstrt = kMaskTab[static_cast<std::size_t>(start)];
    const int bndend = kMaskTab[static_cast<std::size_t>(end - 1)] + 1;

    // Merge bands whose correction rounds to the same code into runs, so
    // adjacent agreement costs one segment instead of one per band.
    std::vector<DeltaRun> runs;
    for (int band = bndstrt; band < bndend; ++band) {
        const auto b = static_cast<std::size_t>(band);
        const auto code = delta_code_for(real_bndpsd[b] - bndpsd[b]);
        if (!code) {
            continue;
        }
        if (!runs.empty() && runs.back().band + runs.back().length == band &&
            runs.back().code == *code) {
            ++runs.back().length;
        } else {
            runs.push_back({.band = band, .length = 1, .code = *code});
        }
    }
    if (runs.empty()) {
        return {};
    }
    // §5.4.3.54/E2.3.2.9: at most 8 segments (3-bit deltnseg field + 1). Keep
    // the largest-magnitude corrections when more runs qualify.
    if (runs.size() > 8) {
        std::ranges::nth_element(runs, runs.begin() + 8, std::ranges::greater{},
                                 [](const DeltaRun& r) { return std::abs(2 * r.code - 7); });
        runs.resize(8);
        std::ranges::sort(runs, {}, &DeltaRun::band);
    }

    // Encode each run as (offset, length, code). deltoffst is 5 bits (max
    // 31): a wider gap is bridged with inert (deltlen == 0) filler segments
    // that only advance the cursor. deltlen is 4 bits (max 15): a longer run
    // splits into consecutive zero-offset chunks of the same code. Either can
    // push the total past the 8-segment cap in rare cases (a correction
    // stranded far past band 31, or one spanning most of the spectrum) - the
    // segment-count truncation above already prioritised by magnitude, so
    // this second, band-order-driven cutoff only ever bites the tail of an
    // already-large run and is documented rather than silently mis-encoded.
    DeltaSegments out;
    int cursor = 0;
    for (const auto& run : runs) {
        int band = run.band;
        int remaining = run.length;
        while (remaining > 0) {
            while (band - cursor > 31) {
                if (out.deltnseg >= 8) {
                    return out;
                }
                out.deltoffst[static_cast<std::size_t>(out.deltnseg)] = 31;
                out.deltlen[static_cast<std::size_t>(out.deltnseg)] = 0;
                out.deltba[static_cast<std::size_t>(out.deltnseg)] = 4;  // inert: deltlen == 0
                ++out.deltnseg;
                cursor += 31;
            }
            if (out.deltnseg >= 8) {
                return out;
            }
            const int len = std::min(remaining, 15);
            out.deltoffst[static_cast<std::size_t>(out.deltnseg)] =
                static_cast<std::uint8_t>(band - cursor);
            out.deltlen[static_cast<std::size_t>(out.deltnseg)] = static_cast<std::uint8_t>(len);
            out.deltba[static_cast<std::size_t>(out.deltnseg)] =
                static_cast<std::uint8_t>(run.code);
            ++out.deltnseg;
            cursor = band + len;
            band += len;
            remaining -= len;
        }
    }
    return out;
}

}  // namespace ac3
