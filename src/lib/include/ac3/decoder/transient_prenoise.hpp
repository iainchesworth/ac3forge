#pragma once

#include <span>

#include "ac3/export.hpp"

// A/52:2018 §3.7.2 / Figure E3.2: transient pre-noise time-scaling synthesis.
// A pure post-process on decoded PCM - it touches no bitstream, transform or
// bit-allocation state, only the sample buffer - that overwrites the
// pre-echo a low-bit-rate transient leaves ahead of it with a synthesized
// copy of the (clean) audio already decoded just before that pre-echo.
//
// This is unrelated to enhanced coupling (ac3::eac3::ecpl_*) and to block
// switching (ac3::TransientDetector) - both are encoder-side or transform-
// domain tools; this one runs entirely after IMDCT, on the time-domain
// output, and is signaled per full-bandwidth channel by the decoder's own
// transproce/chintransproc/transprocloc/transproclen fields.

namespace ac3 {

// TC1/TC2 in the spec's own naming: the two synthesis-buffer/cross-fade
// system constants used throughout §3.7.2's pseudocode.
inline constexpr int kTransientPrenoiseTC1 = 256;
inline constexpr int kTransientPrenoiseTC2 = 128;

// The half-open sample range [first, last) apply_transient_prenoise needs to
// both read from and write into for a given transloc/translen - both the
// synthesis-buffer source (which reaches furthest back) and the corrected
// region itself fall inside it. apply_transient_prenoise does not bounds-
// check its own span (it is a plain, unchecked buffer view), so a caller
// working from untrusted bitstream fields must check this range against
// whatever history it actually has before calling it.
struct TransientPrenoiseRange {
    int first = 0;
    int last = 0;
};
[[nodiscard]] AC3FORGE_EXPORT TransientPrenoiseRange transient_prenoise_range(int transloc,
                                                                             int translen);

// Applies the correction in place. `pcm` is one full-bandwidth channel's
// decoded samples, indexed so that `pcm[transloc]` is the sample the
// transient itself starts at (§3.7.2's transprocloc, already multiplied by
// 4 and offset to this buffer's own indexing - not the raw bitstream field).
// `translen` is transproclen, unscaled (already in samples).
//
// `pcm` must hold valid history reaching back to index
// `transloc - (2*kTransientPrenoiseTC1 + 2*pnlen)`, where pnlen is derived
// internally from transloc (the distance back to the leading edge of the
// audio coding block immediately before the one the transient falls in -
// §3.7.1: this is derived, not transmitted). The correction itself is
// written into [transloc - (pnlen + translen + TC1), transloc - pnlen).
// Both bounds are the caller's responsibility to keep in range; this
// function does not itself know where a frame boundary sits.
AC3FORGE_EXPORT void apply_transient_prenoise(std::span<float> pcm, int transloc, int translen);

}  // namespace ac3
