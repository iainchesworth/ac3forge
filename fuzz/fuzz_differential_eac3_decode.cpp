#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "differential_oracle.hpp"

// Differential mode for E-AC-3 (roadmap G3): the same split_access_units +
// Eac3Decoder path fuzz_eac3_decode.cpp already crash-fuzzes (this harness
// shares its seed corpus - see fuzz/run.sh's seed_source_for), but instead
// of only checking for a crash/sanitizer trip, this decodes the SAME
// mutated bytes a second time with FFmpeg and diffs the resulting PCM. See
// fuzz/differential_oracle.hpp's own module comment for exactly when a
// mismatch is treated as a reportable divergence versus expected
// disagreement on a mutated/malformed frame.
//
// FFmpeg has no reading at all of enhanced coupling, transient pre-noise
// processing, or a second dependent substream (7.1.4) - see
// docs/verification.md's "Where the oracles don't reach" and run-codec-
// matrix.sh's own header comment. None of that needs special-casing here:
// whatever the reason FFmpeg declines a stream this project's own decoder
// accepted - a known gap or a genuine bug on either side -
// ffmpeg_strict_decode simply returns false and run_differential treats it
// as "no oracle for this one", the exact same stance run-codec-matrix.sh
// already takes by skipping those streams outright rather than tolerating a
// known failure.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    const auto units = ac3::split_access_units(bytes);
    if (!units || units->empty()) {
        return 0;
    }

    ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> pcm;
    ac3::DecodedAccessUnit first{};
    bool have_first = false;

    // Mirrors ac3cli's own 'decode' path for E-AC-3 (apps/cli/main.cpp:
    // run_decode_eac3) exactly, including the §3.7 transient-pre-noise-
    // processing hold-back convention (decode_access_unit's own doc
    // comment) and the end-of-stream flush() below.
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            return 0;  // our own decoder declined this input - nothing to compare
        }
        if (!decoded->has_value()) {
            continue;  // held back pending transient pre-noise processing, not an error
        }
        const auto& out = **decoded;
        if (!have_first) {
            first = out;
            pcm.resize(out.channels.size());
            have_first = true;
        } else if (out.acmod != first.acmod || out.sample_rate != first.sample_rate) {
            // Format changed mid-stream: not a shape FFmpeg's single raw PCM
            // output could ever be compared against meaningfully.
            return 0;
        }
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), out.channels[ch].begin(), out.channels[ch].end());
        }
    }
    for (auto& substream : decoder.flush()) {
        if (!have_first) {
            first.sample_rate = substream.sample_rate;
            first.acmod = substream.acmod;
            first.dialnorm = substream.dialnorm;
            first.substream_count = 1;
            first.layout = ac3::eac3::chanmap::expand(substream.location_map());
            pcm.resize(substream.channels.size());
            have_first = true;
        }
        for (std::size_t ch = 0; ch < substream.channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), substream.channels[ch].begin(),
                           substream.channels[ch].end());
        }
    }
    if (!have_first || pcm.empty()) {
        return 0;
    }

    const auto sample_rate = ac3::sample_rate_hz(first.sample_rate);
    if (first.acmod == ac3::Acmod::kDualMono) {
        // §E1.3: two unrelated programmes, no Table E2.5 location to order
        // by - Ch1/Ch2 go out in coded order, the identity write_wav_f32's
        // default (empty channel_order) already gives.
        ac3forge::fuzzdiff::run_differential("eac3", bytes, ".ec3", pcm, sample_rate, {});
        return 0;
    }
    const auto map = ac3::plan::wav_order(
        std::span{first.layout.items}.first(static_cast<std::size_t>(first.layout.count)));
    ac3forge::fuzzdiff::run_differential("eac3", bytes, ".ec3", pcm, sample_rate, map);
    return 0;
}
