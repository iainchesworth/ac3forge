#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ac3/export.hpp"

// Offline, whole-buffer sample-rate conversion for a loaded file - NOT the
// live capture drift-correction resampler (ac3::capture::DriftResampler,
// src/audio/include/ac3/capture/resampler.hpp). That one runs once per
// audio-thread callback, correcting tens-of-ppm clock drift between two
// devices, so it deliberately spends nothing on kernel quality: linear
// interpolation is accurate enough at those drift magnitudes and keeps the
// hot path allocation-free. This one runs exactly once per loaded second
// source file, converting its whole native rate to the session's primary
// rate (e.g. 44.1kHz -> 48kHz) before it ever reaches the encoder - a
// completely different cost/quality tradeoff, since a one-shot offline
// conversion can and should afford a proper windowed-sinc polyphase FIR
// kernel instead of a cheap two-tap interpolation. See resampler.cpp for the
// window choice, kernel width and cutoff backoff, and why each was picked.

namespace ac3::dsp {

// Resamples one channel of audio from input_rate to output_rate via a
// windowed-sinc polyphase FIR filter, computed offline over the whole
// buffer at once. For each output frame, walks the fractional input
// position the resample ratio implies and evaluates a fixed-width
// windowed-sinc kernel centered there - the standard polyphase realization,
// good for any rational or irrational ratio without needing a rational
// approximation or an interpolation/decimation stage pair.
//
// input_rate == output_rate is handled as an exact identity (the input is
// copied back unchanged) rather than run through the filter - a 1:1
// "conversion" should reproduce its input exactly, not merely approximate
// it to within the filter's passband ripple.
//
// input_rate == 0 or output_rate == 0 is meaningless (there is no filter
// cutoff, and no output length, that corresponds to a zero sample rate) and
// returns an empty result rather than dividing by zero or fabricating a
// ratio - this is the only case (short of an empty `input`) that returns
// fewer than round(input.size() * output_rate / input_rate) frames.
[[nodiscard]] AC3FORGE_EXPORT std::vector<float> resample(std::span<const float> input,
                                                           std::uint32_t input_rate,
                                                           std::uint32_t output_rate);

// Convenience over resample(): resamples every channel of a planar
// multi-channel buffer independently - the same shape ac3::io::WavData::
// channels uses (one std::vector<float> per channel, not interleaved).
// Each output channel is exactly what calling resample() on that channel
// alone would produce; channels never influence one another (no shared
// stereo/multichannel state, no crosstalk), matching how a loaded WAV's
// channels are otherwise treated as independent streams up to this point in
// the pipeline.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::vector<float>> resample_planar(
    std::span<const std::vector<float>> channels, std::uint32_t input_rate,
    std::uint32_t output_rate);

}  // namespace ac3::dsp
