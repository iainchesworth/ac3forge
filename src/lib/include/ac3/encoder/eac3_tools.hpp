#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The Annex E coding tools' shared machinery: the sub-band groupings that
// coupling and spectral extension both express their coordinates over.
//
// Both tools slice the spectrum into fixed 12-coefficient sub-bands and then
// merge runs of them into wider BANDS, one coordinate per band. The merge
// pattern is a bit array with the same meaning in both tools ("this sub-band
// continues the previous band"), so the grouping arithmetic is written once
// here rather than twice at the two call sites.

namespace ac3::eac3 {

// The widest sub-band count any of the tools reaches: enhanced coupling's 22
// (§E3.5.2). Standard coupling has 18 and spectral extension 17.
inline constexpr int kMaxSubBands = 22;

// The adaptive hybrid transform's length: one coefficient per audio block.
inline constexpr std::size_t kBlocksPerFrameSize = 6;

struct BandLayout {
    int count = 0;                                // bands
    std::array<int, kMaxSubBands> start{};        // first bin, absolute
    std::array<int, kMaxSubBands> size{};         // bins in the band
};

// Group `subbands` consecutive sub-bands of `bins_per_subband` bins each,
// beginning at `first_bin`. structure[i] set merges sub-band i into the
// previous band; structure[0] is never consulted, because the first sub-band
// always opens a band (§E2.3.3.8: "the first band is assumed to be '0' and
// not sent"). `structure` is indexed from the FIRST sub-band of the region,
// so a caller whose default table is indexed absolutely must slice it.
[[nodiscard]] BandLayout group_bands(int first_bin, int subbands, int bins_per_subband,
                                     std::span<const bool> structure);

// Table E2.12, the structure a decoder falls back on when cplbndstrce is 0 in
// the first coupled block. It is used here as the SHAPE worth asking for - it
// merges the top sub-bands into wider bands, so the top of the spectrum costs
// a handful of coordinates rather than one per sub-band - but it is then
// TRANSMITTED rather than left to the default.
//
// Defaulting does not survive contact with a real decoder. §5.4.3.13 pins the
// array's first element to sub-band cplbegf, which makes the table's index
// relative to where coupling starts; read that way, a stream with cplbegf 0
// decodes and every other value yields out-of-range exponents. Sending the
// structure costs ncplsubnd - 1 bits a frame and removes the question, which
// is the better trade for something a decoder cannot tell you it disagrees
// about except by producing noise.
inline constexpr std::array<bool, 18> kDefaultCplBandStructure = {
    false, false, false, false, false, false, false, false, true,
    false, true,  true,  false, true,  true,  true,  true,  true,
};

// §7.4.2: coupling sub-bands are 12 coefficients wide, starting at 37.
inline constexpr int kCplFirstBin = 37;
inline constexpr int kCplBinsPerSubBand = 12;

// --- spectral extension (§E3.6.2, Table E3.13) -----------------------------
// Coefficients 25 through 228 in 17 sub-bands of 12. The table's final entry
// is not a sub-band: it exists so spx_end_subbnd can name the bin one past
// the last synthesized one.
inline constexpr int kSpxFirstBin = 25;
inline constexpr int kSpxBinsPerSubBand = 12;
inline constexpr int kSpxSubBands = 17;

[[nodiscard]] constexpr int spx_band_start(int subbnd) {
    return kSpxFirstBin + kSpxBinsPerSubBand * subbnd;
}

// §E2.3.3.5 and §E2.3.3.6. Both codes are non-linear at the top, which is why
// they are tables in disguise rather than plain offsets.
[[nodiscard]] constexpr int spx_begin_subbnd(int spxbegf) {
    return spxbegf < 6 ? spxbegf + 2 : spxbegf * 2 - 3;
}

[[nodiscard]] constexpr int spx_end_subbnd(int spxendf) {
    return spxendf < 3 ? spxendf + 5 : spxendf * 2 + 3;
}

// §E3.3.1: with spectral extension in use, cplendf is NOT transmitted. It is
// derived from spxbegf so that the coupling region ends exactly where
// synthesis begins - and the spec notes it may come out negative, which is
// legal precisely because it is never sent.
[[nodiscard]] constexpr int derived_cplendf(int spxbegf) {
    return spxbegf < 6 ? spxbegf - 2 : spxbegf * 2 - 7;
}

// The three regions have to tile the spectrum with no gap and no overlap:
// coupling ends where synthesis starts, or a decoder reconstructs a band
// twice or not at all. This is the identity that guarantees it.
static_assert(kCplFirstBin + kCplBinsPerSubBand * (derived_cplendf(3) + 3) ==
              spx_band_start(spx_begin_subbnd(3)));
static_assert(kCplFirstBin + kCplBinsPerSubBand * (derived_cplendf(7) + 3) ==
              spx_band_start(spx_begin_subbnd(7)));

// §E2.3.3.7, Table E2.11. Unlike coupling's, this array is indexed by the
// ABSOLUTE sub-band number: the transmitted loop runs bnd from
// spx_begin_subbnd + 1 to spx_end_subbnd, and §E3.6.2's band-size pseudocode
// reads it over the same absolute range.
inline constexpr std::array<bool, kSpxSubBands> kDefaultSpxBandStructure = {
    false, false, false, false, false, false, false, false, true,
    false, true,  false, true,  false, true,  false, true,
};

// --- adaptive hybrid transform (§E3.4) -------------------------------------
// A second transform stage, cascaded after the MDCT: a 6-point DCT-II taken
// down each spectral bin across the frame's six blocks. For material that is
// not changing between blocks it concentrates six coefficients into
// essentially one, which is where the coding gain comes from - and for
// material that IS changing it spreads them over all six and costs, which is
// why it is a decision the encoder makes per channel per frame.

// §E3.4.5, inverted. The standard gives the decoder's transform,
//   C(k,m) = 2 * sum_j R_j X(k,j) cos(j(2m+1)pi/12),  R_0 = 1/sqrt(2)
// whose basis vectors are orthogonal with norm 12, so the forward direction
// is the same sum scaled by 1/6 (and a further 1/sqrt(2) at j = 0).
// `blocks` are the six normalised MDCT coefficients of one bin; `out` takes
// the six AHT coefficients.
void aht_forward(std::span<const double, kBlocksPerFrameSize> blocks,
                 std::span<double, kBlocksPerFrameSize> out);

// The decoder's direction, so the encoder can see what it will reconstruct.
void aht_inverse(std::span<const double, kBlocksPerFrameSize> coefficients,
                 std::span<double, kBlocksPerFrameSize> out);

// Table E3.2: mantissa bits per coefficient for the scalar hebap range 8-19.
// Outside it the answer is not a per-coefficient width at all - hebap 0 codes
// nothing and 1-7 code all six coefficients as one VQ index - so those return
// zero and the caller must handle them.
[[nodiscard]] constexpr int aht_mantissa_bits(int hebap) {
    constexpr std::array<int, 20> kBits = {0, 0, 0, 0, 0, 0, 0, 0, 3,  4,
                                           5, 6, 7, 8, 9, 10, 11, 12, 14, 16};
    return hebap >= 0 && hebap < 20 ? kBits[static_cast<std::size_t>(hebap)] : 0;
}

// Bits one bin costs for the WHOLE frame under AHT: one VQ index in the
// vector range, six scalar mantissas above it.
[[nodiscard]] int aht_bin_bits(int hebap);

// Nearest codebook entry for a bin's six coefficients, by Euclidean distance
// (§E3.4.4.1). hebap must be in 1..7. Writes the reconstruction the decoder
// will use back into `values`.
[[nodiscard]] int aht_vector_quantize(std::span<double, kBlocksPerFrameSize> values,
                                      int hebap);

}  // namespace ac3::eac3
