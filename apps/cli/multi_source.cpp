#include "multi_source.hpp"

#include <algorithm>
#include <cmath>
#include <print>

#include "support.hpp"

namespace ac3cli {

namespace plan = ac3::plan;

std::size_t offset_samples_for(std::span<const std::pair<std::size_t, double>> offsets,
                               std::size_t index, std::uint32_t sample_rate) {
    std::size_t result = 0;
    for (const auto& [i, seconds] : offsets) {
        if (i == index) {
            result = static_cast<std::size_t>(std::lround(seconds * static_cast<double>(sample_rate)));
        }
    }
    return result;
}

std::optional<LoadedSources> load_sources(
    std::string_view in_path, std::span<const std::string> extra,
    std::span<const std::pair<std::size_t, double>> offsets) {
    auto primary = ac3::io::read_wav(std::string{in_path});
    if (!primary) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(primary.error()));
        return std::nullopt;
    }
    LoadedSources out;
    out.sample_rate = primary->sample_rate;
    out.shapes.push_back({.channels = primary->channels.size(), .label = std::string{in_path}});
    out.wavs.push_back(std::move(*primary));

    for (const auto& path : extra) {
        auto wav = ac3::io::read_wav(path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", path, ac3::io::describe(wav.error()));
            return std::nullopt;
        }
        if (wav->sample_rate != out.sample_rate) {
            std::println(stderr,
                         "error: {} is {} Hz, but {} is {} Hz - every source must share a "
                         "sample rate",
                         path, wav->sample_rate, in_path, out.sample_rate);
            return std::nullopt;
        }
        out.shapes.push_back({.channels = wav->channels.size(), .label = path});
        out.wavs.push_back(std::move(*wav));
    }

    // A sourceIndex offset= names beyond how many sources actually loaded has
    // nothing to shift - ignored rather than an error, the same way an unused
    // trailing option elsewhere in this file is simply inert.
    out.offset_samples.resize(out.wavs.size());
    for (std::size_t i = 0; i < out.wavs.size(); ++i) {
        out.offset_samples[i] = offset_samples_for(offsets, i, out.sample_rate);
    }
    out.total_frames = 0;
    for (std::size_t i = 0; i < out.wavs.size(); ++i) {
        out.total_frames =
            std::max(out.total_frames, out.offset_samples[i] + out.wavs[i].frame_count());
    }
    return out;
}

std::optional<plan::Routing> routing_for_sources(const plan::Plan& p, const LoadedSources& sources,
                                                 const std::optional<std::string>& map_spec) {
    if (!map_spec) {
        if (sources.shapes.size() > 1) {
            std::println(stderr,
                         "error: more than one source needs map= to say where each channel "
                         "goes ({})",
                         plan::kAssignmentSyntax);
            return std::nullopt;
        }
        return routing_or_error(p, sources.shapes.front().channels);
    }
    plan::Assignment assignment;
    if (!plan::parse_assignment(*map_spec, sources.shapes, assignment)) {
        std::println(stderr, "error: bad map= spec ({})", plan::kAssignmentSyntax);
        return std::nullopt;
    }
    const auto target = plan::resolve(p);
    const bool dual_mono = target.bed_acmod == ac3::Acmod::kDualMono;
    if (!dual_mono) {
        // route() (below) only carries kLocation rows into the output - see
        // its own comment. obj/objm reach it here because this CLI has no
        // object-assembly path of its own (that is the GUI's, see
        // encoder_controller.cpp's encodeObjects); p1/p2 reach it only if a
        // caller wrote them for a target that isn't dual mono, so route()
        // would drop those too, for lack of anywhere to route them to. Either
        // way, a channel silently contributing nothing is worth a warning
        // rather than a surprise in the output.
        for (const auto kind : {plan::DestinationKind::kObject, plan::DestinationKind::kObjectMono,
                                plan::DestinationKind::kProgramme1,
                                plan::DestinationKind::kProgramme2}) {
            for (const auto& [s, c] : assignment.rows_of(kind)) {
                std::println(stderr,
                             "warning: {}.{} maps to '{}', which this command has no way to "
                             "carry - that channel contributes nothing to the output",
                             s, c, plan::format_destination(assignment.at(s, c)));
            }
        }
    }
    auto routing = dual_mono ? plan::dual_mono_routing(sources.shapes, assignment)
                             : plan::route(target, sources.shapes, assignment);
    if (!routing) {
        std::println(stderr, "error: map= does not resolve to a valid routing for this format");
        return std::nullopt;
    }
    return routing;
}

void gather_frame(const LoadedSources& sources, std::size_t start,
                  std::vector<std::vector<float>>& dest) {
    std::size_t flat = 0;
    for (std::size_t s = 0; s < sources.wavs.size(); ++s) {
        const auto& wav = sources.wavs[s];
        const std::size_t total = wav.frame_count();
        const std::size_t offset = sources.offset_samples[s];
        for (const auto& channel : wav.channels) {
            const float hold = total > 0 ? channel[total - 1] : 0.0f;
            auto& out = dest[flat];
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                if (at < offset) {
                    out[static_cast<std::size_t>(i)] = 0.0f;
                    continue;
                }
                const std::size_t shifted = at - offset;
                out[static_cast<std::size_t>(i)] = shifted < total ? channel[shifted] : hold;
            }
            ++flat;
        }
    }
}

void print_routing(const plan::Plan& p, const plan::Routing& routing, std::string_view label,
                   FILE* out) {
    if (routing.is_permutation()) {
        std::println(out, "  source carried directly into {}", label);
        return;
    }
    const auto names = plan::coded_channel_names(plan::resolve(p));
    std::string silent;
    for (int c = 0; c < routing.coded_channels; ++c) {
        bool fed = false;
        for (int s = 0; s < routing.source_channels && !fed; ++s) {
            fed = routing.at(c, s) != 0.0;
        }
        if (!fed) {
            silent += silent.empty() ? "" : " ";
            silent += names[static_cast<std::size_t>(c)];
        }
    }
    std::println(out, "  {} source channels rendered onto {}", routing.source_channels, label);
    if (!silent.empty()) {
        std::println(out, "  silent (the source carries nothing that belongs there): {}", silent);
    }
}

}  // namespace ac3cli
