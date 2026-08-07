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

// §7.2.2.1: the composite SNR offset.
[[nodiscard]] constexpr int snr_offset(int csnroffst, int fsnroffst) {
    return (((csnroffst - 15) << 4) + fsnroffst) << 2;
}

// §7.2.2.2-7.2.2.7 for one fbw channel. exps are the DECODED exponents (the
// decoder mirror — never the raw ones); bap.size() == exps.size() == endmant.
// csnroffst == 0 && fsnroffst == 0 triggers the §7.2.2.1.1 special case
// (all-zero bap).
void compute_bit_allocation(std::span<const std::uint8_t> exps, SampleRate sample_rate,
                            const BitAllocCodes& codes, int csnroffst, int fsnroffst,
                            std::span<std::uint8_t> bap);

}  // namespace ac3
