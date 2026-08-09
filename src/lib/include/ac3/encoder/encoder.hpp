#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError, SkipPlan/plan_padding
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"

// The AC-3 encoder: any audio coding mode (mono through 3/2) plus optional
// LFE, long blocks, optional channel coupling, 2/0 rematrixing, adaptive
// D15/D25/D45 exponents re-sent mid-frame when a channel's exponents drift
// (§8.2.8; the coupling channel is always D15, the LFE one D15 set per
// frame), static bit-allocation parameters (A/52 §8.2.12 basic-encoder
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
    // §5.4.2.16: Ch2's dialnorm, required when acmod is kDualMono (1+1) and
    // meaningless otherwise — the two programmes are levelled independently.
    std::optional<int> dialnorm2 = std::nullopt;
    int chbwcod = -1;        // fbw bandwidth code 0..60; -1 = auto from bitrate
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // Channel coupling (§7.4): above the coupling frequency the fbw channels
    // share one channel plus per-band coordinates. Needs >= 2 fbw channels;
    // the win shows up at low bit rates, where the saved coefficients buy
    // precision everywhere else. cplbegf/cplendf are sub-band indices; -1
    // asks the encoder to choose, which it does from the per-channel rate
    // (start) and from the bandwidth it would have coded anyway (end).
    bool coupling = false;
    int cplbegf = -1;
    int cplendf = -1;

    // --- dynamic range and downmix metadata (§7.7, §7.8) -------------------
    // Dynamic range control. std::nullopt leaves dynrnge clear in every block,
    // which is what §7.7.1.2 says an encoder applying no compression does, and
    // keeps a DRC-free stream bit-identical to one from before this existed.
    std::optional<meta::Profile> drc = std::nullopt;
    // Heavy compression, independent of drc: the two answer different
    // questions (§7.7.2.1), so a stream may carry either, both or neither.
    std::optional<meta::HeavyConfig> heavy = std::nullopt;
    // Table 5.9 / Table 5.10. Transmitted only when the layout has the
    // channels they describe, but they always define the §7.8 downmix, so the
    // heavy-compression peak detector consults them whatever acmod is.
    meta::CentreMixLevel cmixlev = meta::CentreMixLevel::kMinus4_5dB;
    meta::SurroundMixLevel surmixlev = meta::SurroundMixLevel::kMinus6dB;
};

class FrameEncoder {
public:
    explicit FrameEncoder(const EncoderConfig& config);

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
    // Both controllers smooth their gain over time, so they have to outlive a
    // frame - a per-frame instance would restart the attack every 32 ms.
    std::optional<meta::RangeController> range_;
    std::optional<meta::HeavyCompressor> heavy_;
    // Ch2's own controllers, present only when acmod is kDualMono. A shared
    // instance would smooth one programme's gain history into the other's.
    std::optional<meta::RangeController> range2_;
    std::optional<meta::HeavyCompressor> heavy2_;
};

}  // namespace ac3
