#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
        {.kind = DestinationKind::kProgramme1},
        {.kind = DestinationKind::kProgramme2},
        to_location(Location::kLeft),
        to_location(Location::kLfe2),
    };
    for (const auto& dest : cases) {
        const auto token = ac3::plan::format_destination(dest);
        INFO("token " << token);
        const auto parsed = ac3::plan::parse_destination(token);
        REQUIRE(parsed.has_value());
        CHECK(parsed->kind == dest.kind);
        if (dest.kind == DestinationKind::kLocation) {
            CHECK(parsed->location == dest.location);
        }
    }

    CHECK(ac3::plan::format_destination({.kind = DestinationKind::kUnassigned}) == "none");
    CHECK_FALSE(ac3::plan::parse_destination("not-a-real-token").has_value());
}
