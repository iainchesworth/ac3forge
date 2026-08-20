#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/plan.hpp"

// Assignment is the explicit alternative to route()'s automatic panning: a
// caller states exactly where each source channel goes rather than letting
// direction decide. What matters here is that it agrees with the automatic
// path wherever both apply (dual mono's identity routing, most notably), and
// that it fails closed - a collision or an inexpressible location is a
// nullopt, never a silent best guess.

using Catch::Approx;
using ac3::plan::Assignment;
using ac3::plan::Destination;
using ac3::plan::DestinationKind;
using ac3::plan::SourceShape;
using Location = ac3::eac3::chanmap::Location;

namespace {

[[nodiscard]] Destination to_location(Location location) {
    return {.kind = DestinationKind::kLocation, .location = location};
}

}  // namespace

// ---------------------------------------------------------------------------
// Assignment - the table itself
// ---------------------------------------------------------------------------

TEST_CASE("an unset channel reads back as unassigned", "[assignment]") {
    Assignment assignment;
    CHECK(assignment.at(0, 0).kind == DestinationKind::kUnassigned);

    assignment.set(0, 0, to_location(Location::kLeft));
    CHECK(assignment.at(0, 0).kind == DestinationKind::kLocation);
    CHECK(assignment.at(0, 0).location == Location::kLeft);

    // clear() and set()-to-unassigned are the same operation: the row goes
    // back to reading as never touched, not as a stored "none".
    assignment.clear(0, 0);
    CHECK(assignment.at(0, 0).kind == DestinationKind::kUnassigned);
}

TEST_CASE("unassigned() reports only channels a source actually declares", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 3, .label = "a.wav"}};
    Assignment assignment;
    assignment.set(0, 0, to_location(Location::kLeft));
    assignment.set(0, 1, {.kind = DestinationKind::kObject});
    // Beyond the source's declared width - not there to be unassigned.
    assignment.set(0, 5, to_location(Location::kRight));

    const auto missing = assignment.unassigned(sources);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0] == std::pair<std::size_t, std::size_t>{0, 2});
}

TEST_CASE("rows_of returns rows of one kind in (source, channel) order", "[assignment]") {
    Assignment assignment;
    assignment.set(1, 0, {.kind = DestinationKind::kProgramme1});
    assignment.set(0, 2, {.kind = DestinationKind::kProgramme1});
    assignment.set(0, 0, {.kind = DestinationKind::kProgramme2});

    const auto p1 = assignment.rows_of(DestinationKind::kProgramme1);
    REQUIRE(p1.size() == 2);
    CHECK(p1[0] == std::pair<std::size_t, std::size_t>{0, 2});
    CHECK(p1[1] == std::pair<std::size_t, std::size_t>{1, 0});

    const auto p2 = assignment.rows_of(DestinationKind::kProgramme2);
    REQUIRE(p2.size() == 1);
    CHECK(p2[0] == std::pair<std::size_t, std::size_t>{0, 0});
}

// ---------------------------------------------------------------------------
// route(target, sources, assignment)
// ---------------------------------------------------------------------------

TEST_CASE("a fully-assigned single source is carried, not rendered", "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const auto coded = ac3::plan::coded_channels(target);
    REQUIRE(coded.size() == 6);

    const std::vector<SourceShape> sources{{.channels = 6, .label = "orbit51.wav"}};
    Assignment assignment;
    for (std::size_t i = 0; i < coded.size(); ++i) {
        assignment.set(0, i, to_location(coded[i].location));
    }

    const auto routing = ac3::plan::route(target, sources, assignment);
    REQUIRE(routing.has_value());
    CHECK(routing->source_channels == 6);
    CHECK(routing->coded_channels == 6);
    CHECK(routing->is_permutation());
    for (int i = 0; i < 6; ++i) {
        CHECK(routing->at(i, i) == Approx(1.0));
    }
}

TEST_CASE("assignment spreads across sources at the right flat index", "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const auto coded = ac3::plan::coded_channels(target);
    REQUIRE(coded.size() == 6);

    // Two of the six coded channels come from source 0, the other four from
    // source 1 - the flat index has to land on source 1's channels, not
    // source 0's, from coded index 2 onward.
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"},
                                           {.channels = 4, .label = "b.wav"}};
    Assignment assignment;
    assignment.set(0, 0, to_location(coded[0].location));
    assignment.set(0, 1, to_location(coded[1].location));
    assignment.set(1, 0, to_location(coded[2].location));
    assignment.set(1, 1, to_location(coded[3].location));
    assignment.set(1, 2, to_location(coded[4].location));
    assignment.set(1, 3, to_location(coded[5].location));

    const auto routing = ac3::plan::route(target, sources, assignment);
    REQUIRE(routing.has_value());
    CHECK(routing->source_channels == 6);
    CHECK(routing->is_permutation());
    for (int flat = 0; flat < 6; ++flat) {
        CHECK(routing->at(flat, flat) == Approx(1.0));
    }
}

TEST_CASE("a bed channel and its dependent replacement both hear an explicit assignment",
          "[assignment]") {
    // 7.1's dependent replaces the bed's Ls/Rs with its own AND adds the
    // rears - so Ls appears twice in coded_channels(), once bed, once not.
    // An explicit row for Ls has to reach both, the same way the automatic
    // router's LFE handling already does by location rather than by index.
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k71);
    const auto coded = ac3::plan::coded_channels(target);
    REQUIRE(coded.size() == 10);

    const std::vector<SourceShape> sources{{.channels = 1, .label = "ls.wav"}};
    Assignment assignment;
    assignment.set(0, 0, to_location(Location::kLeftSurround));

    const auto routing = ac3::plan::route(target, sources, assignment);
    REQUIRE(routing.has_value());
    int hits = 0;
    for (std::size_t c = 0; c < coded.size(); ++c) {
        if (coded[c].location != Location::kLeftSurround) {
            CHECK(routing->at(static_cast<int>(c), 0) == Approx(0.0));
            continue;
        }
        CHECK(routing->at(static_cast<int>(c), 0) == Approx(1.0));
        ++hits;
    }
    CHECK(hits == 2);  // the bed's Ls and the dependent's Ls, both fed
}

TEST_CASE("route() rejects two rows aimed at the same location", "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};
    Assignment assignment;
    assignment.set(0, 0, to_location(Location::kLeft));
    assignment.set(0, 1, to_location(Location::kLeft));

    CHECK_FALSE(ac3::plan::route(target, sources, assignment).has_value());
}

TEST_CASE("route() rejects a location the target cannot express", "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::kStereo);
    const std::vector<SourceShape> sources{{.channels = 1, .label = "a.wav"}};
    Assignment assignment;
    assignment.set(0, 0, to_location(Location::kLeftSurround));

    CHECK_FALSE(ac3::plan::route(target, sources, assignment).has_value());
}

TEST_CASE("object and unassigned rows contribute nothing, but are not an error",
          "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};
    Assignment assignment;
    assignment.set(0, 0, {.kind = DestinationKind::kObject});
    // channel 1 left entirely unassigned

    const auto routing = ac3::plan::route(target, sources, assignment);
    REQUIRE(routing.has_value());
    for (int c = 0; c < routing->coded_channels; ++c) {
        for (int s = 0; s < routing->source_channels; ++s) {
            CHECK(routing->at(c, s) == Approx(0.0));
        }
    }
}

TEST_CASE("objm rows contribute nothing to route(), same as obj", "[assignment]") {
    // route() only ever carries kLocation content; an objm group's folded
    // content reaches the stream through the object plane assembly instead
    // (see encoder_controller.cpp's encodeObjects) - route() itself must
    // treat kObjectMono exactly like kObject: present, but all-zero here.
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};
    Assignment assignment;
    assignment.set(0, 0, {.kind = DestinationKind::kObjectMono});
    assignment.set(0, 1, {.kind = DestinationKind::kObjectMono});

    const auto routing = ac3::plan::route(target, sources, assignment);
    REQUIRE(routing.has_value());
    for (int c = 0; c < routing->coded_channels; ++c) {
        for (int s = 0; s < routing->source_channels; ++s) {
            CHECK(routing->at(c, s) == Approx(0.0));
        }
    }
}

TEST_CASE("route() applies a row's trim as linear gain, not just unity", "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::kStereo);
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};
    Assignment assignment;
    assignment.set(0, 0, {.kind = DestinationKind::kLocation, .location = Location::kLeft,
                          .trim_db = -6.0});
    assignment.set(0, 1, {.kind = DestinationKind::kLocation, .location = Location::kRight,
                          .trim_db = 0.0});

    const auto routing = ac3::plan::route(target, sources, assignment);
    REQUIRE(routing.has_value());
    // -6dB is not exactly a factor of 0.5, but close enough that a test
    // asserting "roughly half" would not catch a formula bug (e.g. dividing
    // by 6 instead of 20) - so this checks the exact 10^(-6/20) figure.
    CHECK(routing->at(0, 0) == Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(1e-9));
    CHECK(routing->at(1, 1) == Approx(1.0));  // untrimmed row stays unity
}

TEST_CASE("route() refuses an empty source list", "[assignment]") {
    const auto target = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const Assignment assignment;
    CHECK_FALSE(ac3::plan::route(target, {}, assignment).has_value());
}

// ---------------------------------------------------------------------------
// dual_mono_routing - agreement with the existing hardcoded identity route
// ---------------------------------------------------------------------------

TEST_CASE("dual_mono_routing reproduces route()'s identity for one two-channel source",
          "[assignment][dual-mono]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "orbit.wav"}};
    Assignment assignment;
    assignment.set(0, 0, {.kind = DestinationKind::kProgramme1});
    assignment.set(0, 1, {.kind = DestinationKind::kProgramme2});

    const auto routing = ac3::plan::dual_mono_routing(sources, assignment);
    REQUIRE(routing.has_value());

    const auto legacy = ac3::plan::route(ac3::plan::LayoutId::kDualMono, 2,
                                         ac3::meta::CentreMixLevel::kMinus4_5dB,
                                         ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(legacy.has_value());

    CHECK(routing->source_channels == legacy->source_channels);
    CHECK(routing->coded_channels == legacy->coded_channels);
    for (int c = 0; c < routing->coded_channels; ++c) {
        for (int s = 0; s < routing->source_channels; ++s) {
            CHECK(routing->at(c, s) == Approx(legacy->at(c, s)));
        }
    }
}

TEST_CASE("dual_mono_routing spans two independent one-channel sources", "[assignment][dual-mono]") {
    const std::vector<SourceShape> sources{{.channels = 1, .label = "en.wav"},
                                           {.channels = 1, .label = "fr.wav"}};
    Assignment assignment;
    assignment.set(0, 0, {.kind = DestinationKind::kProgramme1});
    assignment.set(1, 0, {.kind = DestinationKind::kProgramme2});

    const auto routing = ac3::plan::dual_mono_routing(sources, assignment);
    REQUIRE(routing.has_value());
    CHECK(routing->is_permutation());
    CHECK(routing->at(0, 0) == Approx(1.0));
    CHECK(routing->at(1, 1) == Approx(1.0));
    CHECK(routing->at(0, 1) == Approx(0.0));
    CHECK(routing->at(1, 0) == Approx(0.0));
}

TEST_CASE("dual_mono_routing applies each programme row's own trim", "[assignment][dual-mono]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "orbit.wav"}};
    Assignment assignment;
    assignment.set(0, 0, {.kind = DestinationKind::kProgramme1, .trim_db = -12.0});
    assignment.set(0, 1, {.kind = DestinationKind::kProgramme2, .trim_db = 6.0});

    const auto routing = ac3::plan::dual_mono_routing(sources, assignment);
    REQUIRE(routing.has_value());
    CHECK(routing->at(0, 0) == Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(1e-9));
    CHECK(routing->at(1, 1) == Approx(std::pow(10.0, 6.0 / 20.0)).epsilon(1e-9));
}

TEST_CASE("dual_mono_routing rejects a missing or doubled programme", "[assignment][dual-mono]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};

    Assignment missing_p2;
    missing_p2.set(0, 0, {.kind = DestinationKind::kProgramme1});
    CHECK_FALSE(ac3::plan::dual_mono_routing(sources, missing_p2).has_value());

    Assignment doubled_p1;
    doubled_p1.set(0, 0, {.kind = DestinationKind::kProgramme1});
    doubled_p1.set(0, 1, {.kind = DestinationKind::kProgramme1});
    CHECK_FALSE(ac3::plan::dual_mono_routing(sources, doubled_p1).has_value());
}

// ---------------------------------------------------------------------------
// derive_codec
// ---------------------------------------------------------------------------

TEST_CASE("derive_codec stays AC-3 until something actually needs E-AC-3", "[assignment]") {
    const auto narrow = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k51);
    const ac3::plan::Tools no_tools{};
    const ac3::plan::Metadata plain_meta{};

    CHECK(ac3::plan::derive_codec(narrow, no_tools, plain_meta, std::nullopt,
                                  ac3::SampleRate::k48000) == ac3::plan::Codec::kAc3);

    SECTION("an immersive target promotes") {
        const auto wide = ac3::plan::channel_plan_for(ac3::plan::LayoutId::k71);
        CHECK(ac3::plan::derive_codec(wide, no_tools, plain_meta, std::nullopt,
                                      ac3::SampleRate::k48000) == ac3::plan::Codec::kEac3);
    }

    SECTION("a ticked Annex E tool promotes") {
        ac3::plan::Tools tools{};
        tools.coupling = true;
        CHECK(ac3::plan::derive_codec(narrow, tools, plain_meta, std::nullopt,
                                      ac3::SampleRate::k48000) == ac3::plan::Codec::kEac3);
    }

    SECTION("VBR promotes") {
        const ac3::eac3::VbrConfig vbr{.quality = 0.75};
        CHECK(ac3::plan::derive_codec(narrow, no_tools, plain_meta, vbr,
                                      ac3::SampleRate::k48000) == ac3::plan::Codec::kEac3);
    }

    SECTION("mixing metadata promotes") {
        ac3::plan::Metadata meta{};
        meta.mixmeta = true;
        CHECK(ac3::plan::derive_codec(narrow, no_tools, meta, std::nullopt,
                                      ac3::SampleRate::k48000) == ac3::plan::Codec::kEac3);
    }

    SECTION("a reduced sample rate promotes") {
        CHECK(ac3::plan::derive_codec(narrow, no_tools, plain_meta, std::nullopt,
                                      ac3::SampleRate::k24000) == ac3::plan::Codec::kEac3);
    }
}

// ---------------------------------------------------------------------------
// destination tokens
// ---------------------------------------------------------------------------

TEST_CASE("destination tokens round-trip through format/parse", "[assignment]") {
    const std::vector<Destination> cases{
        {.kind = DestinationKind::kUnassigned},
        {.kind = DestinationKind::kObject},
        {.kind = DestinationKind::kObjectMono},
        {.kind = DestinationKind::kProgramme1},
        {.kind = DestinationKind::kProgramme2},
        to_location(Location::kLeft),
        to_location(Location::kLfe2),
        {.kind = DestinationKind::kLocation, .location = Location::kLeft, .trim_db = -3.5},
        {.kind = DestinationKind::kObject, .trim_db = 12.0},
        {.kind = DestinationKind::kObjectMono, .trim_db = -0.5},
        {.kind = DestinationKind::kProgramme1, .trim_db = 24.0},
        {.kind = DestinationKind::kProgramme2, .trim_db = -24.0},
    };
    for (const auto& dest : cases) {
        const auto token = ac3::plan::format_destination(dest);
        INFO("token " << token);
        const auto parsed = ac3::plan::parse_destination(token);
        REQUIRE(parsed.has_value());
        CHECK(parsed->kind == dest.kind);
        CHECK(parsed->trim_db == dest.trim_db);
        if (dest.kind == DestinationKind::kLocation) {
            CHECK(parsed->location == dest.location);
        }
    }

    CHECK(ac3::plan::format_destination({.kind = DestinationKind::kUnassigned}) == "none");
    CHECK(ac3::plan::format_destination({.kind = DestinationKind::kObjectMono}) == "objm");
    CHECK_FALSE(ac3::plan::parse_destination("not-a-real-token").has_value());
}

TEST_CASE("a trim suffix formats compactly and parses back exactly", "[assignment]") {
    // format_destination's spelling: whole dB values print with no decimal,
    // fractional ones with exactly one - never float-formatting noise like
    // "-3.50" or "-3.4999999999999996".
    CHECK(ac3::plan::format_destination(to_location(Location::kLeft)) == "L");  // no trim -> no @
    CHECK(ac3::plan::format_destination(
              {.kind = DestinationKind::kLocation, .location = Location::kLeft, .trim_db = -3.5}) ==
          "L@-3.5");
    CHECK(ac3::plan::format_destination(
              {.kind = DestinationKind::kLocation, .location = Location::kLeft, .trim_db = 2.0}) ==
          "L@2");
    CHECK(ac3::plan::format_destination(
              {.kind = DestinationKind::kLocation, .location = Location::kLeft, .trim_db = -0.5}) ==
          "L@-0.5");

    const auto parsed = ac3::plan::parse_destination("obj@-6.5");
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == DestinationKind::kObject);
    CHECK(parsed->trim_db == -6.5);
}

TEST_CASE("a trim outside the documented range is clamped or rejected", "[assignment]") {
    // A small overshoot (within 1dB of the boundary) snaps to the boundary
    // rather than failing outright - see parse_trim's own comment on why a
    // coarse trim control is allowed a little slack.
    const auto slight = ac3::plan::parse_destination("L@24.5");
    REQUIRE(slight.has_value());
    CHECK(slight->trim_db == 24.0);
    const auto slight_low = ac3::plan::parse_destination("L@-24.7");
    REQUIRE(slight_low.has_value());
    CHECK(slight_low->trim_db == -24.0);

    // Wildly out of range is a mistake to report, not silently clamp.
    CHECK_FALSE(ac3::plan::parse_destination("L@30").has_value());
    CHECK_FALSE(ac3::plan::parse_destination("L@1000").has_value());
    CHECK_FALSE(ac3::plan::parse_destination("L@not-a-number").has_value());
    CHECK_FALSE(ac3::plan::parse_destination("L@").has_value());
}

// ---------------------------------------------------------------------------
// parse_assignment / format_assignment - the map= spec, whole
// ---------------------------------------------------------------------------

TEST_CASE("parse_assignment reads a full map= spec, one entry per channel", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"},
                                           {.channels = 1, .label = "b.wav"}};
    Assignment assignment;
    REQUIRE(ac3::plan::parse_assignment("0.0:L,0.1:R,1.0:LFE", sources, assignment));
    CHECK(assignment.at(0, 0) == to_location(Location::kLeft));
    CHECK(assignment.at(0, 1) == to_location(Location::kRight));
    CHECK(assignment.at(1, 0) == to_location(Location::kLfe));
}

TEST_CASE("parse_assignment expands a channel range for obj and none only", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 4, .label = "a.wav"}};

    Assignment objects;
    REQUIRE(ac3::plan::parse_assignment("0.0-3:obj", sources, objects));
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK(objects.at(0, c).kind == DestinationKind::kObject);
    }

    Assignment none;
    REQUIRE(ac3::plan::parse_assignment("0.0-3:none", sources, none));
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK(none.at(0, c).kind == DestinationKind::kUnassigned);
    }

    // A location names exactly one channel - a range there is ambiguous
    // about which one it means, so it is rejected rather than guessed at.
    Assignment rejected;
    CHECK_FALSE(ac3::plan::parse_assignment("0.0-3:L", sources, rejected));
}

TEST_CASE("parse_assignment expands a channel range for objm too, folding to one group",
          "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 4, .label = "a.wav"}};
    Assignment assignment;
    REQUIRE(ac3::plan::parse_assignment("0.0-1:objm,0.2-3:obj", sources, assignment));
    CHECK(assignment.at(0, 0).kind == DestinationKind::kObjectMono);
    CHECK(assignment.at(0, 1).kind == DestinationKind::kObjectMono);
    // The other range on the same source stays plain obj - objm is a
    // per-range choice, not sticky for the whole source.
    CHECK(assignment.at(0, 2).kind == DestinationKind::kObject);
    CHECK(assignment.at(0, 3).kind == DestinationKind::kObject);
}

TEST_CASE("parse_assignment reads a per-channel trim alongside the destination", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};
    Assignment assignment;
    REQUIRE(ac3::plan::parse_assignment("0.0:L@-3.5,0.1:R", sources, assignment));
    CHECK(assignment.at(0, 0).trim_db == -3.5);
    CHECK(assignment.at(0, 1).trim_db == 0.0);
}

TEST_CASE("parse_assignment requires every declared channel to appear", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"}};
    Assignment assignment;
    // Channel 1 is never mentioned - not even as "none".
    CHECK_FALSE(ac3::plan::parse_assignment("0.0:L", sources, assignment));
}

TEST_CASE("parse_assignment rejects a channel mentioned twice", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 1, .label = "a.wav"}};
    Assignment assignment;
    CHECK_FALSE(ac3::plan::parse_assignment("0.0:L,0.0:R", sources, assignment));
}

TEST_CASE("parse_assignment rejects an out-of-range source or channel", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 1, .label = "a.wav"}};
    Assignment assignment;
    CHECK_FALSE(ac3::plan::parse_assignment("1.0:L", sources, assignment));   // no source 1
    CHECK_FALSE(ac3::plan::parse_assignment("0.1:L", sources, assignment));  // no channel 1
}

TEST_CASE("parse_assignment rejects malformed text outright", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 1, .label = "a.wav"}};
    Assignment assignment;
    CHECK_FALSE(ac3::plan::parse_assignment("", sources, assignment));
    CHECK_FALSE(ac3::plan::parse_assignment("0.0", sources, assignment));         // no ':'
    CHECK_FALSE(ac3::plan::parse_assignment("0:L", sources, assignment));         // no '.'
    CHECK_FALSE(ac3::plan::parse_assignment("0.0:bogus", sources, assignment));   // bad dest
}

TEST_CASE("format_assignment round-trips through parse_assignment", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 2, .label = "a.wav"},
                                           {.channels = 2, .label = "b.wav"}};
    Assignment original;
    original.set(0, 0, to_location(Location::kLeft));
    original.set(0, 1, to_location(Location::kRight));
    original.set(1, 0, {.kind = DestinationKind::kObject});
    original.set(1, 1, {.kind = DestinationKind::kUnassigned});

    const auto text = ac3::plan::format_assignment(sources, original);
    Assignment roundtripped;
    REQUIRE(ac3::plan::parse_assignment(text, sources, roundtripped));
    for (std::size_t s = 0; s < sources.size(); ++s) {
        for (std::size_t c = 0; c < sources[s].channels; ++c) {
            CHECK(roundtripped.at(s, c) == original.at(s, c));
        }
    }
}

TEST_CASE("format_assignment round-trips trims and an objm group", "[assignment]") {
    const std::vector<SourceShape> sources{{.channels = 3, .label = "a.wav"}};
    Assignment original;
    original.set(0, 0, {.kind = DestinationKind::kLocation,
                        .location = Location::kLeft,
                        .trim_db = -3.5});
    original.set(0, 1, {.kind = DestinationKind::kObjectMono, .trim_db = 6.0});
    original.set(0, 2, {.kind = DestinationKind::kObjectMono, .trim_db = 6.0});

    const auto text = ac3::plan::format_assignment(sources, original);
    INFO("map= text: " << text);
    Assignment roundtripped;
    REQUIRE(ac3::plan::parse_assignment(text, sources, roundtripped));
    for (std::size_t c = 0; c < 3; ++c) {
        CHECK(roundtripped.at(0, c) == original.at(0, c));
    }
}
