#include "encode.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "../multi_source.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

namespace {

// Reports a bad tool token against the syntax, the same way a bad layout is
// reported against the layout list.
bool tools_or_error(std::string_view text, plan::Tools& out) {
    if (plan::parse_tools(text, out)) {
        return true;
    }
    std::println(stderr, "error: unknown tool set '{}' ({})", text, plan::kToolsSyntax);
    return false;
}

// As above, for the vbr argument.
bool vbr_or_error(std::string_view text, std::optional<ac3::eac3::VbrConfig>& out) {
    if (plan::parse_vbr(text, out)) {
        return true;
    }
    std::println(stderr, "error: unrecognised vbr setting '{}' ({})", text, plan::kVbrSyntax);
    return false;
}

}  // namespace

int run_eac3_encode_multi(std::string_view in_path, std::string_view out_path,
                          std::uint32_t bitrate, std::string_view tools,
                          std::string_view layout, std::string_view vbr, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources) {
        return 1;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kEac3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        std::size_t total_channels = 0;
        for (const auto& shape : sources->shapes) {
            total_channels += shape.channels;
        }
        const auto id = plan::layout_for_source(total_channels);
        if (!id) {
            std::println(stderr, "error: {} channels - {}", total_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return 1;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing) {
        return 1;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // Renders one frame's worth of every source's samples onto the coded
    // channels `out`/`views` alias - shared by the measurement pre-pass below
    // and the real encode loop after it, so the two can never render this
    // programme two different ways.
    auto route_frame = [&](std::size_t start) {
        gather_frame(*sources, start, source);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
    };

    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    const bool want_dialnorm = p.meta.measure_dialnorm;
    // dialnorm2 only means anything under 1+1 - silently inert otherwise,
    // exactly like run_eac3_encode's identical check for its one file.
    const bool want_dialnorm2 = dual_mono && p.meta.measure_dialnorm2;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (want_dialnorm || want_dialnorm2) {
        // §5.4.2.8's BS.1770 pass has to measure what the encoder actually
        // receives - the routed/rendered coded channels, not each source's
        // own raw layout, since map= can permute, trim or fold several
        // sources onto them - so this renders the entire programme once
        // purely to measure it. Dual mono gets one single-channel meter per
        // programme (Ch1/Ch2 are unrelated, §E1.3 - see
        // measured_dialnorm_channel's own comment); every other target gets
        // one whole-programme meter, the same BS.1770 channel weighting
        // measured_dialnorm uses for the single-file case.
        std::optional<ac3::meta::LoudnessMeter> whole;
        std::optional<ac3::meta::LoudnessMeter> ch1;
        std::optional<ac3::meta::LoudnessMeter> ch2;
        if (dual_mono) {
            if (want_dialnorm) {
                ch1.emplace(*sr, ac3::Acmod::k1_0, false);
            }
            if (want_dialnorm2) {
                ch2.emplace(*sr, ac3::Acmod::k1_0, false);
            }
        } else if (want_dialnorm) {
            whole.emplace(*sr, cp.bed_acmod, cp.bed_lfe);
        }
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole) {
                whole->push(views);
            }
            if (ch1) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
        }
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured) {
                std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2) {
                std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return 1;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    assert(static_cast<int>(nchans) == encoder.channel_count());
    // Streamed out as encoded, exactly as run_eac3_encode below - the
    // multi-source shape only differs on the INPUT side (route_frame over
    // whole sources), not in what leaves.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        route_frame(start);
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: the encoder cannot express this configuration");
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
    if (p.vbr) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number.
        const double mean_bytes = out_sink.frames() == 0
                                      ? 0.0
                                      : static_cast<double>(out_sink.total_bytes()) /
                                            static_cast<double>(out_sink.frames());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(sources->sample_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        std::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), plan::format_vbr(p.vbr), sources->sample_rate, label,
                     nchans, plan::format_tools(p.tools), out_path);
        std::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     out_sink.min_bytes(), out_sink.max_bytes(), mean_bytes, mean_kbps);
    } else {
        std::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), bitrate, sources->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label, status);
    return 0;
}

int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools, std::string_view layout,
                    std::string_view vbr, const Options& meta,
                    std::string_view in2_path) {
    if (!meta.sources.empty() || meta.map_spec) {
        if (!in2_path.empty()) {
            std::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return 1;
        }
        return run_eac3_encode_multi(in_path, out_path, bitrate, tools, layout, vbr, meta);
    }
    // The same streaming-vs-whole-file split as run_encode, for the same
    // reasons - see its comment. A failed open falls through so read_wav_arg
    // produces the error message it always has.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           !meta.p.measure_dialnorm2 && in2_path.empty() &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return 1;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        std::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return 1;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kEac3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        // An unnamed layout follows the source, which is what this command
        // did before it could be told otherwise.
        const auto id = plan::layout_for_source(src_channels);
        if (!id) {
            std::println(stderr, "error: {} channels - {}", src_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return 1;
    }
    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm", status)
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe, status);
        if (!measured) {
            std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2) {
            std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing) {
        return 1;
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    assert(static_cast<int>(nchans) == encoder.channel_count());
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    const std::size_t total = offset + frame_count;

    std::vector<std::vector<float>> source(src_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // The encoded access units leave as they are produced - see
    // EncodedStreamSink; its stats also feed the VBR report below, which
    // used to re-walk the whole frame list for them.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    // See run_encode's identical streaming state: the loop consumes the
    // source strictly in order, so a rolling read position and the last
    // real sample per channel are all the streaming path needs.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // Hold the last real sample past end-of-file rather than dropping to
        // hard zero - see run_encode's identical padding for why: a sudden
        // drop to silence is itself a transient the encoder would (correctly)
        // spend a block-switch on, for a discontinuity that only exists
        // because this frame ends mid-buffer. Ahead of the source's own
        // samples, offset= silence is real silence, not padding.
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, ac3::kSamplesPerFrame)
                               : 0;
            std::size_t want = 0;
            if (lead < ac3::kSamplesPerFrame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(ac3::kSamplesPerFrame - lead, remaining);
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                std::fill_n(source[c].begin(), lead, 0.0f);
                stream_dst[c] = std::span{source[c]}.subspan(lead, want);
            }
            if (want > 0) {
                const auto got = stream_in.read_planar(stream_dst, want);
                if (!got || *got != want) {
                    std::println(stderr, "error: {}: {}", in_path,
                                 ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                       : got.error()));
                    out_sink.abort();
                    return 1;
                }
                consumed += want;
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                if (want > 0) {
                    stream_hold[c] = source[c][lead + want - 1];
                }
                std::fill(source[c].begin() + static_cast<std::ptrdiff_t>(lead + want),
                          source[c].end(), stream_hold[c]);
                in[c] = source[c];
            }
        } else {
            for (std::size_t c = 0; c < source.size(); ++c) {
                const float hold = frame_count > 0 ? wav->channels[c][frame_count - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    if (at < offset) {
                        source[c][static_cast<std::size_t>(i)] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][static_cast<std::size_t>(i)] =
                        shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: the encoder cannot express this configuration");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(unit->bytes)) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    if (p.vbr) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number:
        // the sink kept the tally as the units streamed out.
        const double mean_bytes = out_sink.frames() == 0
                                      ? 0.0
                                      : static_cast<double>(out_sink.total_bytes()) /
                                            static_cast<double>(out_sink.frames());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(src_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        std::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), plan::format_vbr(p.vbr), src_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
        std::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     out_sink.min_bytes(), out_sink.max_bytes(), mean_bytes, mean_kbps);
    } else {
        std::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), bitrate, src_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label, status);
    return 0;
}

int run_encode_multi(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                     bool couple, std::string_view layout, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources) {
        return 1;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "AC-3", false);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kAc3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        std::size_t total_channels = 0;
        for (const auto& shape : sources->shapes) {
            total_channels += shape.channels;
        }
        const auto id = plan::layout_for_source(total_channels);
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            std::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         total_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        std::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing) {
        return 1;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // Renders one frame's worth of every source's samples onto the coded
    // channels `out`/`views` alias - shared by the measurement pre-pass below
    // and the real encode loop after it, so the two can never render this
    // programme two different ways.
    auto route_frame = [&](std::size_t start) {
        gather_frame(*sources, start, source);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
    };

    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    const bool want_dialnorm = p.meta.measure_dialnorm;
    // dialnorm2 only means anything under 1+1 - silently inert otherwise,
    // exactly like run_encode's identical check for its one file.
    const bool want_dialnorm2 = dual_mono && p.meta.measure_dialnorm2;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes this function writes below already own stdout in that
    // case, and no human-readable report (the dialnorm=auto measurement just
    // below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (want_dialnorm || want_dialnorm2) {
        // §5.4.2.8's BS.1770 pass has to measure what the encoder actually
        // receives - the routed/rendered coded channels, not each source's
        // own raw layout, since map= can permute, trim or fold several
        // sources onto them - so this renders the entire programme once
        // purely to measure it. Dual mono gets one single-channel meter per
        // programme (Ch1/Ch2 are unrelated, §E1.3 - see
        // measured_dialnorm_channel's own comment); every other target gets
        // one whole-programme meter, the same BS.1770 channel weighting
        // measured_dialnorm uses for the single-file case.
        std::optional<ac3::meta::LoudnessMeter> whole;
        std::optional<ac3::meta::LoudnessMeter> ch1;
        std::optional<ac3::meta::LoudnessMeter> ch2;
        if (dual_mono) {
            if (want_dialnorm) {
                ch1.emplace(*sr, ac3::Acmod::k1_0, false);
            }
            if (want_dialnorm2) {
                ch2.emplace(*sr, ac3::Acmod::k1_0, false);
            }
        } else if (want_dialnorm) {
            whole.emplace(*sr, cp.bed_acmod, cp.bed_lfe);
        }
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole) {
                whole->push(views);
            }
            if (ch1) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
        }
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured) {
                std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2) {
                std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return 1;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    const auto config = plan::ac3_config(p);
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, sources->sample_rate};
    // Streamed out as encoded, exactly as run_encode below - see
    // run_eac3_encode_multi's identical note.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        route_frame(start);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
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
    std::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", out_sink.frames(),
                 bitrate, sources->sample_rate,
                 ac3::analysis::layout_name(config.acmod, config.lfe), out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return 0;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple, std::string_view layout, const Options& meta,
               std::string_view in2_path) {
    if (!meta.sources.empty() || meta.map_spec) {
        if (!in2_path.empty()) {
            std::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return 1;
        }
        return run_encode_multi(in_path, out_path, bitrate, couple, layout, meta);
    }
    // A seekable file whose whole-programme passes are not needed - no
    // dialnorm=auto BS.1770 measurement, no second dual-mono file to merge -
    // streams one frame-sized block at a time, holding ~40 KB of samples
    // resident instead of the whole file plus its planar float copy (the
    // measured peak was linear in duration before this: 152 MiB for a 60 s
    // 5.1 encode, 438 MiB for 180 s). Everything else takes the whole-file
    // read below, unchanged - including a failed open, which falls through
    // so read_wav_arg can produce the error message it always has.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           !meta.p.measure_dialnorm2 && in2_path.empty() &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return 1;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        // The same refusal prepare_dual_mono_source gives the one-file 1+1
        // case; the streaming path validates off the header instead.
        std::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return 1;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "AC-3", false);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kAc3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        // An unnamed layout follows the source, which is what this command
        // did before it could be told otherwise. Naming one is how a stereo
        // file reaches a 5.1 stream, or a 5.1 file gets folded down per §7.8.
        const auto id = plan::layout_for_source(src_channels);
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            std::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         src_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        std::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    // §5.4.2.8 says dialnorm "shall affect the sound reproduction level", so
    // getting it wrong is not a cosmetic error - a stream that claims 31 when
    // dialogue is really at -18 plays 13 dB too loud on a levelled system.
    // Measuring needs the whole programme (the BS.1770 relative gate does),
    // which is why it happens here rather than inside the frame encoder. It
    // gets the OUTPUT layout, because the BS.1770 channel weighting depends on
    // which coded positions are surrounds.
    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes this function writes below already own stdout in that
    // case, and no human-readable report (the dialnorm=auto measurement just
    // below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm", status)
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe, status);
        if (!measured) {
            std::println(stderr,
                         "error: {}no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2) {
            std::println(stderr,
                         "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing) {
        return 1;
    }

    const auto config = plan::ac3_config(p);
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, src_rate};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    const std::size_t total = offset + frame_count;

    std::vector<std::vector<float>> source(src_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // The encoded frames leave as they are produced - see EncodedStreamSink
    // for how a failed run still honours keep-partial exactly as the old
    // accumulate-then-write shape did.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    // The streaming path's own state: the frame loop below asks for the
    // source's samples strictly in order (offset= only ever shifts where
    // they land inside a frame, never which come next), so a rolling read
    // position plus the last real sample per channel - for the same
    // hold-padding the whole-file path applies - is all it takes.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // The tail frame is padded to a full 1536 samples; the meter sees only
        // the real ones, so the padding cannot pull the RMS down. Padding
        // holds the last real sample rather than dropping to hard zero: a
        // sudden drop to silence is itself a transient, and the encoder's own
        // §8.2.2 detector would (correctly) spend a block-switch on it,
        // paying real side-info bits to preserve a discontinuity that exists
        // only because this frame ends mid-buffer, not in the source audio.
        // Ahead of the source's own samples, offset= silence is real
        // silence, not padding.
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, ac3::kSamplesPerFrame)
                               : 0;
            std::size_t want = 0;
            if (lead < ac3::kSamplesPerFrame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(ac3::kSamplesPerFrame - lead, remaining);
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                std::fill_n(source[c].begin(), lead, 0.0f);
                stream_dst[c] = std::span{source[c]}.subspan(lead, want);
            }
            if (want > 0) {
                const auto got = stream_in.read_planar(stream_dst, want);
                if (!got || *got != want) {
                    std::println(stderr, "error: {}: {}", in_path,
                                 ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                       : got.error()));
                    out_sink.abort();
                    return 1;
                }
                consumed += want;
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                if (want > 0) {
                    stream_hold[c] = source[c][lead + want - 1];
                }
                std::fill(source[c].begin() + static_cast<std::ptrdiff_t>(lead + want),
                          source[c].end(), stream_hold[c]);
                in[c] = source[c];
            }
        } else {
            for (std::size_t c = 0; c < source.size(); ++c) {
                const float hold = frame_count > 0 ? wav->channels[c][frame_count - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    if (at < offset) {
                        source[c][static_cast<std::size_t>(i)] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][static_cast<std::size_t>(i)] =
                        shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(*frame)) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    std::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", out_sink.frames(),
                bitrate, src_rate, ac3::analysis::layout_name(config.acmod, config.lfe),
                out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return 0;
}

}  // namespace ac3cli::commands
