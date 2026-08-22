#pragma once

#include <algorithm>

// The SNR-offset search's shared core, warm-startable. Both encoders (AC-3's
// encoder.cpp and E-AC-3's eac3_frame.cpp) rate-control the same way: find
// the largest composite SNR offset ((csnroffst << 4) | fsnroffst, 0..1023)
// whose mantissa cost still fits the frame's budget - a monotone predicate,
// binary-searched. That search is the whole of CBR's rate control and runs
// its bit-allocation evaluation ~11 times a frame; the phase-5 Tracy zones
// (PR #115/#120) put that cluster at the top of both paths' profiles.
//
// Internal to src/forge/src/encoder/ on purpose - this is plumbing between the
// two encoder translation units, not library surface.

namespace ac3::internal {

// Largest x in [0, limit] with fits(x), where fits is monotone
// non-increasing in x (true up to some boundary, false beyond it). Returns 0
// when nothing fits, exactly as the plain binary search's lo never moves off
// 0 in that case.
//
// `hint` is the previous frame's converged answer (or negative when there is
// none - then this IS the plain binary search over [0, limit]). The hint
// changes HOW FAST the answer is found, never WHAT it is: the bracket grown
// around it provably contains the boundary - every failed probe bounds the
// answer from above by monotonicity, every fitting probe bounds it from
// below - and the search inside the bracket is the same binary loop the
// cold path runs. Consecutive frames of real program material converge to
// the same or a near-neighbouring offset, so the common case is 2-3
// evaluations instead of log2(limit + 1) + 1.
template <typename Fits>
int search_max_fitting(int limit, int hint, Fits&& fits) {
    int lo = 0;
    int hi = limit;
    if (hint >= 0 && hint <= limit) {
        if (fits(hint)) {
            // The answer is at or above the hint: march the lower bound up
            // in doubling steps until a probe fails (new upper bound) or the
            // range ends.
            lo = hint;
            int step = 1;
            while (lo + step <= limit && fits(lo + step)) {
                lo += step;
                step *= 2;
            }
            hi = std::min(limit, lo + step - 1);
        } else {
            // The answer is strictly below the hint: march the upper bound
            // down in doubling steps until a probe fits (new lower bound) or
            // the bottom of the range is passed.
            int step = 1;
            hi = hint - 1;
            while (hi >= 0) {
                const int probe = std::max(0, hi - step + 1);
                if (fits(probe)) {
                    lo = probe;
                    break;
                }
                hi = probe - 1;
                step *= 2;
            }
            if (hi < 0) {
                return 0;
            }
        }
    }
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fits(mid)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

}  // namespace ac3::internal
