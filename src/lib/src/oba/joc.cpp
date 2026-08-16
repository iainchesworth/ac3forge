#include "ac3/oba/joc.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/core/window.hpp"

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

// §6.6.3 Pseudocode 4, decoding by longest match rather than walking a tree:
// a prefix code is uniquely determined by its (code, length) pairs, so this
// is equivalent to the normative tree and was how tests/test_oba.cpp
// originally validated the generated encode tables (kMtxCoarse/kMtxFine were
// inverted FROM those trees, so agreeing with an independent forward walk of
// them, not with build_payload's own logic, is what that test proved). This
// promotes the same algorithm to production decode.
[[nodiscard]] int huff_decode(std::span<const HuffCode> table, BitReader& r) {
    std::uint32_t accumulated = 0;
    for (int bits = 1; bits <= 32; ++bits) {
        accumulated = (accumulated << 1) | r.read_bit();
        if (r.overflowed()) {
            return -1;
        }
        for (std::size_t value = 0; value < table.size(); ++value) {
            if (table[value].bits == bits && table[value].code == accumulated) {
                return static_cast<int>(value);
            }
        }
    }
    return -1;
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

std::optional<FrameParameters> parse_payload(std::span<const std::byte> payload) {
    BitReader r{payload};

    // --- joc_header (§6.2.2) ---
    if (r.read(3) != kDmxConfig5X) {  // joc_dmx_config_idx: only 5.X is reachable here
        return std::nullopt;
    }
    const int objects = static_cast<int>(r.read(6)) + 1;  // joc_num_objects_bits
    if (objects > kMaxObjects) {
        return std::nullopt;
    }
    if (r.read(3) != 0) {  // joc_ext_config_idx: no extensional configuration data
        return std::nullopt;
    }

    // --- joc_info (§6.2.3) ---
    // §6.3.3.2's equation is ambiguous in the published PDF for anything
    // other than zero/zero - see build_payload's own comment - so a nonzero
    // clip gain here is refused rather than computed from a formula this
    // codebase cannot verify.
    if (r.read(3) != 0) {  // joc_clipgain_x_bits
        return std::nullopt;
    }
    if (r.read(5) != 0) {  // joc_clipgain_y_bits
        return std::nullopt;
    }
    const int seq_count = static_cast<int>(r.read(10));

    // FrameParameters has one num_bands_idx/fine_quant for the WHOLE matrix,
    // matching how AtmosEncoder only ever writes one shared value for every
    // object - so every object's own copy of these fields is read and
    // checked to agree with the first, rather than allowed to vary per
    // object the way the wire format in principle permits.
    int num_bands_idx = 0;
    bool fine_quant = false;
    for (int object = 0; object < objects; ++object) {
        if (r.read(1) != 1) {  // b_joc_obj_present: every object is coded every frame here
            return std::nullopt;
        }
        const auto bands_idx = static_cast<int>(r.read(3));
        if (r.read(1) != 0) {  // b_joc_sparse: whole-matrix mode only
            return std::nullopt;
        }
        const bool quant = r.read(1) != 0;  // joc_num_quant_idx
        if (object == 0) {
            num_bands_idx = bands_idx;
            fine_quant = quant;
        } else if (bands_idx != num_bands_idx || quant != fine_quant) {
            return std::nullopt;
        }
        // --- joc_data_point_info (§6.2.4) ---
        if (r.read(1) != 0) {  // joc_slope_idx: smooth only
            return std::nullopt;
        }
        if (r.read(1) != 0) {  // joc_num_dpoints_bits: one data point only
            return std::nullopt;
        }
    }

    FrameParameters params;
    params.objects = objects;
    params.channels = kNumChannels5X;
    params.num_bands_idx = num_bands_idx;
    params.fine_quant = fine_quant;
    params.seq_count = seq_count;
    params.matrix.assign(params.coefficient_count(), 0.0);

    // --- joc_data (§6.2.5) ---
    const int steps = quant_steps(fine_quant);
    const int bands = params.bands();
    const std::span<const HuffCode> table =
        fine_quant ? std::span<const HuffCode>{kMtxFine} : std::span<const HuffCode>{kMtxCoarse};
    for (int object = 0; object < objects; ++object) {
        for (int channel = 0; channel < params.channels; ++channel) {
            int previous = steps / 2;
            for (int band = 0; band < bands; ++band) {
                const int difference = huff_decode(table, r);
                if (difference < 0) {
                    return std::nullopt;
                }
                const int code = (previous + difference) % steps;
                previous = code;
                params.at(object, channel, band) = dequantize(code, fine_quant);
            }
        }
    }

    // At most a byte of padding_bits should remain (§6.2.1), same bound
    // tests/test_oba.cpp's own encode-side test holds build_payload to - a
    // corrupt object/band count that made this decode stop short leaves
    // more than that unaccounted for.
    if (r.overflowed() || payload.size() * 8 - r.bit_position() >= 8) {
        return std::nullopt;
    }
    return params;
}

std::vector<std::vector<float>> reconstruct(std::span<const std::vector<float>> bed,
                                            const FrameParameters& params,
                                            ReconstructionState& state, bool fast_mdct) {
    assert(bed.size() == static_cast<std::size_t>(kNumChannels5X));
    assert(params.channels == kNumChannels5X);
    assert(params.matrix.size() == params.coefficient_count());

    const int objects = params.objects;
    const int bands = params.bands();
    const auto& mapping = kSubbandToBand[static_cast<std::size_t>(params.num_bands_idx)];

    // §6.3.3.3: no ramp on the first frame or right after a splice, and
    // equally none if the previous frame's matrix does not even have the
    // same shape to ramp from (an object/band count change this project's
    // own AtmosEncoder never makes mid-stream, but a general JOC stream
    // could in principle) - both collapse to "this frame's matrix applies
    // to the whole frame outright", the same as ReconstructionState's own
    // default-constructed (never-reconstructed-before) state.
    const bool has_ramp = params.seq_count != 0 &&
                          state.previous_matrix.size() == params.matrix.size() &&
                          state.previous_objects == objects &&
                          state.previous_num_bands_idx == params.num_bands_idx;

    if (static_cast<int>(state.object_history.size()) != objects) {
        // A changed object count invalidates any old per-object history
        // anyway (index i no longer names the same object), so this also
        // covers the very first call, where object_history starts empty.
        state.object_history.assign(static_cast<std::size_t>(objects), {});
    }

    std::vector<std::vector<float>> out(
        static_cast<std::size_t>(objects),
        std::vector<float>(static_cast<std::size_t>(kSamplesPerFrame)));

    auto& bed_mdct = state.bed_mdct_scratch;
    auto& time = state.time_scratch;
    auto& windowed = state.windowed_scratch;
    auto& object_mdct = state.object_mdct_scratch;
    auto& x = state.synth_scratch;

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        // --- analyze this block of the downmix, one MDCT per bed channel ---
        // Only block 0 ever reads negative indices (into the previous
        // frame's tail); every later block's window sits entirely inside
        // THIS frame's own already-decoded samples.
        for (int ch = 0; ch < kNumChannels5X; ++ch) {
            for (int n = 0; n < 512; ++n) {
                const int index = block * kSamplesPerBlock + n - 256;
                time[static_cast<std::size_t>(n)] =
                    index >= 0
                        ? static_cast<double>(
                              bed[static_cast<std::size_t>(ch)][static_cast<std::size_t>(index)])
                        : state.bed_history[static_cast<std::size_t>(ch)]
                                           [static_cast<std::size_t>(256 + index)];
            }
            apply_analysis_window(time, windowed);
            mdct512_forward(windowed, bed_mdct[static_cast<std::size_t>(ch)], fast_mdct);
        }

        // §6.6.5's ramp, evaluated at this block's own right edge - the same
        // (n + 1) / kSamplesPerFrame convention atmos.cpp's own bed ramp
        // uses, at block granularity instead of per sample.
        const double frac =
            static_cast<double>(block + 1) / static_cast<double>(kBlocksPerFrame);

        for (int object = 0; object < objects; ++object) {
            // --- §6.6.6: this object's spectrum is a per-band linear
            // combination of the downmix's ---
            for (int bin = 0; bin < 256; ++bin) {
                const int band = mapping[static_cast<std::size_t>(bin / 4)];
                double sum = 0.0;
                for (int ch = 0; ch < kNumChannels5X; ++ch) {
                    double m = params.at(object, ch, band);
                    if (has_ramp) {
                        const auto previous_index =
                            (static_cast<std::size_t>(object) *
                                 static_cast<std::size_t>(kNumChannels5X) +
                             static_cast<std::size_t>(ch)) *
                                static_cast<std::size_t>(bands) +
                            static_cast<std::size_t>(band);
                        const double previous = state.previous_matrix[previous_index];
                        m = previous + frac * (m - previous);
                    }
                    sum += m *
                          bed_mdct[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bin)];
                }
                object_mdct[static_cast<std::size_t>(bin)] = sum;
            }

            // --- synthesize, same overlap-add eac3_decoder.cpp's own
            // channel reconstruction uses ---
            imdct512_windowed(object_mdct, x);
            auto& history = state.object_history[static_cast<std::size_t>(object)];
            auto& pcm = out[static_cast<std::size_t>(object)];
            for (int n = 0; n < kSamplesPerBlock; ++n) {
                pcm[static_cast<std::size_t>(block * kSamplesPerBlock + n)] = static_cast<float>(
                    2.0 * (x[static_cast<std::size_t>(n)] + history[static_cast<std::size_t>(n)]));
                history[static_cast<std::size_t>(n)] = x[static_cast<std::size_t>(256 + n)];
            }
        }
    }

    for (int ch = 0; ch < kNumChannels5X; ++ch) {
        for (int n = 0; n < 256; ++n) {
            state.bed_history[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] =
                static_cast<double>(
                    bed[static_cast<std::size_t>(ch)]
                       [static_cast<std::size_t>(kSamplesPerFrame - 256 + n)]);
        }
    }
    state.previous_matrix = params.matrix;
    state.previous_objects = objects;
    state.previous_num_bands_idx = params.num_bands_idx;

    return out;
}

}  // namespace ac3::joc
