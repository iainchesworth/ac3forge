// Bitstream-aware loudness QC (roadmap C2): decode a stream, measure it with
// the real BS.1770-4/EBU Tech 3342 meter, and check the result against a
// named delivery-spec gate from ac3::meta::qc.hpp.
//
// ac3cli qc is the same idea over a real file on disk; this shows the
// library API underneath it end to end. The stream here is deliberately
// encoded with a dialnorm that does NOT match the audio's real level - the
// exact authoring mistake this command exists to catch - so the report below
// has a genuine mismatch, not a coincidental match.

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
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/qc.hpp"

namespace {

constexpr ac3::Acmod kAcmod = ac3::Acmod::k3_2;
constexpr bool kLfe = true;
constexpr int kFrames = 130;  // ~4.2 s: several full 400 ms BS.1770 gate windows
constexpr std::array<double, 6> kTones{1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
constexpr double kAmplitude = 0.5;
// The embedded claim: dialogue at -31 dBFS (about as quiet as dialnorm can
// say). The tone mix actually encoded below sits well above that, so the
// measured-vs-embedded delta reported below is real, not rounding noise.
constexpr int kEmbeddedDialnorm = 31;

void fill(std::vector<std::vector<float>>& pcm, int frame) {
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
            pcm[ch][static_cast<std::size_t>(n)] =
                static_cast<float>(kAmplitude * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
        }
    }
}

}  // namespace

int main() {
    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

    // --- encode with a dialnorm that does not match the real level --------
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
        .bitrate_kbps = 448, .dialnorm = kEmbeddedDialnorm, .acmod = kAcmod, .lfe = kLfe});
    ac3::FrameDecoder decoder;
    // Same meter ac3cli qc itself uses: BS.1770 Table 3 channel weighting,
    // built from the STREAM's own acmod/lfe (never assumed).
    ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, kAcmod, kLfe};

    for (int frame = 0; frame < kFrames; ++frame) {
        fill(pcm, frame);
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        // A real QC pass measures the DECODED audio, not the source PCM -
        // the whole point is to check what the stream actually carries.
        const auto decoded = decoder.decode_frame(*encoded);
        if (!decoded) {
            std::printf("decode failed: %.*s\n",
                        static_cast<int>(ac3::describe(decoded.error()).size()),
                        ac3::describe(decoded.error()).data());
            return 1;
        }
        std::vector<std::span<const float>> decoded_views;
        decoded_views.reserve(decoded->channels.size());
        for (const auto& channel : decoded->channels) {
            decoded_views.emplace_back(channel);
        }
        meter.push(decoded_views);
    }

    // --- measure, and compare against the embedded dialnorm ----------------
    const auto lkfs = meter.integrated_lkfs();
    const auto peak = meter.true_peak_dbtp();
    if (!lkfs) {
        std::printf("no audio above the -70 LKFS absolute gate: nothing to report\n");
        return 1;
    }
    // §5.4.2.8: dialnorm states how far dialogue sits below digital 100%, so
    // the stream's own claimed programme level is simply its negation.
    const double claimed_lkfs = -static_cast<double>(kEmbeddedDialnorm);
    std::printf("measured %.2f LKFS, true peak %.2f dBTP\n", *lkfs, peak.value_or(0.0));
    std::printf("embedded dialnorm %d claims %.2f LKFS -> delta %+.2f dB\n", kEmbeddedDialnorm,
                claimed_lkfs, *lkfs - claimed_lkfs);
    std::printf("a measurement-derived dialnorm would be %d, not %d\n",
                ac3::meta::dialnorm_from_lkfs(*lkfs), kEmbeddedDialnorm);

    // --- gate the measurement against every named delivery spec ------------
    for (const auto id : ac3::meta::kQcPresetIds) {
        const auto preset = ac3::meta::qc_preset(id);
        const auto name = ac3::meta::qc_preset_name(id);
        const auto verdict = ac3::meta::evaluate_qc_gate(preset, lkfs, peak);
        std::printf("%.*s: target %+.1f +/-%.1f LKFS, peak <= %+.1f dBTP -> %s\n",
                    static_cast<int>(name.size()), name.data(), preset.target_lkfs,
                    preset.tolerance_lu, preset.max_true_peak_dbtp, verdict.pass() ? "PASS" : "FAIL");
    }
    return 0;
}
