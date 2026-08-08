#include "ac3/encoder/coupling.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace ac3::coupling {

namespace {

// §7.4.3: exponent 15 switches the mantissa from the implicit-leading-one
// form to a plain fraction, which is what lets very small coordinates be
// represented at all.
constexpr int kMaxExp = 15;
constexpr int kMaxMaster = 3;

// How many sub-bands a band starting at this coefficient should span. The
// steps are where a critical band passes one and two sub-band widths: bin
// 117 is 11.0 kHz and bin 181 is 17.0 kHz at 48 kHz, against a sub-band's
// 1125 Hz. Below the first step a sub-band is already coarser than the ear,
// so nothing is gained by joining any.
constexpr int band_width(int start_bin) {
    if (start_bin < 117) {
        return 1;
    }
    return start_bin < 181 ? 2 : 3;
}

}  // namespace

double decode_coordinate(Coordinate coordinate, int master) {
    const double mantissa = coordinate.exp == kMaxExp
                                ? coordinate.mant / 16.0
                                : (coordinate.mant + 16) / 32.0;
    return std::ldexp(mantissa, -(coordinate.exp + 3 * master));
}

Coordinate quantize_coordinate(double value, int master) {
    if (!(value > 0.0)) {
        return {.exp = kMaxExp, .mant = 0};
    }

    // Find the shift that lands the value in [0.5, 1), which is the range the
    // implicit-leading-one mantissa encodes. The master already contributes
    // 3 * master of that shift.
    int shift = static_cast<int>(std::floor(-std::log2(value)));
    shift = std::max(shift, 0);
    int exp = shift - 3 * master;

    if (exp < 0) {
        // Louder than this master allows: clamp to the largest coordinate.
        return {.exp = 0, .mant = 15};
    }
    if (exp >= kMaxExp) {
        // Quieter than the implicit-one form reaches: use the exp==15 escape,
        // whose mantissa is a plain fraction and can go all the way to zero.
        const double scaled = std::ldexp(value, kMaxExp + 3 * master);
        const auto mant = static_cast<int>(std::lround(scaled * 16.0));
        return {.exp = kMaxExp, .mant = static_cast<std::uint8_t>(std::clamp(mant, 0, 15))};
    }

    const double scaled = std::ldexp(value, exp + 3 * master);  // now in [0.5, 1)
    auto mant = static_cast<int>(std::lround(scaled * 32.0)) - 16;
    if (mant > 15) {
        // Rounding pushed it to the next binade; renormalise one step up.
        if (exp == 0) {
            return {.exp = 0, .mant = 15};
        }
        --exp;
        mant = static_cast<int>(std::lround(std::ldexp(value, exp + 3 * master) * 32.0)) - 16;
    }
    return {.exp = static_cast<std::uint8_t>(exp),
            .mant = static_cast<std::uint8_t>(std::clamp(mant, 0, 15))};
}

BandLayout group_bands(int cplbegf, int subbands, std::span<const bool> structure) {
    assert(subbands >= 1 && subbands <= kSubBands);
    assert(structure.size() >= static_cast<std::size_t>(subbands));

    const int first_bin = start_mant(cplbegf);
    BandLayout out;
    out.count = 1;
    out.start[0] = first_bin;
    out.size[0] = kBinsPerSubBand;
    for (int sbnd = 1; sbnd < subbands; ++sbnd) {
        const auto band = static_cast<std::size_t>(out.count);
        if (structure[static_cast<std::size_t>(sbnd)]) {
            out.size[band - 1] += kBinsPerSubBand;
        } else {
            out.start[band] = first_bin + sbnd * kBinsPerSubBand;
            out.size[band] = kBinsPerSubBand;
            ++out.count;
        }
    }
    return out;
}

std::array<bool, kSubBands> band_structure(int cplbegf, int subbands) {
    std::array<bool, kSubBands> out{};
    const int first_bin = start_mant(cplbegf);
    int band_start = first_bin;
    int width = 1;
    for (int sbnd = 1; sbnd < subbands; ++sbnd) {
        if (width < band_width(band_start)) {
            out[static_cast<std::size_t>(sbnd)] = true;
            ++width;
        } else {
            band_start = first_bin + sbnd * kBinsPerSubBand;
            width = 1;
        }
    }
    return out;
}

int choose_master(std::span<const double> values) {
    // The master must cover the LOUDEST coordinate (a too-large master would
    // push it below exponent 0 and clip it); quiet bands are handled by the
    // exp==15 escape, so they never force the master up.
    double loudest = 0.0;
    for (const double value : values) {
        loudest = std::max(loudest, value);
    }
    if (!(loudest > 0.0)) {
        return kMaxMaster;
    }
    const int shift = std::max(0, static_cast<int>(std::floor(-std::log2(loudest))));
    // master * 3 must not exceed the loudest value's own shift.
    return std::clamp(shift / 3, 0, kMaxMaster);
}

}  // namespace ac3::coupling
