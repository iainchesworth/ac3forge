#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"

// The live session's parallel 5.1 downmix receiver leg (bundle B2, item 16)
// feeds a second, independent ac3::FrameEncoder the main plan's ALREADY-
// COMPUTED bed channels - see encoder_controller.cpp's runLiveSession, and
// its own comment on why no separate §7.8 fold exists for this: every layout
// in this codebase renders its bed-position coded channels as a self-
// sufficient rendering of the whole programme (plan::route()/plan::render()),
// so the leg's own job is only "take that PCM and encode it as a legal AC-3
// stream at a clamped rate" - exactly what these tests exercise, without any
// GUI or live-capture machinery.

namespace {

// A real synthesized frame, not silence and not frame 0 (see CONTRIBUTING.md
// "Validation discipline") - a distinct tone per full-bandwidth channel, so a
// wrong channel order or a silently-dropped channel is distinguishable from a
// correct fold, and 3 frames encoded so the MDCT overlap is real by the last
// one.
std::vector<std::vector<float>> bed_frame(std::span<const double> tones_hz, std::uint64_t start,
                                          double amplitude = 0.3) {
    std::vector<std::vector<float>> pcm(tones_hz.size(),
                                        std::vector<float>(ac3::kSamplesPerFrame));
    for (std::size_t ch = 0; ch < tones_hz.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * tones_hz[ch] * n / 48000.0));
        }
    }
    return pcm;
}

// A crude but adequate frequency estimate for a single, dominant sine tone:
// samples per zero crossing, doubled (a full cycle crosses zero twice).
// Good enough to tell 200 Hz from 500 Hz apart, which is all these tests ask
// of it - not a spectral analyzer.
double zero_crossing_hz(std::span<const float> samples, double sample_rate) {
    int crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) {
            ++crossings;
        }
    }
    if (crossings == 0) {
        return 0.0;
    }
    const double seconds = static_cast<double>(samples.size()) / sample_rate;
    return static_cast<double>(crossings) / 2.0 / seconds;
}

double rms(std::span<const float> samples) {
    double sum_sq = 0.0;
    for (const auto s : samples) {
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum_sq / static_cast<double>(samples.size()));
}

}  // namespace

TEST_CASE("clamp_to_legal_ac3_bitrate feeds a bitrate the downmix leg's encoder actually accepts",
         "[tables][live-downmix]") {
    // The exact situation runLiveSession's downmix_encoder construction is
    // in: a plan's own bitrate (here, an off-table value no legal AC-3 rung
    // matches) has to become something plain AC-3 can legally carry BEFORE
    // it ever reaches EncoderConfig - is_valid_bitrate would otherwise
    // reject it outright.
    const std::uint32_t requested = 500;
    REQUIRE_FALSE(ac3::is_valid_bitrate(requested));
    const auto clamped = ac3::clamp_to_legal_ac3_bitrate(requested);
    CHECK(ac3::is_valid_bitrate(clamped));
    CHECK(clamped == 448);

    const ac3::EncoderConfig config{.sample_rate = ac3::SampleRate::k48000,
                                    .bitrate_kbps = clamped,
                                    .acmod = ac3::Acmod::k3_2,
                                    .lfe = true};
    // Construction alone must not throw/assert - proves the clamped value is
    // one the encoder actually accepts, not just one is_valid_bitrate agrees
    // with in isolation.
    REQUIRE_NOTHROW(ac3::FrameEncoder{config});
}

TEST_CASE("the downmix leg's encoder carries every bed channel's own content, not a silent or "
         "collapsed fold",
         "[encoder][live-downmix]") {
    // Five full-bandwidth tones plus a low LFE tone, all distinct - the same
    // "different content per channel when the test is about channel order or
    // separation" the repo's validation rule asks for. Order matches AC-3's
    // own L, C, R, Ls, Rs, with LFE synthesized separately and appended last
    // (config.lfe places it there, not the tone list).
    const std::vector<double> full_band_tones = {220.0, 330.0, 440.0, 550.0, 660.0};
    constexpr double kLfeToneHz = 60.0;

    const auto requested_kbps = ac3::clamp_to_legal_ac3_bitrate(500);
    const ac3::EncoderConfig config{.sample_rate = ac3::SampleRate::k48000,
                                    .bitrate_kbps = requested_kbps,
                                    .dialnorm = 27,
                                    .acmod = ac3::Acmod::k3_2,
                                    .lfe = true};
    ac3::FrameEncoder encoder{config};

    std::vector<std::byte> last_frame;
    std::uint64_t n = 0;
    for (int f = 0; f < 3; ++f) {
        auto pcm = bed_frame(full_band_tones, n);
        pcm.push_back(bed_frame(std::span{&kLfeToneHz, 1}, n, 0.5).front());
        n += static_cast<std::uint64_t>(ac3::kSamplesPerFrame);

        std::vector<std::span<const float>> views;
        views.reserve(pcm.size());
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        REQUIRE(views.size() == 6);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        last_frame = std::move(*frame);
    }

    ac3::FrameDecoder decoder;
    const auto decoded = decoder.decode_frame(last_frame);
    REQUIRE(decoded.has_value());
    CHECK(decoded->acmod == ac3::Acmod::k3_2);
    CHECK(decoded->lfe);
    // The frame really did carry the CLAMPED rate, not the plan's original
    // (illegal-for-AC-3) one - proves the clamp actually reached the encoder
    // rather than being computed and then ignored.
    CHECK(decoded->bitrate_kbps == requested_kbps);
    REQUIRE(decoded->channels.size() == 6);

    // Every full-bandwidth channel's dominant frequency lands near its own
    // tone, in AC-3 order (L, C, R, Ls, Rs) - a wrong permutation or a
    // channel silently zeroed would fail this, a correct fold-through
    // would not.
    for (std::size_t ch = 0; ch < full_band_tones.size(); ++ch) {
        CAPTURE(ch, full_band_tones[ch]);
        CHECK(rms(decoded->channels[ch]) > 0.05);
        const auto measured = zero_crossing_hz(decoded->channels[ch], 48000.0);
        CHECK(measured == Catch::Approx(full_band_tones[ch]).margin(15.0));
    }
    // LFE (index 5, last): present and non-silent, not required to match the
    // full-bandwidth zero-crossing estimate given its own band limiting.
    CHECK(rms(decoded->channels[5]) > 0.02);

    // No two full-bandwidth channels collapsed onto the same content - a
    // coarse but effective check against a fold that silently duplicates one
    // channel into several (e.g. a broken bed_views/chan_views span).
    for (std::size_t a = 0; a < full_band_tones.size(); ++a) {
        for (std::size_t b = a + 1; b < full_band_tones.size(); ++b) {
            CAPTURE(a, b);
            const auto freq_a = zero_crossing_hz(decoded->channels[a], 48000.0);
            const auto freq_b = zero_crossing_hz(decoded->channels[b], 48000.0);
            CHECK(std::abs(freq_a - freq_b) > 30.0);
        }
    }
}
