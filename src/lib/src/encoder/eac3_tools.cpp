#include "ac3/encoder/eac3_tools.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

#include "ac3/core/aht_tables.hpp"

namespace ac3::eac3 {

namespace {

// cos(j(2m+1)pi/12) for j, m in 0..5 - the shared kernel of both directions.
// Six by six of them, so a table rather than a call to cos per coefficient.
struct AhtKernel {
    std::array<std::array<double, kBlocksPerFrameSize>, kBlocksPerFrameSize> cell{};

    AhtKernel() {
        for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
            for (std::size_t m = 0; m < kBlocksPerFrameSize; ++m) {
                cell[j][m] = std::cos(static_cast<double>(j) *
                                      (2.0 * static_cast<double>(m) + 1.0) *
                                      std::numbers::pi / 12.0);
            }
        }
    }
};

// The constructor only fills a std::array via std::cos, which cannot throw
// for a finite argument; there is no allocation and nothing user-supplied to
// fail.
// NOLINTNEXTLINE(cert-err58-cpp)
const AhtKernel kKernel{};

// §E3.4.5's synthesis weights. The standard writes
//     C(k,m) = 2 * sum_j R_j X(k,j) cos(j(2m+1)pi/12),  R_j = 1, R_0 = 1/2
// but a plain-text extraction of the PDF renders a radical sign as nothing at
// all, and BOTH constants in that equation carry one. The real weights are
// sqrt(2) and R_0 = 1/sqrt(2), which is to say the synthesis basis is
//     w_0 = 1,  w_j = sqrt(2)
// - the classic DCT-III, orthogonal with every column at norm-squared 6.
//
// The misreading is not one an internal check can catch. Dropping both
// radicals leaves a perfectly good transform pair: it round-trips exactly, it
// keeps every coefficient in range, and the frame it produces decodes without
// complaint. What it does is make every AHT channel come back 1/sqrt(2)
// quiet, and only in the coefficients with j >= 1 - so a tone whose phase
// happens to repeat every block is reproduced perfectly while the one beside
// it is 3 dB down. Tones at both kinds of frequency, decoded and measured,
// are what separated the two readings.
constexpr double kW0 = 1.0;
const double kWj = std::numbers::sqrt2;

}  // namespace

void aht_forward(std::span<const double, kBlocksPerFrameSize> blocks,
                 std::span<double, kBlocksPerFrameSize> out) {
    for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
        double sum = 0.0;
        for (std::size_t m = 0; m < kBlocksPerFrameSize; ++m) {
            sum += blocks[m] * kKernel.cell[j][m];
        }
        // Analysis is synthesis transposed, divided by the basis norms: 6 at
        // j = 0 and 3 elsewhere, each over that index's synthesis weight.
        out[j] = j == 0 ? sum / (6.0 * kW0) : sum / (3.0 * kWj);
    }
}

void aht_inverse(std::span<const double, kBlocksPerFrameSize> coefficients,
                 std::span<double, kBlocksPerFrameSize> out) {
    for (std::size_t m = 0; m < kBlocksPerFrameSize; ++m) {
        double sum = 0.0;
        for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
            sum += (j == 0 ? kW0 : kWj) * coefficients[j] * kKernel.cell[j][m];
        }
        out[m] = sum;
    }
}

int aht_bin_bits(int hebap) {
    if (hebap <= 0) {
        return 0;
    }
    if (hebap <= 7) {
        return tables::kAhtVqIndexBits[static_cast<std::size_t>(hebap)];
    }
    return 6 * aht_mantissa_bits(hebap);
}

double spx_attenuation(int spxattencod, int index) {
    assert(spxattencod >= 0 && spxattencod < kSpxAttenCodes);
    // The table's three stored taps are the first three of a symmetric five,
    // so an index past the middle mirrors back.
    const int tap = index < 3 ? index : kSpxAttenTaps - 1 - index;
    return std::exp2(-static_cast<double>(spxattencod + 1) *
                     static_cast<double>(tap + 1) / 15.0);
}

void spx_apply_notch(std::span<double> synth, int startmant, const BandLayout& bands,
                     std::span<const bool> wrapflag, int spxattencod) {
    if (spxattencod < 0) {
        return;
    }
    const auto notch = [&](int centre) {
        for (int tap = 0; tap < kSpxAttenTaps; ++tap) {
            const int at = centre - 2 + tap - startmant;
            if (at < 0 || at >= static_cast<int>(synth.size())) {
                continue;
            }
            synth[static_cast<std::size_t>(at)] *= spx_attenuation(spxattencod, tap);
        }
    };
    notch(startmant);
    for (int bnd = 1; bnd < bands.count; ++bnd) {
        if (wrapflag[static_cast<std::size_t>(bnd)]) {
            notch(bands.start[static_cast<std::size_t>(bnd)]);
        }
    }
}

std::span<const int> aht_gaq_gains(int gaqmod) {
    // Table E3.3. Mode 1's gains reach only to hebap 11; modes 2 and 3 reach
    // to 16, which aht_gaq_endbap encodes.
    static constexpr std::array<int, 1> kNone = {1};
    static constexpr std::array<int, 2> kDouble = {1, 2};
    static constexpr std::array<int, 2> kQuad = {1, 4};
    static constexpr std::array<int, 3> kBoth = {1, 2, 4};
    switch (gaqmod) {
        case 1: return kDouble;
        case 2: return kQuad;
        case 3: return kBoth;
        default: return kNone;
    }
}

AhtMantissaCode aht_quantize_mantissa(double value, int mantissa_bits, int gain) {
    assert(mantissa_bits >= 3);
    AhtMantissaCode out;
    const auto mask = [](int bits) {
        return (static_cast<std::uint32_t>(1) << static_cast<unsigned>(bits)) - 1;
    };

    if (gain == 1) {
        // Table E3.5's unity-gain column, which is AC-3's symmetric quantizer:
        // 2^m - 1 levels spanning [-1, 1], the full-scale-negative symbol left
        // unused so it can serve as a tag under the other gains.
        const int levels = (1 << mantissa_bits) - 1;
        const int limit = (1 << (mantissa_bits - 1)) - 1;
        const int code =
            std::clamp(static_cast<int>(std::lround(value * levels / 2.0)), -limit, limit);
        out.code = static_cast<std::uint32_t>(code) & mask(mantissa_bits);
        out.bits = mantissa_bits;
        out.recon = 2.0 * code / levels;
        return out;
    }

    const int small_bits = gain == 2 ? mantissa_bits - 1 : mantissa_bits - 2;
    const int large_bits = gain == 2 ? mantissa_bits - 1 : mantissa_bits;
    const int small_half = 1 << (small_bits - 1);
    const double dead_zone = 1.0 / gain;
    const double large_step =
        gain == 2 ? 1.0 / ((1 << (mantissa_bits - 1)) - 1)
                  : 3.0 / ((1 << (mantissa_bits + 1)) - 2);

    // The small quantizer reads as a plain fractional two's complement value
    // and is then divided by the gain, so its reach stops just short of the
    // dead zone - which is exactly where the large one starts.
    const auto small = static_cast<int>(std::lround(value * small_half * gain));
    if (std::abs(small) < small_half) {
        out.code = static_cast<std::uint32_t>(small) & mask(small_bits);
        out.bits = small_bits;
        out.recon = static_cast<double>(small) / (small_half * gain);
        return out;
    }

    // Large: the tag, then a dead-zone codeword whose sign lives in the two's
    // complement wrap - non-negative codes count outwards from +dead_zone,
    // negative ones from -dead_zone.
    const int steps = (1 << (large_bits - 1)) - 1;
    const int k = std::clamp(
        static_cast<int>(std::lround((std::abs(value) - dead_zone) / large_step)), 0,
        steps);
    const int code = value >= 0.0 ? k : -k - 1;
    out.code = static_cast<std::uint32_t>(-small_half) & mask(small_bits);
    out.bits = small_bits;
    out.escape = static_cast<std::uint32_t>(code) & mask(large_bits);
    out.escape_bits = large_bits;
    out.recon = (value >= 0.0 ? 1.0 : -1.0) * (dead_zone + k * large_step);
    return out;
}

int aht_bin_gaq_bits(std::span<const double, kBlocksPerFrameSize> values,
                     int mantissa_bits, int gain) {
    int bits = 0;
    for (const double value : values) {
        const auto code = aht_quantize_mantissa(value, mantissa_bits, gain);
        bits += code.bits + code.escape_bits;
    }
    return bits;
}

int aht_choose_gain(std::span<const double, kBlocksPerFrameSize> values,
                    int mantissa_bits, int gaqmod) {
    int best = 1;
    int best_bits = std::numeric_limits<int>::max();
    for (const int gain : aht_gaq_gains(gaqmod)) {
        // Gk = 4 needs two bits of headroom in the small codeword, so the
        // narrowest quantizer cannot offer it.
        if (gain == 4 && mantissa_bits < 3) {
            continue;
        }
        const int bits = aht_bin_gaq_bits(values, mantissa_bits, gain);
        // Ties go to the LARGER gain. The gains are listed smallest first and
        // each one's large quantizer is finer than the last - Gk = 4 steps by
        // 1.5/(2^m - 1) where Gk = 1 steps by 2/(2^m - 1) - while their small
        // quantizers share a step. So when two gains cost the same, the
        // larger one reconstructs at least as well.
        if (bits <= best_bits) {
            best_bits = bits;
            best = gain;
        }
    }
    return best;
}

int aht_vector_quantize(std::span<double, kBlocksPerFrameSize> values, int hebap) {
    assert(hebap >= 1 && hebap <= 7);
    const auto book = tables::aht_vq_table(hebap);
    int best = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t entry = 0; entry < book.size(); ++entry) {
        double distance = 0.0;
        for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
            const double candidate =
                static_cast<double>(book[entry][j]) / 32768.0;
            const double error = values[j] - candidate;
            distance += error * error;
            if (distance >= best_distance) {
                break;  // no way back once it is already worse
            }
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(entry);
        }
    }
    for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
        values[j] = static_cast<double>(book[static_cast<std::size_t>(best)][j]) / 32768.0;
    }
    return best;
}

BandLayout group_bands(int first_bin, int subbands, int bins_per_subband,
                       std::span<const bool> structure) {
    assert(subbands >= 1 && subbands <= kMaxSubBands);
    assert(structure.size() >= static_cast<std::size_t>(subbands));

    BandLayout out;
    out.count = 1;
    out.start[0] = first_bin;
    out.size[0] = bins_per_subband;
    for (int sbnd = 1; sbnd < subbands; ++sbnd) {
        const auto band = static_cast<std::size_t>(out.count);
        if (structure[static_cast<std::size_t>(sbnd)]) {
            out.size[band - 1] += bins_per_subband;
        } else {
            out.start[band] = first_bin + sbnd * bins_per_subband;
            out.size[band] = bins_per_subband;
            ++out.count;
        }
    }
    return out;
}

}  // namespace ac3::eac3
