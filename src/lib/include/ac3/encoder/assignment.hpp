#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/export.hpp"

// Multi-source input: an explicit alternative to route()'s automatic,
// direction-based panning. route() places ONE source onto a target by where
// its channels sit in the room; a caller with several sources - several
// files, several capture devices, or dual mono's two independent programmes
// - instead says exactly where each of THEIR channels goes, channel by
// channel. Assignment is that statement, and the route() overload below
// turns it into the same Routing render() already knows how to apply.
//
// This lives beside plan.hpp rather than in it because it is a distinct
// concern - plan.hpp says what to encode, this says how a source's channels
// reach it - and because most callers (a single WAV onto a named layout)
// never need it: they stay on the automatic route().

namespace ac3::plan {

// Where one source channel goes. A closed set: every assigned channel is
// exactly one of these, and an unassigned channel is worth naming rather
// than defaulting silently, since "goes nowhere" is a real answer a caller
// (a GUI's warning banner) needs to show.
enum class DestinationKind : std::uint8_t {
    kUnassigned,
    kLocation,
    kObject,
    kProgramme1,
    kProgramme2,
};

struct Destination {
    DestinationKind kind = DestinationKind::kUnassigned;
    // Meaningful only when kind == kLocation.
    eac3::chanmap::Location location{};

    [[nodiscard]] bool operator==(const Destination&) const = default;
};

// A loaded source's shape, independent of where its samples come from - a
// WAV file and a capture device both reduce to this. Deliberately not
// ac3::io::WavData: assignment has no file-I/O dependency.
struct SourceShape {
    std::size_t channels = 0;
    std::string label;  // "orbit51.wav" / "Scarlett 18i20" - error text only
};

// One row per (source, channel) a caller has actually set; everything else
// reads as kUnassigned. Rows are addressed by position rather than carried
// inline with SourceShape, so a caller can grow or shrink its source list
// without renumbering assignments it already made for channels that did not
// move.
class AC3FORGE_EXPORT Assignment {
   public:
    void set(std::size_t source, std::size_t channel, Destination dest);
    void clear(std::size_t source, std::size_t channel) {
        set(source, channel, Destination{});
    }
    [[nodiscard]] Destination at(std::size_t source, std::size_t channel) const;

    // Every (source, channel) that is kUnassigned AND within `sources`'
    // declared channel counts - the "goes nowhere" inventory a GUI's warning
    // banner reads from. A row set on a channel a source no longer has
    // (after the caller removed a source) does not appear: it is not there
    // to be unassigned.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> unassigned(
        std::span<const SourceShape> sources) const;

    // Rows of one kind, in (source, then channel) order - what an
    // object-mode or dual-mono caller iterates.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> rows_of(
        DestinationKind kind) const;

   private:
    std::map<std::pair<std::size_t, std::size_t>, Destination> rows_;
};

// Builds a Routing over the CONCATENATION of every source's channels (source
// 0's channels first, then source 1's, and so on) from an explicit
// Assignment rather than route()'s automatic panning: every kLocation row
// becomes a unity-gain entry into EVERY coded channel carrying that
// location - a bed channel and any dependent that shares it alike, since an
// explicit assignment states raw content for a location rather than a
// direction to render, and both the bed's legacy fallback and a full
// decoder's discrete channel should hear the same thing. kObject/
// kProgramme*/kUnassigned rows contribute nothing (all-zero) - object audio
// reaches the stream through the Atmos path, not Routing, and programme
// rows are dual_mono_routing()'s concern.
//
// Returns nullopt if `sources` is empty, if two rows target the same
// location, or if `target` cannot express a targeted location at all. Does
// NOT enforce dual mono's "exactly one channel per programme" - a general
// assignment is free to leave both programme slots unset, or even (for a
// target that is not dual mono) unused entirely; dual_mono_routing() is
// where that shape is required.
[[nodiscard]] AC3FORGE_EXPORT std::optional<Routing> route(const ChannelPlan& target,
                                                            std::span<const SourceShape> sources,
                                                            const Assignment& assignment);

// Dual mono's routing, expressed as one particular assignment: exactly one
// channel on kProgramme1 and one on kProgramme2, nothing else required of
// the rest. Returns the same identity Routing plan::route(LayoutId::
// kDualMono, 2, ...) already returns when the two programme channels are
// the whole of a single two-channel source; with several sources loaded it
// generalises to whichever two (source, channel) pairs are marked p1/p2,
// leaving every other loaded channel unrouted rather than an error - dual
// mono only cares about the two channels it was given. Returns nullopt if
// either programme has zero or more than one channel assigned to it - dual
// mono's two programmes are each a single channel, never a mix (§E1.3, no
// downmix between them).
[[nodiscard]] AC3FORGE_EXPORT std::optional<Routing> dual_mono_routing(
    std::span<const SourceShape> sources, const Assignment& assignment);

// Every E-AC-3-forcing choice in one place, generalising plan::carries():
// an immersive target (any dependent substream), VBR, any Annex E tool
// ticked, mixing metadata, or a reduced sample rate. A caller building a
// Plan whose codec is meant to be DERIVED rather than picked calls this
// instead of hand-testing each condition, so a new promotion trigger is
// added here once rather than at every call site that would otherwise
// duplicate plan::carries()'s logic.
[[nodiscard]] AC3FORGE_EXPORT Codec derive_codec(const ChannelPlan& target, const Tools& tools,
                                                  const Metadata& meta,
                                                  const std::optional<eac3::VbrConfig>& vbr,
                                                  SampleRate sample_rate);

// The `map=` token grammar's destination spelling: a Table E2.5 location
// name (as eac3::chanmap::name() prints it, e.g. "Ls", "LFE2"), "obj", "p1",
// "p2", or "none" for an explicit, silence-the-warning unassigned. The
// inverse of parse_destination - round-trips the way format_channels
// already round-trips through parse_channels.
[[nodiscard]] AC3FORGE_EXPORT std::string format_destination(Destination dest);
[[nodiscard]] AC3FORGE_EXPORT std::optional<Destination> parse_destination(std::string_view token);

}  // namespace ac3::plan
