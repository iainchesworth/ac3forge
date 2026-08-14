#include "ac3/encoder/eac3_tools.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

#include "ac3/core/aht_tables.hpp"
#include "ac3/core/fft.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/window.hpp"
#include "ac3/internal/profiling.hpp"

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

double spx_noise_ratio(int band_start, int band_size, int endmant, int blend) {
    const double centre = band_start + 0.5 * band_size;
    const double ratio = centre / static_cast<double>(endmant) - static_cast<double>(blend) / 32.0;
    return std::clamp(ratio, 0.0, 1.0);
}

double SpxNoise::next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    // sqrt(3): a uniform distribution on [-a, a] has variance a^2/3, so this
    // is the radius that makes the mapped value unit-variance.
    constexpr double kRadius = 1.7320508075688772;
    const double unit = static_cast<double>(state) / static_cast<double>(0xFFFFFFFFU);  // [0,1]
    return (unit * 2.0 - 1.0) * kRadius;
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

double aht_dequantize_mantissa(std::uint32_t code, std::uint32_t escape, bool has_escape,
                               int mantissa_bits, int gain) {
    const auto sign_extend = [](std::uint32_t raw, int bits) {
        const auto sign_bit = static_cast<std::uint32_t>(1) << (bits - 1);
        return static_cast<int>((raw ^ sign_bit) - sign_bit);
    };

    if (gain == 1) {
        const int levels = (1 << mantissa_bits) - 1;
        return 2.0 * sign_extend(code, mantissa_bits) / levels;
    }

    const int small_bits = gain == 2 ? mantissa_bits - 1 : mantissa_bits - 2;
    const int large_bits = gain == 2 ? mantissa_bits - 1 : mantissa_bits;
    const int small_half = 1 << (small_bits - 1);
    const double dead_zone = 1.0 / gain;
    const double large_step =
        gain == 2 ? 1.0 / ((1 << (mantissa_bits - 1)) - 1)
                  : 3.0 / ((1 << (mantissa_bits + 1)) - 2);

    if (!has_escape) {
        return static_cast<double>(sign_extend(code, small_bits)) / (small_half * gain);
    }

    // The mirror image of quantize's `code = value >= 0.0 ? k : -k - 1`.
    const int large_code = sign_extend(escape, large_bits);
    const int k = large_code >= 0 ? large_code : -large_code - 1;
    return (large_code >= 0 ? 1.0 : -1.0) * (dead_zone + k * large_step);
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
    AC3_ZONE_SCOPED_N("aht_choose_gain");
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
    AC3_ZONE_SCOPED_N("aht_vector_quantize");
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

BandLayout ecpl_group_bands(int begin_subbnd, int end_subbnd, std::span<const bool> structure) {
    assert(begin_subbnd >= 0 && end_subbnd <= kEcplSubBands && begin_subbnd < end_subbnd);
    assert(structure.size() >= static_cast<std::size_t>(end_subbnd));

    BandLayout out;
    out.count = 1;
    out.start[0] = kEcplSubBandTab[static_cast<std::size_t>(begin_subbnd)];
    out.size[0] = kEcplSubBandTab[static_cast<std::size_t>(begin_subbnd) + 1] - out.start[0];
    for (int sbnd = begin_subbnd + 1; sbnd < end_subbnd; ++sbnd) {
        const int start = kEcplSubBandTab[static_cast<std::size_t>(sbnd)];
        const int width = kEcplSubBandTab[static_cast<std::size_t>(sbnd) + 1] - start;
        const auto band = static_cast<std::size_t>(out.count);
        if (structure[static_cast<std::size_t>(sbnd)]) {
            out.size[band - 1] += width;
        } else {
            out.start[band] = start;
            out.size[band] = width;
            ++out.count;
        }
    }
    return out;
}

namespace {

// Table E3.10: {exptab, manttab} for ecplamp 0..30. Index 31 is handled as a
// special case (amplitude 0) rather than stored here.
constexpr std::array<std::pair<int, int>, 31> kEcplAmpTab = {{
    {0, 0x20}, {0, 0x1b}, {0, 0x17}, {0, 0x13}, {0, 0x10}, {1, 0x1b}, {1, 0x17}, {1, 0x13},
    {1, 0x10}, {2, 0x1b}, {2, 0x17}, {2, 0x13}, {2, 0x10}, {3, 0x1b}, {3, 0x17}, {3, 0x13},
    {3, 0x10}, {4, 0x1b}, {4, 0x17}, {4, 0x13}, {4, 0x10}, {5, 0x1b}, {5, 0x17}, {5, 0x13},
    {5, 0x10}, {6, 0x1b}, {6, 0x17}, {6, 0x13}, {6, 0x10}, {7, 0x1b}, {7, 0x17},
}};

}  // namespace

double decode_ecplamp(int ecplamp) {
    assert(ecplamp >= 0 && ecplamp <= 31);
    if (ecplamp == 31) {
        return 0.0;
    }
    const auto [exp, mant] = kEcplAmpTab[static_cast<std::size_t>(ecplamp)];
    return std::ldexp(static_cast<double>(mant) / 32.0, -exp);
}

int quantize_ecplamp(double value) {
    if (!(value > 0.0)) {
        return 31;
    }
    int best = 0;
    double best_error = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 31; ++i) {
        const double error = std::abs(decode_ecplamp(i) - value);
        if (error < best_error) {
            best_error = error;
            best = i;
        }
    }
    // A value quieter than every real entry reconstructs closer to silence
    // (index 31) than to the smallest real step.
    if (std::abs(0.0 - value) < best_error) {
        return 31;
    }
    return best;
}

int quantize_ecplangle(double angle) {
    const long raw = std::lround(angle * 32.0);
    return static_cast<int>(((raw % 64) + 64) % 64);
}

int quantize_ecplchaos(double chaos) {
    return std::clamp(static_cast<int>(std::lround(-chaos * 7.0)), 0, 7);
}

namespace {

// y[m] = cos(2*pi*(N/4 + 0.5)/N*(m + 0.5)), N = 512, m = 0..255 (§3.5.5.4).
struct EcplYTable {
    std::array<double, 256> value{};
    EcplYTable() {
        constexpr double kPi = std::numbers::pi;
        constexpr double kN = 512.0;
        for (int m = 0; m < 256; ++m) {
            value[static_cast<std::size_t>(m)] =
                std::cos(2.0 * kPi * (kN / 4.0 + 0.5) / kN * (static_cast<double>(m) + 0.5));
        }
    }
};

const EcplYTable& ecpl_y() {
    static const EcplYTable t;
    return t;
}

// §3.5.5.4 step 3: xcos3[i] = cos(pi*i/512), xsin3[i] = -sin(pi*i/512) for
// i = 0..511 - the windowing loop below reads index i = n for the first
// half and i = n+256 for the second, both landing in this one table. This
// depends only on i, never on the PCM being windowed, so it's the same
// fixed pair of arrays on every call: tabulated once here instead of
// std::cos/std::sin (x4 per sample) inside the 256-iteration loop.
struct Xcos3Table {
    std::array<double, 512> cos{};
    std::array<double, 512> sin{};
    Xcos3Table() {
        constexpr double kPi = std::numbers::pi;
        for (int i = 0; i < 512; ++i) {
            const double angle = kPi * static_cast<double>(i) / 512.0;
            cos[static_cast<std::size_t>(i)] = std::cos(angle);
            sin[static_cast<std::size_t>(i)] = -std::sin(angle);
        }
    }
};

const Xcos3Table& xcos3_table() {
    static const Xcos3Table t;
    return t;
}

}  // namespace

void ecpl_channel_spectrum(std::span<const double, 256> prev_mant,
                           std::span<const double, 256> curr_mant,
                           std::span<const double, 256> next_mant, std::span<double, 256> real_out,
                           std::span<double, 256> imag_out) {
    AC3_ZONE_SCOPED_N("ecpl_channel_spectrum");
    // Step 1: three independent 512-sample normative IMDCTs (§7.9.4.1
    // steps 1-5, the exact machinery every other coefficient set in this
    // decoder already goes through).
    std::array<double, 512> x_prev{};
    std::array<double, 512> x_curr{};
    std::array<double, 512> x_next{};
    imdct512_windowed(prev_mant, x_prev);
    imdct512_windowed(curr_mant, x_curr);
    imdct512_windowed(next_mant, x_next);

    // Step 2: overlap the second half of the previous block and the first
    // half of the next block with the current one.
    std::array<double, 512> pcm{};
    for (int n = 0; n < 256; ++n) {
        pcm[static_cast<std::size_t>(n)] =
            x_prev[static_cast<std::size_t>(n) + 256] + x_curr[static_cast<std::size_t>(n)];
        pcm[static_cast<std::size_t>(n) + 256] =
            x_curr[static_cast<std::size_t>(n) + 256] + x_next[static_cast<std::size_t>(n)];
    }

    // Step 3: window again and apply the xcos3/xsin3 twiddle so the
    // subsequent DFT lands as an oddly-stacked filterbank, matching the
    // MDCT. w[N/2-n-1] mirrors from the OPPOSITE end of the window than
    // w[n] does for the first half - not the same value as w[n] itself.
    std::array<double, 512> pcm_real{};
    std::array<double, 512> pcm_imag{};
    const auto& xcos3 = xcos3_table();
    for (int n = 0; n < 256; ++n) {
        const auto un = static_cast<std::size_t>(n);
        const std::size_t un2 = un + 256;
        const double xcos3_n = xcos3.cos[un];
        const double xsin3_n = xcos3.sin[un];
        const double xcos3_n2 = xcos3.cos[un2];
        const double xsin3_n2 = xcos3.sin[un2];
        pcm_real[un] = pcm[un] * kAnalysisWindow[un] * xcos3_n;
        pcm_imag[un] = pcm[un] * kAnalysisWindow[un] * xsin3_n;
        pcm_real[un2] = pcm[un2] * kAnalysisWindow[255 - un] * xcos3_n2;
        pcm_imag[un2] = pcm[un2] * kAnalysisWindow[255 - un] * xsin3_n2;
    }

    // Step 4: the full complex DFT. Only bins 0..255 are ever consumed
    // downstream (§3.5.5.4), so only those are copied out.
    std::array<double, 512> zr{};
    std::array<double, 512> zi{};
    dft512(pcm_real, pcm_imag, zr, zi);
    for (int k = 0; k < 256; ++k) {
        real_out[static_cast<std::size_t>(k)] = zr[static_cast<std::size_t>(k)];
        imag_out[static_cast<std::size_t>(k)] = zi[static_cast<std::size_t>(k)];
    }
}

double ecpl_rand_notrans(int channel, int bin) {
    // A hash of (channel, bin) rather than a stored table - deterministic
    // and stable for the stream's lifetime (the spec's two requirements),
    // without needing per-decoder persistent state to satisfy them. The
    // exact generator is unspecified by the standard, the same freedom
    // SpxNoise documents for its own noise generator.
    std::uint32_t state = static_cast<std::uint32_t>(channel) * 0x9E3779B1U ^
                          static_cast<std::uint32_t>(bin) * 0x85EBCA77U ^ 0xC2B2AE3DU;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const double unit = static_cast<double>(state) / static_cast<double>(0xFFFFFFFFU);  // [0,1]
    return unit * 2.0 - 1.0;  // [-1, 1], matching §3.5.5.3's uniform (not unit-variance)
}

double EcplNoise::next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const double unit = static_cast<double>(state) / static_cast<double>(0xFFFFFFFFU);  // [0,1]
    return unit * 2.0 - 1.0;
}

void ecpl_amplitudes(std::span<const int> ecplamp, std::span<const int> ecplchaos, bool ecpltrans,
                     bool is_first_channel, int begin_subbnd, int end_subbnd,
                     std::span<const bool> structure, std::span<double> amp_out) {
    int band = -1;
    std::size_t cursor = 0;
    for (int sbnd = begin_subbnd; sbnd < end_subbnd; ++sbnd) {
        if (sbnd == begin_subbnd || !structure[static_cast<std::size_t>(sbnd)]) {
            ++band;
        }
        const auto b = static_cast<std::size_t>(band);
        double amp = decode_ecplamp(ecplamp[b]);
        // §3.5.5.2: the chaos modification is skipped for the first coupled
        // channel (whose chaos is defined as zero) and whenever this
        // channel's block carries a transient.
        if (!is_first_channel && !ecpltrans) {
            amp *= 1.0 + 0.38 * decode_ecplchaos(ecplchaos[b]);
        }
        const int start = kEcplSubBandTab[static_cast<std::size_t>(sbnd)];
        const int width = kEcplSubBandTab[static_cast<std::size_t>(sbnd) + 1] - start;
        for (int i = 0; i < width; ++i) {
            amp_out[cursor++] = amp;
        }
    }
}

void ecpl_angles(int channel, std::span<const int> ecplangle, std::span<const int> ecplchaos,
                 bool ecpltrans, bool is_first_channel, int begin_subbnd, int end_subbnd,
                 std::span<const bool> structure, EcplNoise& noise, std::span<double> angle_out) {
    int band = -1;
    std::size_t cursor = 0;
    double band_angle = 0.0;
    double band_chaos = 0.0;
    double band_rand_trans = 0.0;
    for (int sbnd = begin_subbnd; sbnd < end_subbnd; ++sbnd) {
        if (sbnd == begin_subbnd || !structure[static_cast<std::size_t>(sbnd)]) {
            ++band;
            const auto b = static_cast<std::size_t>(band);
            band_angle = is_first_channel ? 0.0 : decode_ecplangle(ecplangle[b]);
            band_chaos = is_first_channel ? 0.0 : decode_ecplchaos(ecplchaos[b]);
            // rand_trans is per-BAND (unlike rand_notrans, which is per-bin
            // and drawn below instead) - one fresh draw per band, every
            // block, then duplicated across that band's bins same as chaos.
            if (ecpltrans) {
                band_rand_trans = noise.next();
            }
        }
        const int start = kEcplSubBandTab[static_cast<std::size_t>(sbnd)];
        const int width = kEcplSubBandTab[static_cast<std::size_t>(sbnd) + 1] - start;
        for (int i = 0; i < width; ++i) {
            const int bin = start + i;
            const double rand = ecpltrans ? band_rand_trans : ecpl_rand_notrans(channel, bin);
            double angle = band_angle + band_chaos * rand;
            if (angle < -1.0) {
                angle += 2.0;
            } else if (angle >= 1.0) {
                angle -= 2.0;
            }
            angle_out[cursor++] = angle;
        }
    }
}

void ecpl_channel_coefficients(std::span<const double, 256> real_in,
                               std::span<const double, 256> imag_in,
                               std::span<const double> amp_bin, std::span<const double> angle_bin,
                               int begin_mant, int end_mant, std::span<double, 256> mant_out) {
    const auto& y = ecpl_y().value;
    constexpr double kPi = std::numbers::pi;
    for (int bin = begin_mant; bin < end_mant; ++bin) {
        const auto idx = static_cast<std::size_t>(bin - begin_mant);
        const double amp = amp_bin[idx];
        const double angle = angle_bin[idx];
        const double c = std::cos(kPi * angle);
        const double s = std::sin(kPi * angle);
        const double zr = real_in[static_cast<std::size_t>(bin)];
        const double zi = imag_in[static_cast<std::size_t>(bin)];
        const double zr_ch = zr * amp * c - zi * amp * s;
        const double zi_ch = zi * amp * c + zr * amp * s;
        // N/2 - 1 - bin, N = 512.
        const auto mirror = static_cast<std::size_t>(255 - bin);
        mant_out[static_cast<std::size_t>(bin)] =
            -2.0 * (y[static_cast<std::size_t>(bin)] * zr_ch + y[mirror] * zi_ch);
    }
}

}  // namespace ac3::eac3
