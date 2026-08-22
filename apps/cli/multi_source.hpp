#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"

// The src=/map= multi-source subsystem: the primary positional file plus every src= path, opened
// and shaped, then routed as one programme (routing_for_sources) and gathered frame-by-frame
// (gather_frame) with each source's own offset= leading silence applied independently. Split out
// of main.cpp as step 2 of the repo-structure review's H4 monolith split - see support.hpp's own
// top comment for step 1 and the overall plan.
//
// Deliberately NOT unified with the classic single-file/in2.wav path (which stays in main.cpp,
// untouched): the two have genuinely different data shapes (one ac3::io::WavData vs several), and
// duplicating the small amount that does overlap costs far less than a shared abstraction would
// risk - see this header's own struct/function comments, carried over verbatim from main.cpp.
namespace ac3cli {

struct LoadedSources {
    std::vector<ac3::io::WavData> wavs;
    std::vector<ac3::plan::SourceShape> shapes;
    std::uint32_t sample_rate = 0;
    // Per-source leading silence, in samples at sample_rate - parallel to
    // wavs/shapes (index 0 = in_path, 1..N = each src= in load order, the
    // same numbering offset= addresses), computed from Options::offsets once
    // sample_rate is known. Applied in gather_frame, ahead of a source's own
    // samples.
    std::vector<std::size_t> offset_samples;
    // The longest source's frame count once ITS OWN offset is applied, not
    // the longest raw source alone - a run covers everything any loaded
    // source carries, including whatever offset= shifted it by. Each source
    // still holds its own last real sample past its own end (see
    // gather_frame), so a short source does not go silent early relative to
    // a long one.
    std::size_t total_frames = 0;
};

// The leading-silence sample count offset= asked for on source `index`
// (the same 0-based numbering src= establishes), from every offset= the
// operator gave - the last occurrence for that index wins, since
// parse_options does not dedupe. Shared by load_sources (every loaded
// source) and the classic single-file path (source index always 0).
std::size_t offset_samples_for(std::span<const std::pair<std::size_t, double>> offsets,
                               std::size_t index, std::uint32_t sample_rate);

// Opens `in_path` plus every path in `extra`, in that order, and checks they
// all share one sample rate - plan::render has no notion of resampling, and
// a silently mismatched pair would drift apart rather than error.
std::optional<LoadedSources> load_sources(
    std::string_view in_path, std::span<const std::string> extra,
    std::span<const std::pair<std::size_t, double>> offsets);

// The routing for a loaded, possibly multi-source run: explicit assignment
// when map= was given, else exactly routing_or_error's single-source
// automatic panning - map= is opt-in, so omitting it is defined to behave
// exactly as if src=/map= did not exist at all. Dual mono is routed through
// dual_mono_routing rather than the general location-based route(): a 1+1
// target has no soundstage for a location token to mean anything on, so
// map= for it names programmes (p1/p2), not locations.
std::optional<ac3::plan::Routing> routing_for_sources(
    const ac3::plan::Plan& p, const LoadedSources& sources,
    const std::optional<std::string>& map_spec);

// Fills `dest` (one entry per flattened source channel, source 0 first) with
// samples [start, start + kSamplesPerFrame) from `sources`, applying each
// source's own offset= leading silence ahead of its real samples, then
// holding its own last real sample past its own end - independently per
// source, the same tail padding the classic path already applies to its one
// file, so a short source loaded alongside a long one goes silent-by-
// holding at its own end rather than at whichever source happens to be
// shortest overall.
void gather_frame(const LoadedSources& sources, std::size_t start,
                  std::vector<std::vector<float>>& dest);

// Says what the routing did, so a run that quietly left half a layout silent
// is visible rather than something to be discovered later on the meters.
// `label` is whatever resolve_layout printed for this plan - a named
// layout's label, or the channel list a custom selection was parsed from.
// `out` defaults to stdout; see print_channel_summary's comment just above -
// the same reasoning applies here.
void print_routing(const ac3::plan::Plan& p, const ac3::plan::Routing& routing,
                   std::string_view label, FILE* out = stdout);

}  // namespace ac3cli
