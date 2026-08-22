#include "ac3/encoder/assignment.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
            return channel < sources[s].channels ? std::optional(offset + channel) : std::nullopt;
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

namespace {

// dB to linear amplitude, for a Destination::trim_db riding a Routing gain
// entry that would otherwise be unity.
[[nodiscard]] double linear_gain(double trim_db) {
    return std::pow(10.0, trim_db / 20.0);
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
            const auto gain = linear_gain(dest.trim_db);
            bool found = false;
            for (std::size_t coded_index = 0; coded_index < coded.size(); ++coded_index) {
                if (coded[coded_index].location != dest.location) {
                    continue;
                }
                // c < sources[s].channels by the loop bound above, and that is
                // flat_index's only nullopt case (see its comment) - flat is
                // always engaged here. clang-tidy can't see across the loop
                // condition into the callee, hence the suppression.
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                out.gain[coded_index * total + *flat] = gain;
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
    out.gain[0 * total + *i1] = linear_gain(assignment.at(p1[0].first, p1[0].second).trim_db);
    out.gain[1 * total + *i2] = linear_gain(assignment.at(p2[0].first, p2[0].second).trim_db);
    return out;
}

Codec derive_codec(const ChannelPlan& target, const Tools& tools, const Metadata& meta,
                   const std::optional<eac3::VbrConfig>& vbr, SampleRate sample_rate) {
    const bool needs_eac3 = !target.dependents.empty() || vbr.has_value() || tools.any() ||
                            meta.mixmeta || is_reduced_rate(sample_rate);
    return needs_eac3 ? Codec::kEac3 : Codec::kAc3;
}

namespace {

// dB range a trim can express - see Destination::trim_db's own comment.
constexpr double kMinTrimDb = -24.0;
constexpr double kMaxTrimDb = 24.0;

// Snaps to a tenth-of-a-dB grid and clamps to [kMinTrimDb, kMaxTrimDb].
// A trim is a coarse control (a GUI spinner, not a mixing console), and a
// fixed decimal grid is what lets format_destination/parse_destination
// round-trip a trim EXACTLY: an arbitrary double would need up to 17
// significant decimal digits to survive a round trip losslessly, which
// "L@-3.5" is deliberately not trying to be. format_trim() below reprints
// from the same integer-tenths value this derives, never from trim_db
// directly, so the two stay in lockstep.
[[nodiscard]] double snap_trim(double trim_db) {
    const double clamped = std::clamp(trim_db, kMinTrimDb, kMaxTrimDb);
    return static_cast<double>(std::llround(clamped * 10.0)) / 10.0;
}

// Not std::from_chars: its floating-point overload is absent from some
// libc++ builds this project targets (see parse_unit_double's identical
// note in plan.cpp), so this hand-rolls the same locale-independent,
// reject-all-trailing-garbage contract with strtod instead. strtod itself
// is locale-sensitive; nothing in this codebase ever calls setlocale, so
// the process locale stays "C" for its entire lifetime and this is safe in
// practice, not just in theory.
[[nodiscard]] bool parse_trim(std::string_view text, double& out) {
    if (text.empty()) {
        return false;
    }
    const std::string buffer(text);
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size() || errno == ERANGE) {
        return false;
    }
    // A little slack before rejecting outright: snap_trim's own clamp would
    // silently absorb a small overshoot, but a token wildly outside the
    // documented range (e.g. a stray "@240") is more likely a caller
    // mistake than a trim - reject rather than silently clamp that far.
    if (value < kMinTrimDb - 1.0 || value > kMaxTrimDb + 1.0) {
        return false;
    }
    out = snap_trim(value);
    return true;
}

// Prints a trim already on the tenth-of-a-dB grid (see snap_trim) as the
// shortest exact decimal - a whole-dB trim reads as "-3", a fractional one
// as "-3.5" - built from the same integer-tenths value snap_trim derives,
// never printed straight from the double, so format_trim(snap_trim(x)) and
// parse_trim's own snap_trim(strtod(...)) always agree exactly.
[[nodiscard]] std::string format_trim(double trim_db) {
    const auto tenths = std::llround(trim_db * 10.0);
    const auto whole = tenths / 10;
    const auto frac = std::abs(tenths % 10);
    std::string out;
    if (tenths < 0 && whole == 0) {
        out += '-';  // e.g. -0.5: whole truncates to 0 and loses the sign
    }
    out += std::to_string(whole);
    if (frac != 0) {
        out += '.';
        out += static_cast<char>('0' + frac);
    }
    return out;
}

}  // namespace

std::string format_destination(Destination dest) {
    std::string out;
    switch (dest.kind) {
        case DestinationKind::kUnassigned:
            out = "none";
            break;
        case DestinationKind::kLocation:
            out = std::string{eac3::chanmap::name(dest.location)};
            break;
        case DestinationKind::kObject:
            out = "obj";
            break;
        case DestinationKind::kObjectMono:
            out = "objm";
            break;
        case DestinationKind::kProgramme1:
            out = "p1";
            break;
        case DestinationKind::kProgramme2:
            out = "p2";
            break;
    }
    if (dest.trim_db != 0.0) {
        out += '@';
        out += format_trim(dest.trim_db);
    }
    return out;
}

std::optional<Destination> parse_destination(std::string_view token) {
    // The trim suffix, if any, splits off first - everything before '@' (or
    // the whole token, if there is none) is the base destination spelling.
    const auto at = token.find('@');
    const auto base = token.substr(0, at);

    Destination dest;
    if (base == "none") {
        dest = Destination{};
    } else if (base == "obj") {
        dest = Destination{.kind = DestinationKind::kObject};
    } else if (base == "objm") {
        dest = Destination{.kind = DestinationKind::kObjectMono};
    } else if (base == "p1") {
        dest = Destination{.kind = DestinationKind::kProgramme1};
    } else if (base == "p2") {
        dest = Destination{.kind = DestinationKind::kProgramme2};
    } else if (const auto location = eac3::chanmap::parse_location(base)) {
        dest = Destination{.kind = DestinationKind::kLocation, .location = *location};
    } else {
        return std::nullopt;
    }

    if (at != std::string_view::npos) {
        double trim = 0.0;
        if (!parse_trim(token.substr(at + 1), trim)) {
            return std::nullopt;
        }
        dest.trim_db = trim;
    }
    return dest;
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
            // regardless (or, for objm, the whole range folds to ONE
            // identity together - still no single channel it "means"), and
            // "none" does not care how many there are. A location or a
            // programme names exactly one channel, so a range there would
            // be ambiguous about which channel it actually means.
            if ((dest->kind != DestinationKind::kObject &&
                 dest->kind != DestinationKind::kObjectMono &&
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
