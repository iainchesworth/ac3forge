#pragma once

#include <array>
#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Transient detection (A/52 §8.2.2): the basic-encoder recipe that decides
// blksw[ch], the one-bit-per-channel-per-block flag driving block switching.
// Shared by both encoders - the recipe is generation-agnostic, and neither
// this project's exponent/bit-allocation/mantissa layers nor the decoder need
// to know how blksw was chosen, only what it says.

namespace ac3 {

// One instance per full-bandwidth channel of a continuous encode: both the
// cascaded biquad's filter memory and the hierarchical peak tree's P[j][0]
// carry (§8.2.2 step 3's own note - "P[j][0]... is defined to be the peak of
// the last segment on level j of the tree calculated immediately prior to
// the current tree") persist from one 256-sample segment to the next, so
// state belongs to the channel's stream, not to a single call.
class AC3FORGE_EXPORT TransientDetector {
   public:
    explicit TransientDetector(SampleRate sample_rate);

    // pcm[0..255]: the 256 NEW samples this block period contributes - the
    // second half of the block's 512-sample analysis window, which is the
    // only half §8.2.2 defines blksw from - raw PCM, BEFORE the KBD window
    // (§8.2.2 says nothing about windowing, and the biquad's own state
    // needs the raw, continuous signal to filter meaningfully). Call once
    // per channel per block, in stream order; the window's FIRST half was
    // last block's call, whose tree this call compares against via the
    // persistent P[j][0] carry.
    //
    // This used to take the full 512-sample window and run BOTH halves
    // through the persistent cascade each call - but consecutive windows
    // overlap by 256 samples, so every segment was filtered twice (the
    // filter saw ...S0,S0,S1,S1,... instead of the continuous stream
    // §8.2.2's recipe describes) and the encoder paid twice the filtering
    // cost for the stutter. Streaming each segment exactly once is both the
    // spec-true reading and, measured with the phase-5 Tracy zones, the
    // single largest cost in encode_frame's former unzoned remainder.
    // Returns blksw for this block.
    bool detect(std::span<const float, 256> pcm);

   private:
    // Direct-form-I biquad section, cascaded two-deep as §8.2.2 literally
    // specifies ("cascaded biquad direct form I IIR filter"), each an RBJ
    // audio-EQ-cookbook high-pass at the mandated 8 kHz cutoff with a
    // Butterworth Q (1/sqrt(2)) - a standard, principled derivation for a
    // cutoff the spec fixes but does not hand a coefficient formula for.
    struct Biquad {
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        double process(double x);
    };

    bool run_pass(std::span<const float, 256> half);

    std::array<Biquad, 2> stages_;

    // §8.2.2 step 3's cross-pass carry: the previous pass's own last segment
    // at each tree level, used as this pass's P[j][0].
    double prev_level1_ = 0.0;
    double prev_level2_ = 0.0;
    double prev_level3_ = 0.0;
    // The very first segment this instance ever sees has no real
    // "immediately prior tree" to compare against - its baseline is the
    // default-constructed 0.0, i.e. synthetic silence, and comparing
    // genuine content against that would flag a spurious transient on
    // literally any non-silent stream's opening block - see detect()'s own
    // comment.
    bool first_block_ = true;
};

}  // namespace ac3
