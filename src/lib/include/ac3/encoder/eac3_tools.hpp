#pragma once

#include <array>
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

}  // namespace ac3::eac3
