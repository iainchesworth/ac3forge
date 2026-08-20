#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/export.hpp"

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
    int count = 0;                          // bands
    std::array<int, kMaxSubBands> start{};  // first bin, absolute
    std::array<int, kMaxSubBands> size{};   // bins in the band
};

// Group `subbands` consecutive sub-bands of `bins_per_subband` bins each,
// beginning at `first_bin`. structure[i] set merges sub-band i into the
// previous band; structure[0] is never consulted, because the first sub-band
// always opens a band (§E2.3.3.8: "the first band is assumed to be '0' and
// not sent"). `structure` is indexed from the FIRST sub-band of the region,
// so a caller whose default table is indexed absolutely must slice it.
[[nodiscard]] AC3FORGE_EXPORT BandLayout group_bands(int first_bin, int subbands,
                                                     int bins_per_subband,
                                                     std::span<const bool> structure);

// Table E2.12, the structure a decoder falls back on when cplbndstrce is 0 in
// a coupled block. It is used here as the SHAPE worth asking for - it merges
// the top sub-bands into wider bands, so the top of the spectrum costs a
// handful of coordinates rather than one per sub-band - but it is then
// TRANSMITTED rather than left to the default, so this encoder's own streams
// never rely on the fallback.
//
// The array is indexed ABSOLUTELY from cplbegf == 0 (confirmed against
// FFmpeg's decode_band_structure(), which memcpy's this table in full and
// only then offsets its read pointer by cplbegf), not relative to wherever
// this block's coupling region actually starts. A decoder applying it against
// a stream with cplbegf != 0 must slice from kDefaultCplBandStructure[cplbegf]
// onward, not from index 0.
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

// §E3.6.4.2.3: a five-tap notch straddling the border between the coded band
// and the synthesized one, and every point where the copy wraps back to the
// start of its source. Those are the places the translation leaves a seam -
// two unrelated pieces of spectrum butted together - and the notch tapers
// across it. The taps are t0, t1, t2, t1, t0 centred on the first synthesized
// bin, so the deepest attenuation sits exactly on the join and two CODED bins
// below it get attenuated too.
inline constexpr int kSpxAttenTaps = 5;
inline constexpr int kSpxAttenCodes = 32;

// Table E3.14 is 2^(-(spxattencod + 1) * (index + 1) / 15) throughout - all 96
// of its entries agree with this to within the precision it prints them at, so
// there is nothing to transcribe and nothing to get wrong transcribing.
[[nodiscard]] AC3FORGE_EXPORT double spx_attenuation(int spxattencod, int index);

// Apply the notch to a synthesized region, in place. `synth` covers the
// extension region from `startmant` upwards; `bands` and `wrapflag` say where
// the copy wrapped, and so where the seams are besides the first one.
//
// Two of the five taps at each seam fall BELOW the band they are centred on.
// At the first seam that puts them on coded coefficients, which the decoder
// attenuates and this does not - they are already quantized and belong to no
// extension band, so nothing downstream of here needs to know. At a wrap the
// same two taps land on the end of the previous band, which very much is
// ours, and dropping them would leave the encoder's idea of that band's
// energy too high.
AC3FORGE_EXPORT void spx_apply_notch(std::span<double> synth, int startmant,
                                     const BandLayout& bands, std::span<const bool> wrapflag,
                                     int spxattencod);

// §E3.6.4.2.1: how much of a band's synthesized content is pseudo-random
// noise versus the translated low-band copy, encode and decode alike -
// `nratio` in the standard's pseudocode. `band_start`/`band_size` locate the
// band in the coefficient domain; `endmant` is the extension region's
// exclusive end (spx_band_start(spx_end_subbnd)); `blend` is the transmitted
// spxblnd (0..31).
[[nodiscard]] AC3FORGE_EXPORT double spx_noise_ratio(int band_start, int band_size, int endmant,
                                                     int blend);

// §E3.6.4.2.4's noise(): "a pseudo-random number generated from a zero-mean,
// unity-variance noise generator." The standard deliberately leaves the exact
// generator unspecified - the same class of freedom AC-3's own dither
// sequence has (§7.3.4, "any reasonably random sequence") - so any generator
// meeting that shape is spec-conformant. This one is a plain xorshift32
// mapped onto a symmetric ±sqrt(3) uniform distribution (variance a²/3, so
// a = sqrt(3) gives variance 1 with zero mean by symmetry). Deterministic:
// the same stream always decodes to the same PCM.
struct AC3FORGE_EXPORT SpxNoise {
    std::uint32_t state = 0x9E3779B9U;  // never zero, or xorshift sticks at 0
    [[nodiscard]] double next();
};

// --- enhanced coupling (§E3.5) ----------------------------------------------
// An alternative to standard coupling (§E3.3): 22 sub-bands instead of 18,
// amplitude/angle/chaos-quantized coordinates instead of exponent/mantissa
// ones, and a phase-restoring reconstruction built on a full complex DFT
// rather than a plain per-band scale factor. Selected by ecplinu; standard
// and enhanced coupling are mutually exclusive per block, never combined.

inline constexpr int kEcplSubBands = kMaxSubBands;  // 22, §E3.5.2
inline constexpr int kEcplFirstBin = 13;

// Table E3.9: absolute bin boundaries of the 22 enhanced coupling sub-bands
// (0..3 are 6 bins wide, 4..21 are 12 - unlike standard coupling's uniform
// 12, so the table is boundaries rather than a first-bin/width pair).
// Element 22 is one past the last real sub-band, letting a caller compute
// sub-band widths as kEcplSubBandTab[sbnd + 1] - kEcplSubBandTab[sbnd] for
// every sbnd including the last.
inline constexpr std::array<int, kEcplSubBands + 1> kEcplSubBandTab = {
    13,  19,  25,  31,  37,  49,  61,  73,  85,  97,  109, 121,
    133, 145, 157, 169, 181, 193, 205, 217, 229, 241, 253,
};

// §E2.3.3.16: ecplbegf's piecewise decode. Asymmetric with ecplendf's own
// conversion below - this is the easiest of the two to get backwards.
[[nodiscard]] constexpr int ecpl_begin_subbnd(int ecplbegf) {
    if (ecplbegf < 3) {
        return ecplbegf * 2;
    }
    if (ecplbegf < 13) {
        return ecplbegf + 2;
    }
    return ecplbegf * 2 - 10;
}

// §E2.3.3.17, the branch taken when spectral extension is NOT active this
// block (ecplendf is transmitted).
[[nodiscard]] constexpr int ecpl_end_subbnd(int ecplendf) { return ecplendf + 7; }

// §E2.3.3.17's other branch: with spectral extension active, ecplendf is not
// transmitted and the enhanced coupling region instead ends exactly where
// synthesis begins, mirroring derived_cplendf's role for standard coupling.
[[nodiscard]] constexpr int ecpl_end_subbnd_from_spx(int spxbegf) {
    return spxbegf < 6 ? spxbegf + 5 : spxbegf * 2;
}

// Table E2.13, the default enhanced coupling band structure used when
// ecplbndstrce is 0 in the first block that turns enhanced coupling on.
// Indexed by the ABSOLUTE sub-band number, same convention as
// kDefaultSpxBandStructure above (§E2.3.3.18's note that entries at and
// below sub-band 8 are always false and never transmitted).
inline constexpr std::array<bool, kEcplSubBands> kDefaultEcplBandStructure = {
    false, false, false, false, false, false, false, false, false, true,  false, true,
    false, true,  false, true,  true,  true,  false, true,  true,  true,
};

// §E2.3.3.19: groups enhanced coupling sub-bands [begin_subbnd, end_subbnd)
// into bands using kEcplSubBandTab's actual (non-uniform) widths - the
// uniform-width group_bands() above cannot be reused here for that reason.
// `structure` is indexed absolutely, same convention as the default table.
[[nodiscard]] AC3FORGE_EXPORT BandLayout ecpl_group_bands(int begin_subbnd, int end_subbnd,
                                                          std::span<const bool> structure);

// Table E3.10: ecplamp (5 bits, 0..31) to a linear amplitude scaling value.
// Index 31 is the "-infinity dB" special case; 0..30 span 0 dB to -45.01 dB
// in ~1.5 dB steps and decode via (manttab / 32) >> exptab.
[[nodiscard]] AC3FORGE_EXPORT double decode_ecplamp(int ecplamp);
[[nodiscard]] AC3FORGE_EXPORT int quantize_ecplamp(double value);

// Table E3.11: ecplangle (6 bits, 0..63) to a linear angle in units of pi
// radians, range [-1, 1). The table is exactly i/32 for i < 32 and
// (i - 64)/32 above it, so this is a formula rather than a literal lookup.
[[nodiscard]] constexpr double decode_ecplangle(int ecplangle) {
    return static_cast<double>(ecplangle < 32 ? ecplangle : ecplangle - 64) / 32.0;
}
[[nodiscard]] AC3FORGE_EXPORT int quantize_ecplangle(double angle);

// Table E3.12: ecplchaos (3 bits, 0..7) to a linear scaling value in
// [-1, 0]. Exactly -i/7, so again a formula rather than a literal lookup.
[[nodiscard]] constexpr double decode_ecplchaos(int ecplchaos) {
    return -static_cast<double>(ecplchaos) / 7.0;
}
[[nodiscard]] AC3FORGE_EXPORT int quantize_ecplchaos(double chaos);

// §E3.5.5.1's non-aliased channel reconstruction: rebuilds the enhanced
// coupling channel's complex spectrum for one block from that block's own
// mantissas plus its neighbors'. Each of `prev_mant`/`curr_mant`/`next_mant`
// is a 256-bin MDCT coefficient array, zero outside the coupled range - the
// same shape the coupling-channel mantissa stream already decodes into.
// `prev_mant`/`next_mant` are all-zero when the adjacent block did not use
// enhanced coupling (§3.5.5.1's own rule), which this decoder also applies
// at a syncframe's first/last block, where the true neighbor lives in an
// adjacent frame this call was not given (see the decoder's own comment on
// that limitation). Only bins 0..255 of the resulting spectrum are ever read
// downstream (§3.5.5.4 only uses bin < N/2), so only those are written.
AC3FORGE_EXPORT void ecpl_channel_spectrum(std::span<const double, 256> prev_mant,
                                           std::span<const double, 256> curr_mant,
                                           std::span<const double, 256> next_mant,
                                           std::span<double, 256> real_out,
                                           std::span<double, 256> imag_out);

// §3.5.5.3's fixed de-correlation sequence for a channel/bin not carrying a
// transient: deterministic and stable for the whole stream (the spec's own
// requirement - "generated once ... stay the same for every block"),
// implemented as a hash of (channel, bin) rather than a stored table, so no
// per-decoder state is needed to satisfy it.
[[nodiscard]] AC3FORGE_EXPORT double ecpl_rand_notrans(int channel, int bin);

// §3.5.5.3's other sequence, for a channel/bin WITH a transient present
// (ecpltrans[ch]): regenerated every block, so - unlike the one above - this
// one is genuine sequential state, one instance per substream/frame.
struct AC3FORGE_EXPORT EcplNoise {
    std::uint32_t state = 0x2545F491U;  // never zero, or xorshift sticks at 0
    [[nodiscard]] double next();  // uniform on [-1, 1] (§3.5.5.3, not unit-variance)
};

// §3.5.5.2's amplitude decode + chaos modification, expanded from BANDS (as
// transmitted) all the way to individual BINS via `structure` and
// kEcplSubBandTab - mirroring §3.5.5.2's own necplbnds expansion loop in one
// pass rather than two. `ecplamp`/`ecplchaos` are indexed per BAND (0..band
// count - 1, as read off the wire); `structure` is indexed absolutely by
// sub-band, same convention as ecpl_group_bands. `ecpltrans`/
// `is_first_channel` gate the chaos modification per §3.5.5.2's own
// conditions. Writes one amplitude per BIN into `amp_out`, indexed from 0
// (not offset by the region's first bin) - callers slice storage
// themselves. `amp_out` must be sized to the bin count of
// [begin_subbnd, end_subbnd).
AC3FORGE_EXPORT void ecpl_amplitudes(std::span<const int> ecplamp, std::span<const int> ecplchaos,
                                     bool ecpltrans, bool is_first_channel, int begin_subbnd,
                                     int end_subbnd, std::span<const bool> structure,
                                     std::span<double> amp_out);

// §3.5.5.3's angle decode + de-correlation add, WITHOUT the optional linear
// interpolation (ecplangleintrp == 1): band angles apply directly to every
// bin of that band, the same duplication ecpl_amplitudes uses. The
// interpolated form is legal syntax this decoder does not implement (no
// stream this project's own encoder produces sets the flag - the decoder
// refuses streams that do, the same stance taken elsewhere in this file for
// syntax nothing here exercises). `noise` supplies rand_trans[] when
// `ecpltrans` is set; ecpl_rand_notrans is used internally otherwise.
// `angle_out` has the same shape/indexing as ecpl_amplitudes' `amp_out`.
AC3FORGE_EXPORT void ecpl_angles(int channel, std::span<const int> ecplangle,
                                 std::span<const int> ecplchaos, bool ecpltrans,
                                 bool is_first_channel, int begin_subbnd, int end_subbnd,
                                 std::span<const bool> structure, EcplNoise& noise,
                                 std::span<double> angle_out);

// §3.5.5.4: the final complex-product reconstruction, given this block's
// enhanced coupling channel spectrum (`real_in`/`imag_in`, bins 0..255 from
// ecpl_channel_spectrum) and one channel's already-resolved per-bin
// amplitude/angle (`amp_bin`/`angle_bin`, indexed from `begin_mant` - i.e.
// element 0 corresponds to bin `begin_mant`, matching ecpl_amplitudes/
// ecpl_angles' own output indexing). Writes into `mant_out` over
// [begin_mant, end_mant), absolutely indexed like the rest of this file's
// coefficient arrays.
AC3FORGE_EXPORT void ecpl_channel_coefficients(std::span<const double, 256> real_in,
                                               std::span<const double, 256> imag_in,
                                               std::span<const double> amp_bin,
                                               std::span<const double> angle_bin,
                                               int begin_mant, int end_mant,
                                               std::span<double, 256> mant_out);

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
AC3FORGE_EXPORT void aht_forward(std::span<const double, kBlocksPerFrameSize> blocks,
                                 std::span<double, kBlocksPerFrameSize> out);

// The decoder's direction, so the encoder can see what it will reconstruct.
AC3FORGE_EXPORT void aht_inverse(std::span<const double, kBlocksPerFrameSize> coefficients,
                                 std::span<double, kBlocksPerFrameSize> out);

// Table E3.2: mantissa bits per coefficient for the scalar hebap range 8-19.
// Outside it the answer is not a per-coefficient width at all - hebap 0 codes
// nothing and 1-7 code all six coefficients as one VQ index - so those return
// zero and the caller must handle them.
[[nodiscard]] constexpr int aht_mantissa_bits(int hebap) {
    constexpr std::array<int, 20> kBits = {0, 0, 0, 0, 0, 0,  0,  0,  3,  4,
                                           5, 6, 7, 8, 9, 10, 11, 12, 14, 16};
    return hebap >= 0 && hebap < 20 ? kBits[static_cast<std::size_t>(hebap)] : 0;
}

// Bits one bin costs for the WHOLE frame under AHT: one VQ index in the
// vector range, six scalar mantissas above it.
[[nodiscard]] AC3FORGE_EXPORT int aht_bin_bits(int hebap);

// Nearest codebook entry for a bin's six coefficients, by Euclidean distance
// (§E3.4.4.1). hebap must be in 1..7. Writes the reconstruction the decoder
// will use back into `values`.
[[nodiscard]] AC3FORGE_EXPORT int aht_vector_quantize(std::span<double, kBlocksPerFrameSize> values,
                                                      int hebap);

// --- gain-adaptive quantization (§E3.4.4.2) --------------------------------
// A variable-length layer over the scalar range. The encoder may amplify a
// bin's six mantissas by a gain Gk before coding them, which lets the small
// ones - the common case, since the DCT concentrates energy - be sent in
// fewer bits. The ones that no longer fit are flagged with a tag (the small
// quantizer's unused full-scale-negative symbol) and followed by a longer
// codeword. One gain per bin goes out as side information.
//
// Table E3.6's remapping constants are not transcribed here. They restate
// three uniform quantizers, and deriving those instead means the arithmetic
// below is checkable rather than trusted: tools/generators/gen_aht_tables.py reproduces
// all 120 of the standard's constants from it and fails if any disagrees.

// Table E3.3: which gains a mode permits. Mode 0 permits only unity, which is
// GAQ switched off.
[[nodiscard]] AC3FORGE_EXPORT std::span<const int> aht_gaq_gains(int gaqmod);

// §E3.4.2: at and above this hebap a bin carries no gain word and falls back
// to the unity-gain quantizer, whatever the mode.
[[nodiscard]] constexpr int aht_gaq_endbap(int gaqmod) {
    return gaqmod < 2 ? 12 : 17;
}

// Whether a bin carries a gain word. Note the standard's gaqbin is tri-state:
// this is the "1" case, and hebap >= endbap is the "-1" case, which differs
// only in that no gain is transmitted - both still code six mantissas.
[[nodiscard]] constexpr bool aht_gaq_has_gain(int hebap, int gaqmod) {
    return hebap > 7 && hebap < aht_gaq_endbap(gaqmod);
}

// One quantized mantissa. `escape_bits` is zero when the small quantizer
// sufficed; otherwise `code` is the tag and `escape` the longer codeword that
// immediately follows it.
struct AhtMantissaCode {
    std::uint32_t code = 0;
    std::uint32_t escape = 0;
    int bits = 0;
    int escape_bits = 0;
    double recon = 0.0;
};

[[nodiscard]] AC3FORGE_EXPORT AhtMantissaCode aht_quantize_mantissa(double value, int mantissa_bits,
                                                                    int gain);

// What one bin's six mantissas cost at a given gain, tags and escapes
// included. This is why an AHT frame's size cannot be known without
// quantizing it.
[[nodiscard]] AC3FORGE_EXPORT int aht_bin_gaq_bits(
    std::span<const double, kBlocksPerFrameSize> values, int mantissa_bits, int gain);

// The cheapest gain a mode allows for this bin. Distortion barely moves
// between gains - each is about 2^m - 1 reconstruction points either way, just
// spaced differently - so bits are the whole objective.
[[nodiscard]] AC3FORGE_EXPORT int aht_choose_gain(
    std::span<const double, kBlocksPerFrameSize> values, int mantissa_bits, int gaqmod);

// §E3.4.2: gain words transmitted for `active` gain-carrying bins. Modes 1
// and 2 send one bit each; mode 3 packs three bins' three-state gains into a
// 5-bit word, so a partial final triplet still costs a whole one.
[[nodiscard]] constexpr int aht_gaq_sections(int active, int gaqmod) {
    if (gaqmod == 0) {
        return 0;
    }
    return gaqmod == 3 ? (active + 2) / 3 : active;
}

[[nodiscard]] constexpr int aht_gaq_gain_bits(int gaqmod) {
    return gaqmod == 3 ? 5 : 1;
}

// Table E3.4: the three-state gain as it is packed, 1 -> 0, 2 -> 1, 4 -> 2.
[[nodiscard]] constexpr int aht_gaq_mapped(int gain) {
    return gain == 4 ? 2 : (gain == 2 ? 1 : 0);
}

// The decode direction of the table above: the packed value read off the
// wire back to the gain it names.
[[nodiscard]] constexpr int aht_gaq_gain_from_mapped(int mapped) {
    return mapped == 2 ? 4 : (mapped == 1 ? 2 : 1);
}

// The decode direction of aht_quantize_mantissa (§E3.4.4.2 / Table E3.5).
// `code` and `escape` are the RAW bit patterns as read off the wire (small-
// and large-codeword width respectively, both sign-extended internally as
// two's complement); `has_escape` says whether the caller found the tag
// (`code == 1 << (small_bits - 1)` as a raw pattern) and therefore read
// `escape` at all - for gain 1 there is never a tag or an escape, and
// `escape` is ignored. mantissa_bits is Table E3.2's per-hebap width; the
// small/large bit counts this derives internally are the exact ones
// aht_quantize_mantissa derives when producing them, so the two stay in
// lockstep by construction rather than by keeping two tables in sync.
[[nodiscard]] AC3FORGE_EXPORT double aht_dequantize_mantissa(std::uint32_t code,
                                                             std::uint32_t escape, bool has_escape,
                                                             int mantissa_bits, int gain);

}  // namespace ac3::eac3
