#include "ac3/encoder/eac3_tools.hpp"

#include <cassert>

namespace ac3::eac3 {

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
