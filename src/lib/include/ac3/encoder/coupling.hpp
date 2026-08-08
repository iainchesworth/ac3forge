#pragma once

#include <array>
#include <cstdint>
#include <span>

// Channel coupling (A/52 §7.4, §8.2.4-8.2.5): above a chosen frequency the
// coupled channels stop carrying their own coefficients and share a single
// coupling channel, with per-channel per-band coupling coordinates restoring
// each channel's high-frequency envelope. That trades stereo/surround detail
// at the top of the spectrum for a large bit saving - the tool that makes
// 5.1 viable well below 448 kbit/s.

namespace ac3::coupling {

// §7.4.2: coefficients 37..252 form 18 sub-bands of 12.
inline constexpr int kFirstBin = 37;
inline constexpr int kBinsPerSubBand = 12;
inline constexpr int kSubBands = 18;

[[nodiscard]] constexpr int start_mant(int cplbegf) {
    return kFirstBin + kBinsPerSubBand * cplbegf;
}

// cplendf is interpreted by adding 3 (§5.4.3.12 / §7.4.2).
[[nodiscard]] constexpr int end_mant(int cplendf) {
    return kFirstBin + kBinsPerSubBand * (cplendf + 3);
}

[[nodiscard]] constexpr int sub_band_count(int cplbegf, int cplendf) {
    return 3 + cplendf - cplbegf;
}

// §5.4.3.13: consecutive sub-bands can be joined into one coupling BAND, and
// a coordinate is sent per band rather than per sub-band. The join pattern is
// cplbndstrc, one bit per sub-band after the first, and AC-3 always transmits
// it - unlike Annex E, which also offers a default table.
struct BandLayout {
    int count = 0;                          // ncplbnd
    std::array<int, kSubBands> start{};     // first bin of each band, absolute
    std::array<int, kSubBands> size{};      // bins in each band
};

// structure[i] set merges sub-band i into the band before it; structure[0] is
// never consulted, because the first sub-band always opens a band. Indices
// count from the FIRST COUPLED sub-band, which is how cplbndstrc is numbered.
[[nodiscard]] BandLayout group_bands(int cplbegf, int subbands, std::span<const bool> structure);

// The structure this encoder asks for. A coordinate restores a band's level,
// so a band wants to be about as wide as the ear's own resolution up there:
// critical bandwidth is roughly 2 kHz at 10 kHz and 4 kHz at 15, against a
// sub-band's 1125 Hz. The bands therefore widen with frequency, and how many
// there are depends on where coupling starts as well as how far it runs.
[[nodiscard]] std::array<bool, kSubBands> band_structure(int cplbegf, int subbands);

// The transmitted form of one coupling coordinate (§7.4.3). The master is
// per channel, shared by all of that channel's bands; exponent and mantissa
// are per band.
struct Coordinate {
    std::uint8_t exp = 0;   // cplcoexp, 4 bits
    std::uint8_t mant = 0;  // cplcomant, 4 bits
};

// Annex E's spectral extension coordinate (§E3.6.3) is the same format with
// a narrower mantissa: 4-bit exponent, 2-bit mantissa, 2-bit per-channel
// master buying three exponent steps at a time, implicit leading one except
// at exponent 15 where the mantissa becomes a plain fraction. Only the
// mantissa width differs, so it is a parameter rather than a second copy of
// the arithmetic.
inline constexpr int kCplMantissaBits = 4;
inline constexpr int kSpxMantissaBits = 2;

// Reconstruct exactly as the decoder does (§7.4.3), so the encoder can see
// the value the decoder will actually apply.
[[nodiscard]] double decode_coordinate(Coordinate coordinate, int master,
                                       int mantissa_bits = kCplMantissaBits);

// Quantize a linear coupling coordinate for a given per-channel master.
// Values are clamped into the representable range rather than wrapping.
[[nodiscard]] Coordinate quantize_coordinate(double value, int master,
                                             int mantissa_bits = kCplMantissaBits);

// The smallest master (0..3) that keeps every coordinate in `values`
// representable. The master buys 3 exponent steps at a time, extending the
// range downward by 54 dB in total.
[[nodiscard]] int choose_master(std::span<const double> values);

}  // namespace ac3::coupling
