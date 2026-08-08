#include "ac3/oba/joc.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ac3/core/bitwriter.hpp"

namespace ac3::joc {

namespace {

// §6.6.4: joc_mix_mtx_dq = (q - nquant/2) * 820 / (4096 * (1 + quant_idx)).
[[nodiscard]] constexpr int quant_steps(bool fine) { return fine ? 192 : 96; }
[[nodiscard]] constexpr double quant_scale(bool fine) {
    return 820.0 / (4096.0 * (fine ? 2.0 : 1.0));
}

void put_code(BitWriter& w, const HuffCode& code) {
    w.put(code.code, code.bits);
}

}  // namespace

int quantize(double coefficient, bool fine_quant) {
    const int steps = quant_steps(fine_quant);
    const long code = std::lround(coefficient / quant_scale(fine_quant)) + steps / 2;
    return static_cast<int>(std::clamp(code, 0L, static_cast<long>(steps) - 1));
}

double dequantize(int code, bool fine_quant) {
    // Both step counts are even, so the origin is exact and the subtraction
    // stays in integers until the scale is applied.
    const int origin = quant_steps(fine_quant) / 2;
    return static_cast<double>(code - origin) * quant_scale(fine_quant);
}

std::vector<std::byte> build_payload(const FrameParameters& params) {
    assert(params.objects >= 1 && params.objects <= kMaxObjects);
    assert(params.channels == kNumChannels5X);
    assert(params.matrix.size() == params.coefficient_count());

    const int bands = params.bands();
    const int steps = quant_steps(params.fine_quant);
    // The two tables differ in length, so they only meet as a span.
    const std::span<const HuffCode> table =
        params.fine_quant ? std::span<const HuffCode>{kMtxFine}
                          : std::span<const HuffCode>{kMtxCoarse};

    BitWriter w;

    // --- joc_header (§6.2.2) ---
    w.put(kDmxConfig5X, 3);
    w.put(static_cast<std::uint32_t>(params.objects - 1), 6);  // joc_num_objects_bits
    w.put(0, 3);  // joc_ext_config_idx: no extensional configuration data

    // --- joc_info (§6.2.3) ---
    // §6.3.3.2's equation renders ambiguously in the published PDF - the
    // fraction, the power of two and the bracketing all collide - but every
    // reading of it agrees that zero for both fields is joc_clipgain = 1. This
    // encoder applies no clip protection, so unity is the honest value and the
    // ambiguity does not bite.
    w.put(0, 3);  // joc_clipgain_x_bits
    w.put(0, 5);  // joc_clipgain_y_bits
    w.put(static_cast<std::uint32_t>(params.seq_count), 10);

    for (int object = 0; object < params.objects; ++object) {
        w.put(1, 1);  // b_joc_obj_present: every object is coded every frame
        w.put(static_cast<std::uint32_t>(params.num_bands_idx), 3);
        // Sparse mode codes one channel per band and gives every other channel
        // a fixed value - and that value is joc_num_quant/2 + 2 (§6.6.2), not
        // the quantizer's zero, so the channels it does not name still leak
        // about 0,4 into the object. The whole-matrix mode says what it means
        // for every channel, which for a downmix this encoder built itself is
        // both cheap enough and exactly right.
        w.put(0, 1);  // b_joc_sparse
        w.put(params.fine_quant ? 1u : 0u, 1);  // joc_num_quant_idx

        // --- joc_data_point_info (§6.2.4) ---
        // Smooth interpolation with a single data point: §6.6.5 then ramps the
        // matrix linearly from the previous frame's values across all the
        // frame's QMF timeslots. A steep slope would step at the frame edge,
        // which is audible on a moving object; two data points would let the
        // ramp bend mid-frame, which one OAMD update per frame cannot use.
        w.put(0, 1);  // joc_slope_idx: smooth
        w.put(0, 1);  // joc_num_dpoints_bits => one data point
    }

    // --- joc_data (§6.2.5) ---
    // §6.6.2 Pseudocode 3 runs the differential the other way: the decoder
    // accumulates modulo nquant along the bands, starting from nquant/2 - the
    // quantizer's zero - so the first band's codeword is the coefficient
    // itself and every later one is a step. Working modulo nquant means the
    // difference always fits the alphabet, however far apart two bands are.
    for (int object = 0; object < params.objects; ++object) {
        for (int channel = 0; channel < params.channels; ++channel) {
            int previous = steps / 2;
            for (int band = 0; band < bands; ++band) {
                const int code = quantize(params.at(object, channel, band),
                                          params.fine_quant);
                const int difference = ((code - previous) % steps + steps) % steps;
                put_code(w, table[static_cast<std::size_t>(difference)]);
                previous = code;
            }
        }
    }

    return w.take();  // padding_bits (§6.2.1) to the byte boundary
}

}  // namespace ac3::joc
