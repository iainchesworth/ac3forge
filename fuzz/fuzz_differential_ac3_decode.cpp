#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/io/wav.hpp"
#include "differential_oracle.hpp"

// Differential mode for AC-3 (roadmap G3): the same split_frames +
// FrameDecoder path fuzz_ac3_decode.cpp already crash-fuzzes (this harness
// shares its seed corpus - see fuzz/run.sh's seed_source_for), but instead
// of only checking for a crash/sanitizer trip, this decodes the SAME
// mutated bytes a second time with FFmpeg and diffs the resulting PCM. See
// fuzz/differential_oracle.hpp's own module comment for exactly when a
// mismatch is treated as a reportable divergence versus expected
// disagreement on a mutated/malformed frame.
//
// This harness is deliberately much slower than fuzz_ac3_decode.cpp - every
// input it can actually compare spawns a real FFmpeg process - which is why
// it only pays that cost once THIS project's own decoder has already
// accepted the whole input end to end: the overwhelming majority of
// mutations get rejected immediately (bad sync word, bad CRC, a reserved
// field), and none of those are worth an FFmpeg process under a bounded
// time budget (see fuzz.yml's fuzz-differential job, and fuzz/README.md for
// why this is bounded mutation, not continuous fuzzing).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    const auto frames = ac3::split_frames(bytes);
    if (!frames || frames->empty()) {
        return 0;
    }

    ac3::FrameDecoder decoder;
    std::vector<std::vector<float>> pcm;
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    bool lfe = false;
    ac3::SampleRate sample_rate = ac3::SampleRate::k48000;
    bool have_first = false;

    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            return 0;  // our own decoder declined this input - nothing to compare
        }
        if (!have_first) {
            acmod = decoded->acmod;
            lfe = decoded->lfe;
            sample_rate = decoded->sample_rate;
            pcm.resize(decoded->channels.size());
            have_first = true;
        } else if (decoded->acmod != acmod || decoded->lfe != lfe ||
                   decoded->sample_rate != sample_rate) {
            // Format changed mid-stream: not a shape FFmpeg's single raw PCM
            // output could ever be compared against meaningfully.
            return 0;
        }
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), decoded->channels[ch].begin(),
                           decoded->channels[ch].end());
        }
    }
    if (!have_first || pcm.empty()) {
        return 0;
    }

    // Same channel-order convention ac3cli's own `decode` writes a WAV with
    // (src/cli/main.cpp: run_decode) - see ac3::io::wav_channel_order's own
    // doc comment.
    const auto map = ac3::io::wav_channel_order(acmod, lfe);
    ac3forge::fuzzdiff::run_differential("ac3", bytes, ".ac3", pcm, ac3::sample_rate_hz(sample_rate),
                                          map);
    return 0;
}
