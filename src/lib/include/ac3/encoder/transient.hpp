#pragma once

#include <array>
#include <span>

#include "ac3/core/tables.hpp"

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
// the current tree") persist from one 512-sample analysis window to the
// next, so state belongs to the channel's stream, not to a single call.
class TransientDetector {
public:
    explicit TransientDetector(SampleRate sample_rate);

    // time[0..511]: this channel's 512-sample analysis window - the same
    // history_ + new-PCM array the encoder already builds, BEFORE the KBD
    // window (§8.2.2 says nothing about windowing, and the biquad's own
    // state needs the raw, continuous signal to filter meaningfully).
    // Returns blksw for this block: true exactly when a transient is found
    // in the SECOND half, per §8.2.2's own definition of the flag.
    bool detect(std::span<const double, 512> time);

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

    bool run_pass(std::span<const double, 256> half);

    std::array<Biquad, 2> stages_;

    // §8.2.2 step 3's cross-pass carry: the previous pass's own last segment
    // at each tree level, used as this pass's P[j][0].
    double prev_level1_ = 0.0;
    double prev_level2_ = 0.0;
    double prev_level3_ = 0.0;
    // The very first 512-sample block this instance ever sees has no real
    // "immediately prior tree" for EITHER of its two passes: the first
    // pass's own baseline is the default-constructed 0.0, and the second
    // pass's baseline is the first pass's result - which, for a freshly
    // constructed encoder, was computed from zero-filled history, not real
    // prior audio. Comparing genuine content against that synthetic silence
    // would flag a spurious transient on literally any non-silent stream's
    // opening block - see detect()'s own comment.
    bool first_block_ = true;
};

}  // namespace ac3
