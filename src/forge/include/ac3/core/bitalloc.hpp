#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// The A/52 §7.2.2 parametric bit allocation — the decoder-defined heart of
// AC-3. The decoder recomputes this routine from transmitted parameters
// using exact integer arithmetic, so this implementation must be bit-exact;
// it is validated against an independent Python transcription of the spec
// pseudocode (tools/references/bitalloc_ref.py) with zero tolerance.
//
// Scope: fbw, LFE and coupling channels — the coupling channel enters higher
// up the spectrum and seeds its leak state from cplfleak/cplsleak instead of
// running lowcomp (§7.2.2.4), which BitAllocRegion below selects. Delta bit
// allocation (§7.2.2.6) lets an encoder nudge the masking curve this routine
// derives from exponents alone, in either direction, per band.

namespace ac3 {

// Bit-allocation parameter codes as transmitted in the BSI/audblk. Defaults
// are the spec's basic-encoder values (§8.2.12).
struct BitAllocCodes {
    int sdcycod = 2;
    int fdcycod = 1;
    int sgaincod = 1;
    int dbpbcod = 2;
    int floorcod = 4;
    int fgaincod = 4;
};

// Tables 7.11 and 7.8. Exposed because an encoder picking the coupling
// channel's leak seeds needs the same gains the allocator will apply.
[[nodiscard]] AC3FORGE_EXPORT int fast_gain(int fgaincod);
[[nodiscard]] AC3FORGE_EXPORT int slow_gain(int sgaincod);

// Table 7.13: the 50-band mask()/bndpsd() index a bitstream bin belongs to.
// Exposed because a caller validating delta bit allocation segments before
// compute_bit_allocation() ever sees them (deltoffst/deltlen are
// attacker-controlled bitstream fields) needs the same channel-start band
// this routine derives internally as bndstrt, rather than a second copy of
// Table 7.13 guessing at the same value.
[[nodiscard]] AC3FORGE_EXPORT int bin_to_band(int bin);

// §7.2.2.1: the composite SNR offset.
[[nodiscard]] constexpr int snr_offset(int csnroffst, int fsnroffst) {
    return (((csnroffst - 15) << 4) + fsnroffst) << 2;
}

// §7.2.2.6: a resolved set of delta bit allocation segments for ONE channel
// (or the coupling channel) — the spec's cpldelt*/delt*[ch] pair collapses to
// this one shape because a compute_bit_allocation() call already represents
// exactly one such channel. deltnseg == 0 means no segments: the spec's own
// recommended reset state ("perform no delta alloc" / absent).
struct DeltaSegments {
    int deltnseg = 0;                         // 1..8 segments when > 0
    std::array<std::uint8_t, 8> deltoffst{};  // 5-bit band offsets (Table 5.3/E1.3)
    std::array<std::uint8_t, 8> deltlen{};    // 4-bit band lengths
    std::array<std::uint8_t, 8> deltba{};     // 3-bit adjustment codes (Table 5.17)
};

// Where the allocation starts, and - for the coupling channel - the leak
// state the spec seeds instead of running the low-frequency lowcomp path.
struct BitAllocRegion {
    int start = 0;          // strtmant: 0 for fbw and LFE, cplstrtmant for coupling
    bool coupling = false;  // §7.2.2.4 takes the "else" branch: no lowcomp
    int cplfleak = 0;       // 3-bit cplfleak, only when coupling
    int cplsleak = 0;       // 3-bit cplsleak, only when coupling
    // §7.2.2.1.1: the all-zero-SNR mute is a FRAME-WIDE condition - csnroffst
    // together with every fsnroffst, cplfsnroffst and lfefsnroffst. It cannot
    // be decided from one channel's offsets, so the caller evaluates it and
    // passes the answer; getting this wrong zeroes one channel's allocation
    // while the others allocate normally, which desynchronises the shared
    // mantissa stream.
    bool snr_all_zero = false;
    // §E3.4.3.1: when the adaptive hybrid transform is in use for this
    // channel, the final table lookup goes through hebaptab instead of
    // baptab. Everything up to that point - psd, banding, excitation,
    // masking, the snroffset/floor/truncation dance - is identical, so this
    // is one table swap rather than a second allocator. The outcome is a
    // pointer in 0..19 rather than 0..15, and it means something different:
    // 1-7 select vector quantisers, 8-19 scalar ones.
    bool high_efficiency = false;
    // §7.2.2.6: this call's delta segments (see DeltaSegments above) — the
    // caller picks whichever of cpldelt*/delt*[ch] belongs to this channel.
    DeltaSegments delta{};
};

// §7.2.2.2-7.2.2.7 for one channel. exps are the DECODED exponents (the
// decoder mirror — never the raw ones); exps and bap are indexed from bin 0
// even when the region starts higher, so both span [0, endmant).
// csnroffst == 0 && fsnroffst == 0 triggers the §7.2.2.1.1 special case
// (all-zero bap).
AC3FORGE_EXPORT void compute_bit_allocation(std::span<const std::uint8_t> exps,
                                            SampleRate sample_rate, const BitAllocCodes& codes,
                                            int csnroffst, int fsnroffst,
                                            std::span<std::uint8_t> bap,
                                            const BitAllocRegion& region = {});

// §7.2.2.6, encoder side. compute_bit_allocation()'s masking curve is built
// only from the quantized exponent (psd[bin] = 3072 - exps[bin]<<7 — exactly
// 128 units, one Table 5.17 delta step, per exponent step), which discards
// where the real coefficient sits within that exponent's range. This compares
// that flat curve against one built from the real, pre-quantization
// coefficient magnitudes the encoder still has at this point, and returns the
// delta segments that correct the gap: merged into contiguous runs, quantized
// to the nearest Table 5.17 code, capped at the spec's 8-segment limit
// (largest-magnitude runs kept if more would qualify).
//
// `coefficients` and `exps` are the same channel, same length, both indexed
// from bin 0 exactly like compute_bit_allocation's own `exps`/`bap`; `start`
// is that call's BitAllocRegion::start (0 for fbw/LFE, cplstrtmant for
// coupling) — the segments this returns are meant to populate that same
// region's `delta` field. The returned deltoffst[0] is relative to the
// channel's own start band (bin_to_band(start)), matching how
// compute_bit_allocation() applies it back — see that function's own note.
[[nodiscard]] AC3FORGE_EXPORT DeltaSegments choose_delta_segments(
    std::span<const double> coefficients, std::span<const std::uint8_t> exps, int start);

}  // namespace ac3
