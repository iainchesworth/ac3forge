// Dynamic range control, heavy compression, and a measured dialnorm.
//
// An AV receiver reads exactly these bits to set level, compress dynamics and
// fold down to fewer speakers than the stream carries. Leaving them at their
// defaults is a decision, not a neutral choice.
//
// dialnorm cannot be derived from the frame being encoded: BS.1770 gating is
// defined over the whole programme, so the caller measures first and
// configures second, which is what an analysis pass in a real encoder does.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"

namespace {

constexpr ac3::Acmod kAcmod = ac3::Acmod::k3_2;
constexpr bool kLfe = true;
// 250 frames * 1536 samples / 48000 Hz = 8 s: long enough for short-term
// loudness's 3 s window and a non-trivial Loudness Range (the loud/quiet
// split below gives it two clearly different levels to span), not just
// integrated loudness's 400 ms floor.
constexpr int kFrames = 250;
constexpr std::array<double, 6> kTones{1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};

// Loud for the first half, quiet for the second, so the compressor has
// something to act on.
void fill(std::vector<std::vector<float>>& pcm, int frame) {
    const double amplitude = frame < kFrames / 2 ? 0.7 : 0.05;
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
            pcm[ch][static_cast<std::size_t>(n)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
        }
    }
}

}  // namespace

int main() {
    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

    // --- pass one: measure -------------------------------------------------
    // Weights follow BS.1770 Table 3: unity front, +1.5 dB surrounds, LFE
    // excluded outright.
    ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, kAcmod, kLfe};
    for (int frame = 0; frame < kFrames; ++frame) {
        fill(pcm, frame);
        meter.push(views);
    }

    // nullopt until at least one 400 ms block has passed the absolute gate:
    // silence has no meaningful loudness, and inventing one would put a wrong
    // dialnorm on the stream.
    const auto lkfs = meter.integrated_lkfs();
    const int dialnorm = lkfs ? ac3::meta::dialnorm_from_lkfs(*lkfs) : 31;
    std::printf("measured %.2f LKFS -> dialnorm %d\n", lkfs.value_or(0.0), dialnorm);

    // The rest of an R128 meter, read off the same pass: momentary (400 ms)
    // and short-term (3 s) are the un-gated windows integrated_lkfs() gates
    // internally, reported directly; Loudness Range is EBU Tech 3342's own
    // gated-percentile statistic; true peak (BS.1770-4 Annex 2) is the only
    // one of the four that is not a loudness figure at all - it is an
    // oversampled peak-sample estimate, and the only measure here that does
    // NOT exclude the LFE channel.
    std::printf("momentary %.2f LKFS, short-term %.2f LKFS, LRA %.2f LU, true peak %.2f dBTP\n",
                meter.momentary_lkfs().value_or(0.0), meter.short_term_lkfs().value_or(0.0),
                meter.loudness_range().value_or(0.0), meter.true_peak_dbtp().value_or(0.0));

    // --- pass two: encode with the metadata --------------------------------
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
        .bitrate_kbps = 448,
        .dialnorm = dialnorm,
        .acmod = kAcmod,
        .lfe = kLfe,
        // §7.7.1. A/52 fixes the wire format and the intent but never the
        // curve, so the profile is this project's reading of it.
        .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
        // §7.7.2, independent of drc: the two answer different questions, so a
        // stream may carry either, both or neither.
        .heavy = ac3::meta::HeavyConfig{.dialogue_target_dbfs = -20.0,
                                        .peak_ceiling_dbfs = -0.5},
        // Tables 5.9 / 5.10. These always define the §7.8 downmix, whatever
        // acmod is, so the heavy-compression peak detector consults them too.
        .cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB,
        .surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB,
    });

    std::vector<std::byte> stream;
    for (int frame = 0; frame < kFrames; ++frame) {
        fill(pcm, frame);
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }

    std::printf("%zu bytes of 5.1 AC-3 carrying DRC, compr and dialnorm\n", stream.size());
    return 0;
}
