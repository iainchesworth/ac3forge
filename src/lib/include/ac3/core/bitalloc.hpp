#pragma once

#include <cstdint>
#include <span>

#include "ac3/core/tables.hpp"

// The A/52 §7.2.2 parametric bit allocation — the decoder-defined heart of
// AC-3. The decoder recomputes this routine from transmitted parameters
// using exact integer arithmetic, so this implementation must be bit-exact;
// it is validated against an independent Python transcription of the spec
// pseudocode (tools/bitalloc_ref.py) with zero tolerance.
//
// Scope: the fbw-channel path (start = 0, no coupling, no LFE, no delta bit
// allocation) — what the current encoder emits. Coupling/LFE paths extend
// this same engine later.

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
[[nodiscard]] int fast_gain(int fgaincod);
[[nodiscard]] int slow_gain(int sgaincod);

// §7.2.2.1: the composite SNR offset.
[[nodiscard]] constexpr int snr_offset(int csnroffst, int fsnroffst) {
    return (((csnroffst - 15) << 4) + fsnroffst) << 2;
}

// Where the allocation starts, and - for the coupling channel - the leak
// state the spec seeds instead of running the low-frequency lowcomp path.
struct BitAllocRegion {
    int start = 0;             // strtmant: 0 for fbw and LFE, cplstrtmant for coupling
    bool coupling = false;     // §7.2.2.4 takes the "else" branch: no lowcomp
    int cplfleak = 0;          // 3-bit cplfleak, only when coupling
    int cplsleak = 0;          // 3-bit cplsleak, only when coupling
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
};

// §7.2.2.2-7.2.2.7 for one channel. exps are the DECODED exponents (the
// decoder mirror — never the raw ones); exps and bap are indexed from bin 0
// even when the region starts higher, so both span [0, endmant).
// csnroffst == 0 && fsnroffst == 0 triggers the §7.2.2.1.1 special case
// (all-zero bap).
void compute_bit_allocation(std::span<const std::uint8_t> exps, SampleRate sample_rate,
                            const BitAllocCodes& codes, int csnroffst, int fsnroffst,
                            std::span<std::uint8_t> bap, const BitAllocRegion& region = {});

}  // namespace ac3
