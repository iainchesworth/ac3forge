#include "synth.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "/mnt/c/aa-wt/dolbydigitalconverter/header-inclusion-ci-check-453fde/apps/cli/support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/spatial/spatial.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

namespace {

// One tone per CODED channel, for the synthetic generators. The frequencies
// are deliberately spread and deliberately not harmonics of one another, so a
// channel that lands in the wrong speaker is measurable rather than merely
// suspected. One tone per CODED channel (coded_channels().size(), not the
// smaller rendered count) - a bed channel a dependent replaces still needs
// its own frequency, or nothing here could tell §E3.8.2's overwrite
// happening apart from the dependent being ignored altogether. The specific
// numbers mean nothing beyond being far enough apart to tell channels apart
// by ear; test_eac3_decoder.cpp's per-layout round trips pick their own
// frequencies independently rather than mirroring these.
std::vector<double> layout_tones(const plan::ChannelPlan& cp) {
    const auto coded = plan::coded_channels(cp);
    std::vector<double> out(coded.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 200.0 + 137.0 * static_cast<double>(i);
    }
    return out;
}

// Fills one frame of tones and hands back views onto it.
void fill_tones(std::vector<std::vector<float>>& samples,
                std::vector<std::span<const float>>& views,
                std::span<const double> tone_hz, double amplitude, std::uint64_t n0) {
    for (std::size_t ch = 0; ch < samples.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            samples[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * tone_hz[ch] *
                                     static_cast<double>(n0 + static_cast<std::uint64_t>(i)) /
                                     48000.0));
        }
        views[ch] = samples[ch];
    }
}

// Frames that cover `seconds` of audio, rounded up.
std::uint64_t frame_count(std::uint32_t seconds) {
    return (static_cast<std::uint64_t>(seconds) * 48000 + ac3::kSamplesPerFrame - 1) /
           ac3::kSamplesPerFrame;
}

}  // namespace

int run_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate) {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = bitrate});
    if (!frame) {
        std::println(stderr, "error: bitrate must be one of the 19 legal AC-3 rates");
        return 1;
    }
    const std::uint64_t count = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    if (!write_repeated_frame(out_path, *frame, count)) {
        return 1;
    }
    std::println("wrote {} silent frames to {}", count, out_path);
    return 0;
}

int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
             bool couple_flag, const Options& meta) {
    // A layout may be suffixed with "c" to turn channel coupling on (51c). A
    // bare 'couple' token does the same, so the flag that works for 'encode'
    // is not silently ignored here.
    const bool couple = couple_flag || (!layout.empty() && layout.back() == 'c');
    const std::string_view base = couple ? layout.substr(0, layout.size() - 1) : layout;

    plan::Plan p{.codec = plan::Codec::kAc3, .bitrate_kbps = bitrate, .meta = meta.p};
    std::string label;
    if (!resolve_layout(base, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    const auto config = plan::ac3_config(p);
    const auto cp = plan::resolve(p);

    // A one- or two-channel layout is the frequency-sweep case the freq_hz
    // argument exists for; anything wider gets a tone per speaker instead,
    // because one frequency in six channels cannot show where it ended up.
    auto tone_hz = layout_tones(cp);
    if (plan::rendered_channel_count(cp) <= 2) {
        std::ranges::fill(tone_hz, static_cast<double>(freq_hz));
    }

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    const auto nchans = static_cast<std::size_t>(encoder->channel_count());
    const double amplitude = amplitude_pct / 100.0;
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, 48000};

    const std::uint64_t count = frame_count(seconds);
    std::vector<std::vector<float>> samples(nchans,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    // Streamed out as encoded, keep_partial hard-off: this command has
    // never honoured keep-partial - its output is synthetic and
    // regenerable - so a failure must keep leaving no file behind, which
    // is what abort() then does.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false)) {
        return 1;
    }
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
        fill_tones(samples, views, tone_hz, amplitude, n0);
        n0 += ac3::kSamplesPerFrame;
        meter.process(views);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(*frame))) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println("wrote {} {} frames ({} kbps) to {}", count, label, bitrate, out_path);
    print_channel_summary(meter);
    return 0;
}

int run_eac3_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                  std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                  const Options& meta) {
    plan::Plan p{.codec = plan::Codec::kEac3, .bitrate_kbps = bitrate, .meta = meta.p};
    std::string label;
    if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }
    // No [tools] positional here (unlike eac3-encode), so fast-mdct=off is
    // this command's only way to reach it - same field, same meaning as
    // 'sine'/'encode's identical assignment.
    p.tools.fast_mdct = meta.fast_mdct;
    const auto config = plan::eac3_config(p);
    const auto cp = plan::resolve(p);

    auto tone_hz = layout_tones(cp);
    if (plan::rendered_channel_count(cp) <= 2) {
        std::ranges::fill(tone_hz, static_cast<double>(freq_hz));
    }
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    assert(nchans == tone_hz.size());
    const double amplitude = amplitude_pct / 100.0;

    const std::uint64_t count = frame_count(seconds);
    std::vector<std::vector<float>> samples(nchans,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    // Same output arrangement as 'sine' above, keep_partial hard-off for
    // the same synthetic-and-regenerable reason.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false)) {
        return 1;
    }
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
        fill_tones(samples, views, tone_hz, amplitude, n0);
        n0 += ac3::kSamplesPerFrame;
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: invalid E-AC-3 configuration");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println("wrote {} E-AC-3 {} access units ({} coded channels, {} substreams, "
                 "bsid 16) to {}",
                 count, label, nchans, config.dependents.size() + 1, out_path);
    return 0;
}

int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t orbit_seconds, const Options& meta) {
    ac3::spatial::BedRenderer renderer;
    const auto object =
        renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7, .lfe_send = 0.15});
    const plan::Plan p{.codec = plan::Codec::kAc3,
                       .layout = plan::LayoutId::k51,
                       .bitrate_kbps = bitrate,
                       .tools = {.fast_mdct = meta.fast_mdct},
                       .meta = meta.p};
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(p));
    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, 48000};

    const std::uint64_t count = frame_count(seconds);
    std::vector<float> mono(ac3::spatial::kBlockSamples);
    std::vector<std::vector<float>> frame_channels(6);
    std::vector<std::vector<float>> bed_block(
        6, std::vector<float>(ac3::spatial::kBlockSamples));
    std::vector<std::span<const float>> views(6);
    // Streamed out as encoded, keep_partial hard-off - synthetic and
    // regenerable, same as 'sine' above.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false)) {
        return 1;
    }
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
        for (auto& channel : frame_channels) {
            channel.clear();
        }
        for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
            const double seconds_now = static_cast<double>(n0) / 48000.0;
            renderer.set_target(object,
                                {.azimuth_deg = 360.0 * seconds_now /
                                                std::max<std::uint32_t>(orbit_seconds, 1),
                                 .gain = 0.7,
                                 .lfe_send = 0.15});
            for (std::size_t n = 0; n < mono.size(); ++n) {
                mono[n] = static_cast<float>(
                    0.6 * std::sin(2.0 * std::numbers::pi * 440.0 *
                                   static_cast<double>(n0 + n) / 48000.0));
            }
            n0 += mono.size();
            const std::array<std::span<const float>, 1> audio = {mono};
            const std::array<std::span<float>, 6> bed_views = {
                bed_block[0], bed_block[1], bed_block[2],
                bed_block[3], bed_block[4], bed_block[5]};
            renderer.render_block(audio, bed_views);
            for (std::size_t ch = 0; ch < 6; ++ch) {
                frame_channels[ch].insert(frame_channels[ch].end(), bed_block[ch].begin(),
                                          bed_block[ch].end());
            }
        }
        for (std::size_t ch = 0; ch < 6; ++ch) {
            views[ch] = frame_channels[ch];
        }
        meter.process(views);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(*frame))) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println("wrote {} 5.1 frames: 440 Hz tone orbiting every {} s -> {}", count,
                 orbit_seconds, out_path);
    // An orbit visits every speaker equally, so the summary's job here is to
    // show that no channel was left out and none dominates.
    print_channel_summary(meter);
    return 0;
}

int run_eac3_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                     std::string_view layout, const Options& meta) {
    plan::Plan p{.codec = plan::Codec::kEac3, .bitrate_kbps = bitrate, .meta = meta.p};
    std::string label;
    if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }
    const auto unit = ac3::eac3::build_silent_access_unit(plan::eac3_config(p));
    if (!unit) {
        std::println(stderr, "error: invalid E-AC-3 configuration");
        return 1;
    }
    const std::uint64_t count = frame_count(seconds);
    if (!write_repeated_frame(out_path, unit->bytes, count)) {
        return 1;
    }
    std::println("wrote {} silent E-AC-3 {} access units ({} substreams, "
                 "{} bytes each, bsid 16) to {}",
                 count, label, unit->substream_count(), unit->bytes.size(), out_path);
    return 0;
}

}  // namespace ac3cli::commands
