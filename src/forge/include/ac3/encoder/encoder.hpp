#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/mantissas.hpp"  // MantissaToken, for the token scratch below
#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError, SkipPlan/plan_padding
#include "ac3/encoder/transient.hpp"
#include "ac3/export.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/verify/mirror.hpp"

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
    int dialnorm = 31;  // 1..31 (§5.4.2.8)
    // §5.4.2.16: Ch2's dialnorm, required when acmod is kDualMono (1+1) and
    // meaningless otherwise — the two programmes are levelled independently.
    std::optional<int> dialnorm2 = std::nullopt;
    int chbwcod = -1;  // fbw bandwidth code 0..60; -1 = auto from bitrate
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
    // §7.9.4 fast N/4-FFT forward MDCT (see mdct.hpp's mdct512_forward), on
    // by default since the owner accepted its quality evidence (verified max
    // relative error ~3e-12 against the direct form on random data and real
    // audio, 331 dB direct-vs-fast end-to-end SNR, 0.000 dB delta against an
    // independent oracle at 192-448 kbps; see tests/test_mdct_fast.cpp and
    // `tools/ci/quality_race.py fast-mdct`). false forces the direct §8.2.3.2
    // reference form, which stays maintained as the oracle the fast path is
    // validated against. Only the long transform accelerates today - a
    // block-switched channel's short transforms always take the direct path
    // regardless of this flag.
    bool fast_mdct = true;

    // --- dynamic range and downmix metadata (§7.7, §7.8) -------------------
    // Dynamic range control. std::nullopt leaves dynrnge clear in every block,
    // which is what §7.7.1.2 says an encoder applying no compression does, and
    // keeps a DRC-free stream bit-identical to one from before this existed.
    std::optional<meta::Profile> drc = std::nullopt;
    // Heavy compression, independent of drc: the two answer different
    // questions (§7.7.2.1), so a stream may carry either, both or neither.
    std::optional<meta::HeavyConfig> heavy = std::nullopt;
    // Ch2's own drc/heavy, meaningful only under kDualMono. No fallback to
    // drc/heavy when unset - see plan::Metadata::drc2 for why: dialnorm2 is
    // already independent of dialnorm the same way.
    std::optional<meta::Profile> drc2 = std::nullopt;
    std::optional<meta::HeavyConfig> heavy2 = std::nullopt;
    // Table 5.9 / Table 5.10. Transmitted only when the layout has the
    // channels they describe, but they always define the §7.8 downmix, so the
    // heavy-compression peak detector consults them whatever acmod is.
    meta::CentreMixLevel cmixlev = meta::CentreMixLevel::kMinus4_5dB;
    meta::SurroundMixLevel surmixlev = meta::SurroundMixLevel::kMinus6dB;

    // --- self-check (ac3/verify/mirror.hpp) --------------------------------
    // When set, encode_frame() records its own model of the decoder - the bit
    // offset at each block boundary, and each stream's decoded exponents, bit
    // allocation and delta correction - into this trace, for comparison
    // against a real decode of the same frame. Null by default, and null
    // costs one branch per block and no allocation: the encoder's behaviour
    // and output are identical either way, this only reads state it already
    // has. A runtime pointer rather than a build-time switch because that is
    // how every other optional behaviour in this struct is expressed, and
    // because a check that needs a special build is a check nobody runs.
    // MirrorEncoder (ac3/verify/selfcheck.hpp) drives the whole comparison;
    // this is for a caller wanting to place the decode themselves.
    verify::FrameTrace* trace = nullptr;
};

class AC3FORGE_EXPORT FrameEncoder {
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
    // One per full-bandwidth channel (§8.2.2 excludes the LFE): stateful
    // across frames, like history_ above.
    std::vector<TransientDetector> transient_detectors_;
    // Per-(channel, block) scratch for the MDCT pass, reused rather than
    // stack-declared inside encode_frame (PREfast's C6262 flagged the
    // function's stack frame). Each is always fully overwritten before being
    // read within one iteration, and the two loops that use them run to
    // completion one after another - never interleaved or reentered - so
    // reuse across iterations, and across calls on this instance, changes
    // nothing observable. Not thread-safe for concurrent calls on the same
    // instance, same as history_ and the other per-frame state above.
    std::array<double, 512> time_scratch_{};
    std::array<double, 512> windowed_scratch_{};
    std::array<double, 128> half1_scratch_{};
    std::array<double, 128> half2_scratch_{};
    // Frame-lifetime work buffers, reused across encode_frame calls under
    // the same reasoning (and the same single-instance contract) as the
    // scratch arrays above: each is re-sized via assign()/resize() and fully
    // re-written every frame before anything reads it, so reuse changes
    // nothing observable - it only stops encode_frame from re-allocating
    // them 31 times a second. coeffs_ is the per-(stream, block) MDCT
    // spectrum set (~86 KB at 5.1+coupling); block_exps_ the per-slot raw
    // exponent sets; fixed_/fixed_base_ the flattened fixed-point bins and
    // their per-slot offsets; block_tokens_ each block's mantissa tokens,
    // filled through MantissaBlockWriter::take_tokens_into so the token
    // storage cycles between the writer and these slots without copies.
    std::vector<std::array<double, 256>> coeffs_;
    std::vector<std::int32_t> fixed_;
    std::vector<std::size_t> fixed_base_;
    std::vector<std::vector<std::uint8_t>> block_exps_;
    std::array<std::vector<MantissaToken>, kBlocksPerFrame> block_tokens_;
    std::uint64_t rate_accumulator_ = 0;  // ideal-bits Bresenham state
    std::uint64_t words_emitted_ = 0;
    // The previous frame's converged SNR-offset composite, warm-starting the
    // next frame's search (src/forge/src/encoder/snr_search.hpp). Performance
    // state only: it changes how fast the search converges, never which
    // offset it converges to. Negative until a frame has been encoded.
    int snr_search_hint_ = -1;
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
