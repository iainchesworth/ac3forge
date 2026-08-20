#pragma once

#include <cstdint>
#include <span>

#include "ac3/export.hpp"

// Small, real-time-safe IIR filtering for bundle C's explicit LFE routing:
// when a caller uses the assignment table (ac3::plan::Assignment - see
// docs/gui/source-assignment.md's LFE row) to send a full-bandwidth source
// channel onto the LFE or LFE2 position by hand, that content is not a
// file's own dedicated LFE channel - the automatic routing that carries one
// of those is a separate concern and passes it through untouched, nothing
// here applies to it. A hand-assigned full-range channel is arbitrary
// content the caller has chosen to place on a speaker position that a real
// subwoofer, and the LFE channel's own +10 dB mixing headroom, both assume
// carries only deep bass. Left unfiltered it would put energy well above
// what that position is meant to carry, so the caller wiring this up
// (elsewhere - not this file, see plan.hpp/assignment.hpp) band-limits it
// first with LfeLowpass.
//
// Biquad is the reusable second-order building block (Direct Form II
// Transposed, one instance per complex-conjugate pole pair - a single real-
// coefficient biquad can only realize one such pair); LfeLowpass cascades
// two of them, tuned to different Q values, into one 4th-order Butterworth
// low-pass - the standard way to build a stable higher-order Butterworth
// filter out of second-order sections. Both are allocation-free and carry
// only their own delay-line state between calls, matching this codebase's
// other per-run, caller-driven DSP objects (e.g. ac3::capture::
// DriftResampler, ac3::analysis::LevelMeter): construct once per run,
// process() once per frame in frame order for the run's whole life,
// reset() only at a run boundary.

namespace ac3::dsp {

// A single second-order IIR section in Direct Form II Transposed (the
// numerically well-behaved standard form - two state variables, no separate
// input/output delay lines to keep in sync). Coefficients are normalized so
// a0 == 1 (i.e. b0,b1,b2,a1,a2 only - a0 already divided out).
class AC3FORGE_EXPORT Biquad {
public:
    // Replaces the section's coefficients; does NOT touch the delay-line
    // state (z1_/z2_), so a caller retuning a running filter keeps whatever
    // continuity that implies - LfeLowpass never needs this (it sets
    // coefficients once, at construction), but a Biquad used standalone may.
    void set_coefficients(double b0, double b1, double b2, double a1, double a2);

    // One sample in, one sample out, advancing the delay line by exactly one
    // step. Internally accumulates in double even though the interface is
    // float, matching this codebase's other single-sample-at-a-time filter
    // state (see DriftResampler's own float-in/double-state discipline) -
    // the extra headroom keeps the coefficients' precision from being thrown
    // away by every sample's round trip through float.
    [[nodiscard]] float process(float sample);

    // Zeroes the delay-line state only; coefficients set by
    // set_coefficients() are untouched. Call once per run start (or after a
    // discontinuity a caller wants to not smear into what follows) -
    // matching every other per-run reset in this codebase (e.g.
    // ac3::analysis::LevelMeter::reset()).
    void reset();

private:
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

// Two cascaded Biquad sections realizing a 4th-order Butterworth low-pass -
// the filter bundle C's LFE handling applies to a full-bandwidth channel
// explicitly assigned onto LFE/LFE2 (see docs/gui/source-assignment.md's LFE
// note): sending full-range content there unfiltered would put energy well
// above what an LFE channel and a real subwoofer are meant to carry.
//
// Split as two second-order sections rather than one fourth-order
// difference equation because that is how a real-coefficient IIR filter of
// order > 2 is built at all: the four poles of a 4th-order Butterworth
// low-pass form two complex-conjugate pairs, and a biquad is exactly a
// filter with one such pair (plus the matching zero pair, both at Nyquist
// for a low-pass) - cascading two tuned biquads IS the 4th-order filter,
// not an approximation of one. The two sections use deliberately different
// Q values (see biquad.cpp); using the same Q for both would give a stable
// 4th-order low-pass, but not a maximally-flat Butterworth one.
class AC3FORGE_EXPORT LfeLowpass {
public:
    // corner_hz: -3dB point, ~120 Hz for this project's LFE use.
    // sample_rate: the coded stream's rate (drives the bilinear-transform
    // coefficients) - must be > 2*corner_hz. A corner_hz that is at or past
    // Nyquist (sample_rate/2), or a sample_rate of 0, is not something a
    // legitimate caller ever passes - every coded rate this project supports
    // is many multiples of 120 Hz - so rather than let such input produce
    // NaN/inf coefficients (division by zero, or the bilinear transform's
    // warping blowing up as the corner approaches Nyquist), the constructor
    // clamps corner_hz into a safe band comfortably below Nyquist. See
    // biquad.cpp for the exact bound.
    LfeLowpass(double corner_hz, std::uint32_t sample_rate);

    // In-place low-pass over one frame's worth of samples, carrying filter
    // state (the two sections' delay lines) across calls - call once per
    // encode frame for the life of a run, in frame order, never skipping a
    // frame, or the filter's continuity breaks. No allocation.
    void process(std::span<float> samples);

    // Zeroes both sections' delay-line state (not coefficients) - call once
    // at the start of a run, matching every other per-run reset in this
    // codebase (e.g. ac3::analysis::LevelMeter::reset()).
    void reset();

private:
    Biquad stage1_;
    Biquad stage2_;
};

}  // namespace ac3::dsp
