#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/oba/joc_tables.hpp"

// Joint Object Coding - ETSI TS 103 420 clause 6. The tool that gets more
// objects out of a decoder than there are channels in the bitstream.
//
// JOC codes no audio of its own. It carries a matrix: for each output object,
// how much of each downmix channel to take, per QMF parameter band and
// interpolated across the frame. The decoder (§6.6.6) computes
//     object[obj] = sum over channels of downmix[ch] * mix[obj][ch]
// in the complex QMF domain, so the "matrix" is really a set of per-band
// gains, and the whole tool is 5 channels in, up to 16 objects out.
//
// Because the reconstruction is a linear combination of the downmix, objects
// that were mixed into the SAME downmix channels with the same gains cannot be
// pulled apart again. JOC is a parametric approximation, not a lossless
// separation, and its quality depends entirely on how well-separated the
// objects were in the downmix.

namespace ac3::joc {

// Table 47 / Table 48. Only the 5.X configurations are reachable here: 7.X
// needs Lb/Rb in the downmix, which costs a dependent substream.
inline constexpr int kDmxConfig5X = 0;
inline constexpr int kNumChannels5X = 5;

// §7.1: the complex QMF the reconstruction runs in is 64 subbands wide.
inline constexpr int kQmfSubbands = 64;

// §6.3.2.4: joc_num_objects_bits is 6 bits but capped at 15, so 16 objects.
inline constexpr int kMaxObjects = 16;

// The reconstruction matrix for one frame, in the dequantized units §6.6.4
// produces - a range of roughly [-9,6; 9,5], not a normalized gain.
//
// The layout is [object][channel][band], row-major, which is the order
// joc_data writes it in.
struct FrameParameters {
    int objects = 0;
    int channels = kNumChannels5X;
    // Index into kNumBands (Table 50), not the band count itself.
    int num_bands_idx = 4;  // 9 bands
    // §6.3.3.7. Coarse is 96 quantization steps over the range, fine is 192.
    // Fine halves the step at roughly one extra bit per coefficient.
    bool fine_quant = false;
    // §6.3.3.3: a splice detector, not a timestamp. It counts frames from 1 to
    // 1023 and wraps to 1; 0 means "first frame, or first after a splice", so
    // the decoder knows joc_mix_mtx_prev is meaningless and must not
    // interpolate from it.
    int seq_count = 0;
    std::vector<double> matrix;

    [[nodiscard]] int bands() const { return kNumBands[static_cast<std::size_t>(num_bands_idx)]; }
    [[nodiscard]] std::size_t coefficient_count() const {
        return static_cast<std::size_t>(objects) * static_cast<std::size_t>(channels) *
               static_cast<std::size_t>(bands());
    }
    [[nodiscard]] double& at(int object, int channel, int band) {
        return matrix[(static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) +
                       static_cast<std::size_t>(channel)) *
                          static_cast<std::size_t>(bands()) +
                      static_cast<std::size_t>(band)];
    }
    [[nodiscard]] double at(int object, int channel, int band) const {
        return matrix[(static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) +
                       static_cast<std::size_t>(channel)) *
                          static_cast<std::size_t>(bands()) +
                      static_cast<std::size_t>(band)];
    }
};

// §6.6.4's quantizer, and its inverse. The step is 820/(4096*(1+fine)) and the
// origin sits at nquant/2, so code nquant/2 is exactly zero gain.
[[nodiscard]] int quantize(double coefficient, bool fine_quant);
[[nodiscard]] double dequantize(int code, bool fine_quant);

// One joc() payload (§6.2.1), padded to whole bytes for emdf_payload_size.
[[nodiscard]] std::vector<std::byte> build_payload(const FrameParameters& params);

}  // namespace ac3::joc
