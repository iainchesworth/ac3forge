#include "ac3/encoder/assignment.hpp"

#include <algorithm>
#include <charconv>

namespace ac3::plan {

void Assignment::set(std::size_t source, std::size_t channel, Destination dest) {
    if (dest.kind == DestinationKind::kUnassigned) {
        rows_.erase({source, channel});
        return;
    }
    rows_[{source, channel}] = dest;
}

Destination Assignment::at(std::size_t source, std::size_t channel) const {
    const auto it = rows_.find({source, channel});
    return it == rows_.end() ? Destination{} : it->second;
}

std::vector<std::pair<std::size_t, std::size_t>> Assignment::unassigned(
    std::span<const SourceShape> sources) const {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t s = 0; s < sources.size(); ++s) {
        for (std::size_t c = 0; c < sources[s].channels; ++c) {
            if (at(s, c).kind == DestinationKind::kUnassigned) {
                out.emplace_back(s, c);
            }
        }
    }
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>> Assignment::rows_of(DestinationKind kind) const {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    // rows_ is a std::map keyed on (source, channel), so this is already in
    // (source, then channel) order - nothing to sort.
    for (const auto& [key, dest] : rows_) {
        if (dest.kind == kind) {
            out.push_back(key);
        }
    }
    return out;
}

namespace {

// Flat index of (source, channel) into the concatenated span route()/
// render() expect: source 0's channels first, then source 1's, and so on.
// nullopt only when `channel` is out of range for `source` - `source` itself
// is trusted, since every caller below gets it from iterating `sources`.
[[nodiscard]] std::optional<std::size_t> flat_index(std::span<const SourceShape> sources,
                                                     std::size_t source, std::size_t channel) {
    std::size_t offset = 0;
    for (std::size_t s = 0; s < sources.size(); ++s) {
        if (s == source) {
            return channel < sources[s].channels ? std::optional(offset + channel)
                                                  : std::nullopt;
        }
        offset += sources[s].channels;
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t total_channels(std::span<const SourceShape> sources) {
    std::size_t total = 0;
    for (const auto& source : sources) {
        total += source.channels;
    }
    return total;
}

}  // namespace

std::optional<Routing> route(const ChannelPlan& target, std::span<const SourceShape> sources,
                             const Assignment& assignment) {
    if (sources.empty()) {
        return std::nullopt;
    }
    const std::size_t total = total_channels(sources);
    if (total == 0) {
        return std::nullopt;
    }
    const auto coded = coded_channels(target);
    Routing out{.source_channels = static_cast<int>(total),
                .coded_channels = static_cast<int>(coded.size()),
                .gain = std::vector<double>(total * coded.size(), 0.0)};

    // Locations a row has already claimed - a second row aimed at the same
    // location is a caller error (which of two channels is truth?), not a
    // sum, unlike a bed/dependent PAIR that legitimately share one location
    // and both take gain from the SAME row below.
    std::vector<eac3::chanmap::Location> claimed;

    for (std::size_t s = 0; s < sources.size(); ++s) {
        for (std::size_t c = 0; c < sources[s].channels; ++c) {
            const auto dest = assignment.at(s, c);
            if (dest.kind != DestinationKind::kLocation) {
                continue;
            }
            if (std::ranges::find(claimed, dest.location) != claimed.end()) {
                return std::nullopt;  // two rows targeting the same location
            }
            const auto flat = flat_index(sources, s, c);
            bool found = false;
            for (std::size_t coded_index = 0; coded_index < coded.size(); ++coded_index) {
                if (coded[coded_index].location != dest.location) {
                    continue;
                }
                out.gain[coded_index * total + *flat] = 1.0;
                found = true;
            }
            if (!found) {
                return std::nullopt;  // target cannot express this location
            }
            claimed.push_back(dest.location);
        }
    }
    return out;
}

std::optional<Routing> dual_mono_routing(std::span<const SourceShape> sources,
                                         const Assignment& assignment) {
    const auto p1 = assignment.rows_of(DestinationKind::kProgramme1);
    const auto p2 = assignment.rows_of(DestinationKind::kProgramme2);
    if (p1.size() != 1 || p2.size() != 1) {
        return std::nullopt;
    }
    const auto i1 = flat_index(sources, p1[0].first, p1[0].second);
    const auto i2 = flat_index(sources, p2[0].first, p2[0].second);
    if (!i1 || !i2) {
        return std::nullopt;
    }
    const auto total = total_channels(sources);
    Routing out{.source_channels = static_cast<int>(total),
                .coded_channels = 2,
                .gain = std::vector<double>(total * 2, 0.0)};
    out.gain[0 * total + *i1] = 1.0;
    out.gain[1 * total + *i2] = 1.0;
    return out;
}

Codec derive_codec(const ChannelPlan& target, const Tools& tools, const Metadata& meta,
                   const std::optional<eac3::VbrConfig>& vbr, SampleRate sample_rate) {
    const bool needs_eac3 = !target.dependents.empty() || vbr.has_value() || tools.any() ||
                            meta.mixmeta || is_reduced_rate(sample_rate);
    return needs_eac3 ? Codec::kEac3 : Codec::kAc3;
}

std::string format_destination(Destination dest) {
    switch (dest.kind) {
        case DestinationKind::kUnassigned: return "none";
        case DestinationKind::kLocation: return std::string{eac3::chanmap::name(dest.location)};
        case DestinationKind::kObject: return "obj";
        case DestinationKind::kProgramme1: return "p1";
        case DestinationKind::kProgramme2: return "p2";
    }
    return "none";  // unreachable; every DestinationKind is handled above
}

std::optional<Destination> parse_destination(std::string_view token) {
    if (token == "none") {
        return Destination{};
    }
    if (token == "obj") {
        return Destination{.kind = DestinationKind::kObject};
    }
    if (token == "p1") {
        return Destination{.kind = DestinationKind::kProgramme1};
    }
    if (token == "p2") {
        return Destination{.kind = DestinationKind::kProgramme2};
    }
    if (const auto location = eac3::chanmap::parse_location(token)) {
        return Destination{.kind = DestinationKind::kLocation, .location = *location};
    }
    return std::nullopt;
}

namespace {

[[nodiscard]] bool parse_size(std::string_view text, std::size_t& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

bool parse_assignment(std::string_view text, std::span<const SourceShape> sources,
                      Assignment& out) {
    if (text.empty()) {
        return false;
    }
    // Coverage tracked locally, not in `out` - Assignment cannot tell an
    // explicit "none" apart from a channel nobody mentioned (both are simply
    // absent from its table), so completeness is checked here instead, while
    // there is still a token to blame.
    std::vector<std::vector<bool>> covered(sources.size());
    for (std::size_t s = 0; s < sources.size(); ++s) {
        covered[s].resize(sources[s].channels, false);
    }

    while (!text.empty()) {
        const auto split = text.find(',');
        const auto entry = text.substr(0, split);
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
        if (entry.empty()) {
            return false;
        }

        const auto colon = entry.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const auto address = entry.substr(0, colon);
        const auto dest = parse_destination(entry.substr(colon + 1));
        if (!dest) {
            return false;
        }

        const auto dot = address.find('.');
        std::size_t source_index = 0;
        if (dot == std::string_view::npos || !parse_size(address.substr(0, dot), source_index) ||
            source_index >= sources.size()) {
            return false;
        }

        const auto channel_text = address.substr(dot + 1);
        const auto dash = channel_text.find('-');
        std::size_t first = 0;
        std::size_t last = 0;
        if (dash == std::string_view::npos) {
            if (!parse_size(channel_text, first)) {
                return false;
            }
            last = first;
        } else {
            // A range only makes sense for a destination with no notion of
            // "which one": an object gets its own identity per channel
            // regardless, and "none" does not care how many there are. A
            // location or a programme names exactly one channel, so a range
            // there would be ambiguous about which channel it actually means.
            if ((dest->kind != DestinationKind::kObject &&
                 dest->kind != DestinationKind::kUnassigned) ||
                !parse_size(channel_text.substr(0, dash), first) ||
                !parse_size(channel_text.substr(dash + 1), last) || last < first) {
                return false;
            }
        }
        if (last >= sources[source_index].channels) {
            return false;
        }
        for (std::size_t c = first; c <= last; ++c) {
            if (covered[source_index][c]) {
                return false;  // this channel already has a token
            }
            covered[source_index][c] = true;
            out.set(source_index, c, *dest);
        }
    }

    for (const auto& source_coverage : covered) {
        if (std::ranges::find(source_coverage, false) != source_coverage.end()) {
            return false;  // a loaded channel with no token at all
        }
    }
    return true;
}

std::string format_assignment(std::span<const SourceShape> sources,
                              const Assignment& assignment) {
    std::string out;
    for (std::size_t s = 0; s < sources.size(); ++s) {
        for (std::size_t c = 0; c < sources[s].channels; ++c) {
            if (!out.empty()) {
                out += ',';
            }
            out += std::to_string(s);
            out += '.';
            out += std::to_string(c);
            out += ':';
            out += format_destination(assignment.at(s, c));
        }
    }
    return out;
}

}  // namespace ac3::plan
