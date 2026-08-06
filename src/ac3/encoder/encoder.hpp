#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError, SkipPlan/plan_padding

// The real AC-3 encoder (Milestone 5): 2/0 stereo, long blocks, no coupling,
// D15 exponents in block 0 reused across the frame, static bit-allocation
// parameters (A/52 §8.2.12 basic-encoder defaults), global SNR-offset search
// to fill the frame. The pipeline per frame:
//
//   PCM -> windowed MDCT x6 -> 25-bit fixed coefficients -> raw exponents
//   (min across blocks, so reuse is always safe) -> encode + normative
//   decode (the decoder mirror) -> §7.2.2 bit allocation under a binary
//   search over (csnroffst, fsnroffst) -> mantissa quantization with §7.3.5
//   cross-channel grouping -> bitstream packing + skip-field padding + CRCs.

namespace ac3 {

struct EncoderConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    int dialnorm = 31;  // 1..31 (§5.4.2.8)
    int chbwcod = 60;   // channel bandwidth code, 0..60 (endmant = 3*(cbw+12)+37)
    bool pad441 = false;
};

// Stateful across frames: the MDCT's 50% overlap needs the previous frame's
// final 256 samples per channel (zeros at stream start = standard 256-sample
// encoder delay).
class StereoEncoder {
public:
    explicit StereoEncoder(const EncoderConfig& config) : config_(config) {}

    // Consumes exactly kSamplesPerFrame (1536) samples per channel, values
    // nominally in [-1, 1). Returns one complete syncframe.
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const float> left, std::span<const float> right);

    [[nodiscard]] const EncoderConfig& config() const { return config_; }

private:
    EncoderConfig config_;
    std::array<std::array<double, 256>, 2> history_{};
};

}  // namespace ac3
