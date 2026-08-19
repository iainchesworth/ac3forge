#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/mlp/block.hpp"

// Encoder-side selection: choosing the predictor and matrix a block is
// encoded with. The bitstream carries whatever the encoder chose, so none
// of this affects decodability - it is pure compression quality, and both
// strategies here are the ones MLP's own designers describe:
//
//  - Predictors: the WO's suggestion that practical encoders try "a small
//    number of preselected prediction filters ... choosing whichever is
//    best for a particular block". The palette is the WO Table 1 presets
//    plus the classic integer difference filters (1 - z^-1)^n from the
//    JAES 1996 paper's own opening treatment, ranked by the exact payload
//    cost the block codec will pay (choose_coding_cost), plus each
//    candidate's own header overhead.
//
//  - Matrix: greedy pairwise subtraction. US 7,193,538's own account of
//    shipping MLP: the WO's eigenvector computation "is time consuming,
//    and the procedure ... wherein the zero correlation is achieved simply
//    by subtraction leads to a data rate that theoretically differs little
//    from that resulting from an eigenvector computation." Each round fits
//    a least-squares coefficient between the best channel pair and keeps
//    the step only if it actually reduces the measured cost.

namespace ac3::mlp::select {

// Best-of-palette predictor for one channel's significant-word signal.
[[nodiscard]] AC3FORGE_EXPORT PredictorCoefficients choose_predictor(
    std::span<const std::int32_t> significant);

// Greedy decorrelation over the (already stripped) significant-word
// channels: up to `max_steps` primitive subtraction steps, each kept only
// if it reduces the summed per-channel best-predictor cost.
[[nodiscard]] AC3FORGE_EXPORT std::vector<matrix::Step> choose_matrix(
    std::span<const std::span<const std::int32_t>> significant, int max_steps = 8);

// The whole per-block decision from raw samples: reproduces the block
// codec's own B1 strip, runs the matrix search on the stripped domain, and
// picks a predictor per post-matrix channel.
[[nodiscard]] AC3FORGE_EXPORT MultichannelBlockConfig choose_block_config(
    std::span<const std::span<const std::int32_t>> channels, int wordlength);

}  // namespace ac3::mlp::select
