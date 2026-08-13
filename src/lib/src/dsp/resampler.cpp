#include "ac3/dsp/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ac3::dsp {

namespace {

// Taps on EACH side of the kernel's center - 32 gives 64ish taps total
// (the exact count per output frame varies by fractional phase and buffer
// edges; see the k-range comment in resample_one() below). This sits at the
// top of the "16-32 taps each side" range a one-shot, whole-file offline
// conversion can comfortably afford: cost here is O(output_frames *
// kHalfWidth), paid once per loaded file rather than per audio-thread
// callback, so there is no reason to pinch it the way DriftResampler's live
// hot path has to. Wider buys a narrower transition band and deeper
// stopband; 32 is already well past the point of diminishing audible
// returns for feeding a lossy AC-3/E-AC-3 encoder downstream.
constexpr std::size_t kHalfWidth = 32;

// The ideal brick-wall cutoff sits at kCutoffBackoff of the LIMITING rate's
// Nyquist (the lower of input/output - the one that actually constrains
// what frequency content either side of the conversion can represent
// without aliasing). Backing off by 10% from that Nyquist, rather than
// designing a razor's-edge filter AT Nyquist, leaves the window's finite
// transition band room to roll off to the stopband BEFORE Nyquist instead
// of straddling it - a brick wall placed exactly at Nyquist would let a
// windowed (i.e. non-infinite, non-instant) filter's transition band poke
// out past Nyquist and alias/image regardless of how good the window is.
constexpr double kCutoffBackoff = 0.9;

// sin(pi*x)/(pi*x), defined as 1 at x == 0 via the removable-singularity
// limit - the textbook normalized sinc that is the ideal lowpass filter's
// impulse response shape.
double normalized_sinc(double x) {
    if (std::abs(x) < 1e-9) {
        return 1.0;
    }
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

// Blackman window, evaluated as a continuous function of tau (rather than
// the usual fixed table of integer-sample coefficients) because the
// polyphase kernel below needs the window's value at arbitrary FRACTIONAL
// offsets from its center - every output frame lands at a different
// fractional phase against the input's sample grid, not just at the
// integer taps a conventional fixed-length FIR design would use.
//
// Blackman over Kaiser: Blackman's ~-58dB sidelobe/stopband level is
// already comfortably below both this codec's practical noise floor and
// float32's own precision for a resampling stage feeding a lossy encoder,
// and it needs no Bessel-function evaluation (Kaiser's I0(..) term) to get
// there - a fixed, closed-form shape is less code and one fewer place for a
// numerical bug to hide in a clean-room implementation. Kaiser would buy a
// few more dB of stopband depth and a tunable width/attenuation tradeoff
// neither of which this offline, one-shot conversion actually needs.
double blackman_window(double tau, double half_width) {
    if (std::abs(tau) >= half_width) {
        return 0.0;
    }
    const double x = tau / half_width;  // (-1, 1)
    return 0.42 + 0.5 * std::cos(std::numbers::pi * x) + 0.08 * std::cos(2.0 * std::numbers::pi * x);
}

// One tap of the windowed-sinc lowpass kernel: the ideal lowpass filter's
// impulse response at cutoff `cutoff_norm` (a fraction of the INPUT sample
// rate - tau below is measured in input-sample units, since it is always
// `pos - k` for an input-sample index k), evaluated `tau` input-samples
// away from the kernel's center and tapered by the Blackman window so the
// (otherwise infinite-support) ideal sinc becomes usable as a finite
// kernel.
double kernel_tap(double tau, double cutoff_norm) {
    return 2.0 * cutoff_norm * normalized_sinc(2.0 * cutoff_norm * tau) *
           blackman_window(tau, static_cast<double>(kHalfWidth));
}

// The actual per-channel resampler. `resample()` is just this plus the
// identity/zero-rate special cases documented in the header.
std::vector<float> resample_one(std::span<const float> input, std::uint32_t input_rate,
                                 std::uint32_t output_rate) {
    const double ratio = static_cast<double>(output_rate) / static_cast<double>(input_rate);

    // Cutoff, in Hz, then re-expressed as a fraction of the INPUT rate -
    // kernel_tap()'s convention, since every tap it computes is indexed by
    // an offset measured in input samples.
    const double limiting_rate =
        std::min(static_cast<double>(input_rate), static_cast<double>(output_rate));
    const double cutoff_hz = kCutoffBackoff * limiting_rate / 2.0;
    const double cutoff_norm = cutoff_hz / static_cast<double>(input_rate);

    const auto in_frames = input.size();
    // Nearest whole frame count for the exact ratio - rounding (rather than
    // truncating) keeps the resampled buffer's duration as close as
    // possible to the input's, instead of always shortening it by up to
    // one ratio-scaled frame.
    const auto out_frames =
        static_cast<std::size_t>(std::llround(static_cast<double>(in_frames) * ratio));
    std::vector<float> output(out_frames);

    // Input samples one output frame steps forward by - the reciprocal of
    // `ratio`, since a HIGHER output rate (ratio > 1, e.g. upsampling
    // 44.1->48kHz) means each output frame is CLOSER together in input-
    // sample terms, and vice versa for downsampling.
    const double input_step = 1.0 / ratio;

    for (std::size_t m = 0; m < out_frames; ++m) {
        // Fractional position, in INPUT frames, that output frame m reads
        // from.
        const double pos = static_cast<double>(m) * input_step;
        const auto n0 = static_cast<long long>(std::floor(pos));
        // Every input index this output frame's kernel can reach: from
        // n0 - kHalfWidth + 1 (worst case, pos's fractional part -> 1) to
        // n0 + kHalfWidth (worst case, pos's fractional part -> 0), which
        // keeps every tau = pos - k inside (-kHalfWidth, kHalfWidth] -
        // blackman_window()'s support - without ever needing to compute a
        // tap known in advance to be zero.
        const auto lo = n0 - static_cast<long long>(kHalfWidth) + 1;
        const auto hi = n0 + static_cast<long long>(kHalfWidth);

        double acc = 0.0;
        double weight_sum = 0.0;
        for (long long k = lo; k <= hi; ++k) {
            const double tau = pos - static_cast<double>(k);
            const double w = kernel_tap(tau, cutoff_norm);
            if (w == 0.0) {
                continue;
            }
            // The kernel's own weight sum at THIS fractional phase, not the
            // textbook ∑ h(phase - k) == 1 identity - that identity only
            // holds exactly for an infinite, unwindowed sinc at a cutoff of
            // exactly Nyquist. Windowing to finite width, backing the
            // cutoff off below Nyquist, and (right at the buffer's start/
            // end) truncating taps that fall outside `input` altogether all
            // leave a small phase-dependent gain ripple in the raw
            // ∑ h(phase - k) sum; dividing the accumulated output by
            // whatever that sum ACTUALLY came to here cancels it, holding
            // passband gain flat (see test_dsp_resampler.cpp's in-band
            // amplitude check) and gracefully rescaling the shortened
            // kernel at the very edges of `input` instead of silently
            // losing energy there the way implicit zero-padding alone
            // would.
            weight_sum += w;
            if (k >= 0 && static_cast<std::size_t>(k) < in_frames) {
                acc += w * static_cast<double>(input[static_cast<std::size_t>(k)]);
            }
            // k outside [0, in_frames) contributes nothing to `acc` (an
            // implicit zero-pad past either end of the real data) but still
            // counts toward weight_sum, which is exactly what the
            // normalization above needs to rescale for a truncated edge
            // kernel.
        }

        output[m] = weight_sum > 0.0 ? static_cast<float>(acc / weight_sum) : 0.0f;
    }

    return output;
}

}  // namespace

std::vector<float> resample(std::span<const float> input, std::uint32_t input_rate,
                             std::uint32_t output_rate) {
    if (input.empty()) {
        return {};
    }
    if (input_rate == output_rate) {
        // Exact identity, not "close": running an identical-rate buffer
        // through the filter would still cost a pass over every sample and
        // introduce the kernel's own (tiny but nonzero) passband ripple for
        // no reason - a 1:1 "conversion" has an exact answer, so return it.
        return std::vector<float>(input.begin(), input.end());
    }
    if (input_rate == 0 || output_rate == 0) {
        // A zero rate has no corresponding cutoff frequency and no
        // meaningful output duration - there is no "correct" nonzero-length
        // answer to invent here, and computing one would divide by zero
        // (the ratio and cutoff_norm above both use input_rate/output_rate
        // as denominators). Empty is the honest answer: no valid data was
        // given to resample.
        return {};
    }
    return resample_one(input, input_rate, output_rate);
}

std::vector<std::vector<float>> resample_planar(std::span<const std::vector<float>> channels,
                                                  std::uint32_t input_rate,
                                                  std::uint32_t output_rate) {
    std::vector<std::vector<float>> result;
    result.reserve(channels.size());
    for (const auto& channel : channels) {
        result.push_back(resample(channel, input_rate, output_rate));
    }
    return result;
}

}  // namespace ac3::dsp
