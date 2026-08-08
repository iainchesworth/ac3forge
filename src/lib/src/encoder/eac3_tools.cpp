#include "ac3/encoder/eac3_tools.hpp"

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
