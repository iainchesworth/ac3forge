#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError, SkipPlan/plan_padding

// The AC-3 encoder: any audio coding mode (mono through 3/2) plus optional
// LFE, long blocks, no coupling, D15 exponents in block 0 reused across the
// frame, static bit-allocation parameters (A/52 §8.2.12 basic-encoder
// defaults), global SNR-offset search to fill the frame.
//
// CBR at 44.1 kHz needs non-integral frame sizes: a Bresenham accumulator
// alternates between the two Table 5.18 lengths (even/odd frmsizecod) so the
// long-run rate is exact. At 32/48 kHz the same accumulator degenerates to
// the constant frame size.

namespace ac3 {

struct EncoderConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    int dialnorm = 31;       // 1..31 (§5.4.2.8)
    int chbwcod = -1;        // fbw bandwidth code 0..60; -1 = auto from bitrate
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // Channel coupling (§7.4): above the coupling frequency the fbw channels
    // share one channel plus per-band coordinates. Needs >= 2 fbw channels;
    // the win shows up at low bit rates, where the saved coefficients buy
    // precision everywhere else. cplbegf/cplendf are sub-band indices (-1
    // picks the spec's basic-encoder defaults of 6 and 12).
    bool coupling = false;
    int cplbegf = -1;
    int cplendf = -1;
};

class FrameEncoder {
public:
    explicit FrameEncoder(const EncoderConfig& config) : config_(config) {}

    // channels: the full-bandwidth channels in AC-3 order (Table 5.8: e.g.
    // 3/2 = L, C, R, SL, SR), followed by the LFE channel last when
    // config.lfe is set. Each span holds exactly kSamplesPerFrame samples,
    // nominally in [-1, 1). Returns one complete syncframe.
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels);

    [[nodiscard]] const EncoderConfig& config() const { return config_; }
    [[nodiscard]] int channel_count() const {
        return fullbw_channel_count(config_.acmod) + (config_.lfe ? 1 : 0);
    }

private:
    EncoderConfig config_;
    std::array<std::array<double, 256>, 6> history_{};  // MDCT overlap per channel
    std::uint64_t rate_accumulator_ = 0;                // ideal-bits Bresenham state
    std::uint64_t words_emitted_ = 0;
};

}  // namespace ac3
