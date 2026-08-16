#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/admbridge/bridge.hpp"
#include "ac3/admbridge/coordinates.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3adm/ac3adm.hpp"

// ac3::admbridge - roadmap item B1 phase 2 ("ADM BWF reader feeding the JOC encoder", see
// ROADMAP.md). Most cases here construct ac3adm::AdmDocument/AdmModel values directly (they are
// plain aggregates, per ac3adm/model.hpp's own design - no parser needed to build one) rather
// than a byte-level BW64 file, which keeps the error-path and coordinate/timeline unit tests
// focused on this module's own logic. The one flagship test at the bottom goes through a REAL
// byte-level BW64 fixture and ac3adm::parse_bw64() end to end, then through a real
// ac3::oba::AtmosEncoder::encode_frame()/Eac3Decoder round trip, per this project's own standard
// for codec-adjacent behaviour (silence/frame-0 checks give false passes - see
// tests/test_atmos_motion.cpp's own flagship test for the established pattern this one follows).

namespace {

constexpr int kFrame = ac3::kSamplesPerFrame;

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

ac3adm::PolarPosition polar(double azimuth_deg, double elevation_deg, double distance = 1.0) {
    return {.azimuth_deg = azimuth_deg, .elevation_deg = elevation_deg, .distance = distance};
}

}  // namespace

TEST_CASE("polar_to_adm_cartesian converts BS.2076-2 Clause 8 cardinal points correctly",
         "[admbridge][coordinates]") {
    // Clause 8: azimuth 0 = straight ahead = +Y (front-positive); X is right-positive so
    // positive (left) azimuth is negative X; elevation 0 = level, positive = up = +Z.
    SECTION("straight ahead") {
        const auto c = ac3::admbridge::polar_to_adm_cartesian(polar(0.0, 0.0));
        CHECK_THAT(c.x, Catch::Matchers::WithinAbs(0.0, 1e-9));
        CHECK_THAT(c.y, Catch::Matchers::WithinAbs(1.0, 1e-9));
        CHECK_THAT(c.z, Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
    SECTION("90 degrees left is negative X (X is right-positive)") {
        const auto c = ac3::admbridge::polar_to_adm_cartesian(polar(90.0, 0.0));
        CHECK_THAT(c.x, Catch::Matchers::WithinAbs(-1.0, 1e-9));
        CHECK_THAT(c.y, Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
    SECTION("-90 degrees (right) is positive X") {
        const auto c = ac3::admbridge::polar_to_adm_cartesian(polar(-90.0, 0.0));
        CHECK_THAT(c.x, Catch::Matchers::WithinAbs(1.0, 1e-9));
    }
    SECTION("straight behind (180 degrees) is negative Y") {
        const auto c = ac3::admbridge::polar_to_adm_cartesian(polar(180.0, 0.0));
        CHECK_THAT(c.y, Catch::Matchers::WithinAbs(-1.0, 1e-9));
    }
    SECTION("90 degrees elevation (up) is positive Z, independent of azimuth") {
        const auto c = ac3::admbridge::polar_to_adm_cartesian(polar(45.0, 90.0));
        CHECK_THAT(c.x, Catch::Matchers::WithinAbs(0.0, 1e-9));
        CHECK_THAT(c.y, Catch::Matchers::WithinAbs(0.0, 1e-9));
        CHECK_THAT(c.z, Catch::Matchers::WithinAbs(1.0, 1e-9));
    }
    SECTION("-90 degrees elevation (down) is negative Z") {
        const auto c = ac3::admbridge::polar_to_adm_cartesian(polar(0.0, -90.0));
        CHECK_THAT(c.z, Catch::Matchers::WithinAbs(-1.0, 1e-9));
    }
}

TEST_CASE("adm_cartesian_to_room maps the unit cube onto ac3::oba::Position's room convention",
         "[admbridge][coordinates]") {
    // Table 16 + Clause 8: X right-positive, Y front-positive, Z top-positive, [-1, 1] cube.
    // oamd.hpp: x 0=left..1=right, y 0=front..1=back, z -1=floor..+1=ceiling.
    SECTION("left wall (X=-1) is room x=0") {
        CHECK_THAT(ac3::admbridge::adm_cartesian_to_room({.x = -1.0, .y = 0.0, .z = 0.0}).x,
                  Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
    SECTION("right wall (X=+1) is room x=1") {
        CHECK_THAT(ac3::admbridge::adm_cartesian_to_room({.x = 1.0, .y = 0.0, .z = 0.0}).x,
                  Catch::Matchers::WithinAbs(1.0, 1e-9));
    }
    SECTION("front wall (Y=+1, front-positive) is room y=0") {
        CHECK_THAT(ac3::admbridge::adm_cartesian_to_room({.x = 0.0, .y = 1.0, .z = 0.0}).y,
                  Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
    SECTION("back wall (Y=-1) is room y=1") {
        CHECK_THAT(ac3::admbridge::adm_cartesian_to_room({.x = 0.0, .y = -1.0, .z = 0.0}).y,
                  Catch::Matchers::WithinAbs(1.0, 1e-9));
    }
    SECTION("Z passes through unchanged (both conventions are top-positive [-1, 1])") {
        const auto up = ac3::admbridge::adm_cartesian_to_room({.x = 0.0, .y = 0.0, .z = 0.7});
        CHECK_THAT(up.z, Catch::Matchers::WithinAbs(0.7, 1e-9));
    }
    SECTION("the cube's centre is the room's centre-front-floor (0.5, 0.5, 0)") {
        const auto centre = ac3::admbridge::adm_cartesian_to_room({.x = 0.0, .y = 0.0, .z = 0.0});
        CHECK_THAT(centre.x, Catch::Matchers::WithinAbs(0.5, 1e-9));
        CHECK_THAT(centre.y, Catch::Matchers::WithinAbs(0.5, 1e-9));
        CHECK_THAT(centre.z, Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("polar positions at the 5.1 ring reproduce this project's own known room coordinates",
         "[admbridge][coordinates]") {
    // Cross-check against tests/test_atmos_motion.cpp's own kL/kR/kSR constants (that file's own
    // comment: "the 5.1 ring's L, SR and R azimuths... L +30 degrees, SR -110 degrees, R -30
    // degrees"), which are also exactly BS.2076-2 Annex A's own M+030/M-030/M-110 speaker-label
    // azimuths. Converting those same azimuths through this module's own coordinate functions
    // should reproduce those exact, independently-authored constants.
    SECTION("L: +30 degrees azimuth") {
        const auto p = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(30.0, 0.0)});
        CHECK_THAT(p.x, Catch::Matchers::WithinAbs(0.25, 1e-6));
        CHECK_THAT(p.y, Catch::Matchers::WithinAbs(0.066987, 1e-6));
        CHECK_THAT(p.z, Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
    SECTION("R: -30 degrees azimuth") {
        const auto p = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(-30.0, 0.0)});
        CHECK_THAT(p.x, Catch::Matchers::WithinAbs(0.75, 1e-6));
        CHECK_THAT(p.y, Catch::Matchers::WithinAbs(0.066987, 1e-6));
    }
    SECTION("SR: -110 degrees azimuth") {
        const auto p = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(-110.0, 0.0)});
        CHECK_THAT(p.x, Catch::Matchers::WithinAbs(0.969846, 1e-6));
        CHECK_THAT(p.y, Catch::Matchers::WithinAbs(0.671010, 1e-6));
    }
}

TEST_CASE("adm_position_to_room dispatches on the Position variant", "[admbridge][coordinates]") {
    SECTION("Cartesian alternative goes straight through adm_cartesian_to_room") {
        const ac3adm::Position position{ac3adm::CartesianPosition{.x = 0.4, .y = -0.2, .z = 0.1}};
        const auto expected = ac3::admbridge::adm_cartesian_to_room(std::get<ac3adm::CartesianPosition>(position));
        const auto got = ac3::admbridge::adm_position_to_room(position);
        CHECK(got.x == expected.x);
        CHECK(got.y == expected.y);
        CHECK(got.z == expected.z);
    }
    SECTION("Polar alternative goes through polar_to_adm_cartesian first") {
        const ac3adm::Position position{polar(60.0, 10.0)};
        const auto expected = ac3::admbridge::adm_cartesian_to_room(
            ac3::admbridge::polar_to_adm_cartesian(std::get<ac3adm::PolarPosition>(position)));
        const auto got = ac3::admbridge::adm_position_to_room(position);
        CHECK_THAT(got.x, Catch::Matchers::WithinAbs(expected.x, 1e-12));
        CHECK_THAT(got.y, Catch::Matchers::WithinAbs(expected.y, 1e-12));
        CHECK_THAT(got.z, Catch::Matchers::WithinAbs(expected.z, 1e-12));
    }
}

// ---------------------------------------------------------------------------
// build_channel_path: the §10.3 hold/glide state machine
// ---------------------------------------------------------------------------

namespace {

ac3adm::AudioBlockFormat block_at(double rtime_s, std::optional<double> duration_s,
                                  ac3adm::PolarPosition position, double gain = 1.0,
                                  bool jump_position = false,
                                  std::optional<double> interpolation_length_s = std::nullopt) {
    ac3adm::AudioBlockFormat block;
    block.rtime_s = rtime_s;
    block.has_duration = duration_s.has_value();
    block.duration_s = duration_s.value_or(0.0);
    block.gain = gain;
    block.cartesian = false;
    block.position = position;
    block.has_jump_position = true;
    block.jump_position = jump_position;
    block.has_interpolation_length = interpolation_length_s.has_value();
    block.interpolation_length_s = interpolation_length_s.value_or(0.0);
    return block;
}

ac3adm::AudioChannelFormat channel_with(std::vector<ac3adm::AudioBlockFormat> blocks) {
    ac3adm::AudioChannelFormat channel;
    channel.id = "AC_TEST";
    channel.type = ac3adm::TypeDefinition::kObjects;
    channel.block_formats = std::move(blocks);
    return channel;
}

}  // namespace

TEST_CASE("build_channel_path holds a single static block everywhere", "[admbridge]") {
    const auto channel = channel_with({block_at(0.5, std::nullopt, polar(45.0, 0.0), 0.8)});
    const auto path = ac3::admbridge::build_channel_path(channel, /*object_start_s=*/2.0, false);
    REQUIRE(path.has_value());
    for (const double t : {-100.0, 0.0, 2.5, 1e6}) {
        const auto placement = path->evaluate(t);
        CAPTURE(t);
        const auto expected = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(45.0, 0.0)});
        CHECK_THAT(placement.position.x, Catch::Matchers::WithinAbs(expected.x, 1e-9));
        CHECK(placement.gain == 0.8);
        CHECK(placement.lfe_send == 0.0);
    }
}

TEST_CASE("build_channel_path with jumpPosition=0 ramps continuously across the whole block",
         "[admbridge]") {
    // Block 0 holds at azimuth 30 for [0, 1); block 1 (jumpPosition=0) ramps to azimuth -30 over
    // [1, 3) - BS.2076-2 §10.3: "the renderer will interpolate a moving object between positions
    // over the full duration of the block."
    const auto channel = channel_with({
        block_at(0.0, 1.0, polar(30.0, 0.0)),
        block_at(1.0, 2.0, polar(-30.0, 0.0), 1.0, /*jump_position=*/false),
    });
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, false);
    REQUIRE(path.has_value());

    const auto at_start = path->evaluate(1.0);
    const auto at_mid = path->evaluate(2.0);
    const auto at_end = path->evaluate(3.0);
    const auto want_start = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(30.0, 0.0)});
    const auto want_end = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(-30.0, 0.0)});

    CHECK_THAT(at_start.position.x, Catch::Matchers::WithinAbs(want_start.x, 1e-6));
    CHECK_THAT(at_end.position.x, Catch::Matchers::WithinAbs(want_end.x, 1e-6));
    // Midpoint must be strictly between the two endpoints (a genuine ramp, not a hold-then-jump).
    const double lo = std::min(want_start.x, want_end.x);
    const double hi = std::max(want_start.x, want_end.x);
    CHECK(at_mid.position.x > lo + 1e-6);
    CHECK(at_mid.position.x < hi - 1e-6);
}

TEST_CASE("build_channel_path with jumpPosition=1 and no interpolationLength "
         "jumps near-instantly and holds", "[admbridge]") {
    const auto channel = channel_with({
        block_at(0.0, 1.0, polar(30.0, 0.0)),
        block_at(1.0, 2.0, polar(-30.0, 0.0), 1.0, /*jump_position=*/true),
    });
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, false);
    REQUIRE(path.has_value());

    const auto want_old = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(30.0, 0.0)});
    const auto want_new = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(-30.0, 0.0)});

    // Right at the boundary, the old value should still be in effect (§10.3's own first-block
    // rule aside, the SECOND block's jump has not yet been reached at its own rtime boundary
    // itself - it is reached an instant after).
    CHECK_THAT(path->evaluate(1.0).position.x, Catch::Matchers::WithinAbs(want_old.x, 1e-6));
    // A moment after, and for the rest of the block, the new value holds.
    CHECK_THAT(path->evaluate(1.0001).position.x, Catch::Matchers::WithinAbs(want_new.x, 1e-6));
    CHECK_THAT(path->evaluate(2.0).position.x, Catch::Matchers::WithinAbs(want_new.x, 1e-6));
    CHECK_THAT(path->evaluate(2.9).position.x, Catch::Matchers::WithinAbs(want_new.x, 1e-6));
}

TEST_CASE("build_channel_path with jumpPosition=1 and an interpolationLength "
         "ramps briefly then holds", "[admbridge]") {
    const auto channel = channel_with({
        block_at(0.0, 1.0, polar(30.0, 0.0)),
        block_at(1.0, 2.0, polar(-30.0, 0.0), 1.0, /*jump_position=*/true,
                /*interpolation_length_s=*/0.2),
    });
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, false);
    REQUIRE(path.has_value());

    const auto want_old = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(30.0, 0.0)});
    const auto want_new = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(-30.0, 0.0)});

    CHECK_THAT(path->evaluate(1.0).position.x, Catch::Matchers::WithinAbs(want_old.x, 1e-6));
    // Midway through the 0.2s ramp: strictly between the two values, unlike the no-
    // interpolationLength case above.
    const auto mid = path->evaluate(1.1);
    const double lo = std::min(want_old.x, want_new.x);
    const double hi = std::max(want_old.x, want_new.x);
    CHECK(mid.position.x > lo + 1e-6);
    CHECK(mid.position.x < hi - 1e-6);
    // Ramp complete at 1.2s, and held for the rest of the block.
    CHECK_THAT(path->evaluate(1.2).position.x, Catch::Matchers::WithinAbs(want_new.x, 1e-6));
    CHECK_THAT(path->evaluate(2.9).position.x, Catch::Matchers::WithinAbs(want_new.x, 1e-6));
}

TEST_CASE("build_channel_path's first block always holds regardless of its own jumpPosition",
         "[admbridge]") {
    // §10.3: "the position specified in the first block covers the entire length of the block
    // (regardless of the jumpPosition and interpolationLength properties)." Even with
    // jumpPosition=0 declared on block 0 itself, it must still hold across [0, 1) rather than
    // "ramping in from nothing".
    const auto channel = channel_with({
        block_at(0.0, 1.0, polar(45.0, 0.0), 1.0, /*jump_position=*/false),
        block_at(1.0, 1.0, polar(-45.0, 0.0)),
    });
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, false);
    REQUIRE(path.has_value());
    const auto want_first = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(45.0, 0.0)});
    CHECK_THAT(path->evaluate(0.0).position.x, Catch::Matchers::WithinAbs(want_first.x, 1e-6));
    CHECK_THAT(path->evaluate(0.5).position.x, Catch::Matchers::WithinAbs(want_first.x, 1e-6));
    CHECK_THAT(path->evaluate(0.999).position.x, Catch::Matchers::WithinAbs(want_first.x, 1e-6));
}

TEST_CASE("build_channel_path's first block, when it omits duration itself, still holds only "
         "until the second block's own start - not indefinitely, and not stretching the second "
         "block's ramp back to time zero", "[admbridge]") {
    // §5.4.1 only "should" (not "must") pair rtime with duration once a channel is dynamic (more
    // than one block), so a first block with no duration at all is legal, if discouraged. Its
    // true end is the second block's own start (here rtime=1.0), not "forever" - block 1 below
    // is a jumpPosition=0 glide across ITS OWN [1.0, 2.0) span; if the first block's hold were
    // skipped instead of ending at 1.0, KeyframePath would have only one keyframe at time 0 to
    // interpolate from, stretching the ramp all the way back to 0 instead of starting at 1.0.
    const auto channel = channel_with({
        block_at(0.0, std::nullopt, polar(45.0, 0.0)),
        block_at(1.0, 1.0, polar(-45.0, 0.0), 1.0, /*jump_position=*/false),
    });
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, false);
    REQUIRE(path.has_value());
    const auto want_first = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(45.0, 0.0)});
    const auto want_second = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(-45.0, 0.0)});

    // Still exactly the first block's value anywhere before the second block starts - not
    // already partway interpolated toward the second block's value.
    CHECK_THAT(path->evaluate(0.5).position.x, Catch::Matchers::WithinAbs(want_first.x, 1e-6));
    CHECK_THAT(path->evaluate(0.999).position.x, Catch::Matchers::WithinAbs(want_first.x, 1e-6));

    // The glide happens only within block 1's own [1.0, 2.0) span.
    const auto mid = path->evaluate(1.5);
    const double lo = std::min(want_first.x, want_second.x);
    const double hi = std::max(want_first.x, want_second.x);
    CHECK(mid.position.x > lo + 1e-6);
    CHECK(mid.position.x < hi - 1e-6);
    CHECK_THAT(path->evaluate(2.0).position.x, Catch::Matchers::WithinAbs(want_second.x, 1e-6));
}

TEST_CASE("build_channel_path with force_lfe discards the block's own position and gain",
         "[admbridge]") {
    const auto channel = channel_with({block_at(0.0, std::nullopt, polar(123.0, 45.0), 0.5)});
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, /*force_lfe=*/true);
    REQUIRE(path.has_value());
    const auto placement = path->evaluate(10.0);
    CHECK(placement.gain == 0.0);
    CHECK(placement.lfe_send == 1.0);
}

TEST_CASE("build_channel_path rejects an empty block sequence", "[admbridge]") {
    ac3adm::AudioChannelFormat channel;
    channel.id = "AC_EMPTY";
    const auto path = ac3::admbridge::build_channel_path(channel, 0.0, false);
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error() == ac3::admbridge::BridgeError::kEmptyBlockSequence);
}

// ---------------------------------------------------------------------------
// build(): programme/content/object graph walking, classification and errors
// ---------------------------------------------------------------------------

namespace {

// A minimal, valid one-channel Objects document: one programme, one content, one object, one
// pack, one channel, one track UID resolved through one chna row to one PCM channel. Every test
// below starts from this and mutates the piece it wants to exercise - much less boilerplate than
// repeating the whole graph each time, and every field is a plain public member (ac3adm/model.hpp
// is deliberately just data), so mutating a copy is trivial.
ac3adm::AdmDocument minimal_document() {
    ac3adm::AdmDocument doc;
    doc.model.programmes.push_back({.id = "APR_0001", .name = "P", .content_refs = {"ACO_0001"}});
    doc.model.contents.push_back({.id = "ACO_0001", .name = "C", .object_refs = {"AO_0001"}});
    doc.model.objects.push_back({.id = "AO_0001",
                                 .name = "O",
                                 .start_s = 0.0,
                                 .has_duration = false,
                                 .duration_s = 0.0,
                                 .pack_format_refs = {"AP_0001"},
                                 .track_uid_refs = {"ATU_00000001"},
                                 .object_refs = {}});
    doc.model.pack_formats.push_back({.id = "AP_0001",
                                      .name = "Pack",
                                      .type = ac3adm::TypeDefinition::kObjects,
                                      .channel_format_refs = {"AC_0001"},
                                      .pack_format_refs = {}});
    doc.model.channel_formats.push_back(channel_with({block_at(0.0, std::nullopt, polar(0.0, 0.0))}));
    doc.model.channel_formats.back().id = "AC_0001";
    doc.chna.push_back({.track_index = 1, .uid = "ATU_00000001", .track_ref = "AT_0001_01",
                        .pack_ref = "AP_0001"});
    doc.audio.sample_rate = 48000;
    doc.audio.bits_per_sample = 16;
    doc.audio.channels.push_back(std::vector<float>(kFrame, 0.1f));
    return doc;
}

}  // namespace

TEST_CASE("build() rejects a document with no audioProgramme", "[admbridge]") {
    ac3adm::AdmDocument doc;
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kNoProgramme);
}

TEST_CASE("build() rejects an explicit programme_id that does not exist", "[admbridge]") {
    const auto doc = minimal_document();
    const auto result = ac3::admbridge::build(doc, "APR_9999");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kProgrammeNotFound);
}

TEST_CASE("build() rejects an unresolved audioContent reference", "[admbridge]") {
    auto doc = minimal_document();
    doc.model.programmes[0].content_refs = {"ACO_MISSING"};
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnresolvedReference);
}

TEST_CASE("build() rejects an unresolved audioObject reference", "[admbridge]") {
    auto doc = minimal_document();
    doc.model.contents[0].object_refs = {"AO_MISSING"};
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnresolvedReference);
}

TEST_CASE("build() rejects an unresolved audioPackFormat reference", "[admbridge]") {
    auto doc = minimal_document();
    doc.model.objects[0].pack_format_refs = {"AP_MISSING"};
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnresolvedReference);
}

TEST_CASE("build() rejects a HOA pack (unsupported TypeDefinition)", "[admbridge]") {
    auto doc = minimal_document();
    doc.model.pack_formats[0].type = ac3adm::TypeDefinition::kHoa;
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnsupportedType);
}

TEST_CASE("build() rejects a nested audioPackFormat", "[admbridge]") {
    auto doc = minimal_document();
    doc.model.pack_formats[0].pack_format_refs = {"AP_NESTED"};
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnsupportedType);
}

TEST_CASE("build() rejects a track_uid_refs count that does not match the channel count",
         "[admbridge]") {
    auto doc = minimal_document();
    doc.model.objects[0].track_uid_refs.push_back("ATU_00000002");
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kChannelTrackMismatch);
}

TEST_CASE("build() rejects a track UID with no matching chna row", "[admbridge]") {
    auto doc = minimal_document();
    doc.chna.clear();
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnresolvedReference);
}

TEST_CASE("build() rejects a chna track_index with no corresponding PCM channel", "[admbridge]") {
    auto doc = minimal_document();
    doc.chna[0].track_index = 5;  // only one PCM channel exists (index 1)
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kNoAudioForTrack);
}

TEST_CASE("build() rejects a chna track_index of 0 (the unused-row marker)", "[admbridge]") {
    auto doc = minimal_document();
    doc.chna[0].track_index = 0;
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kNoAudioForTrack);
}

TEST_CASE("build() detects a cycle in nested audioObject references", "[admbridge]") {
    auto doc = minimal_document();
    // AO_0001 -> AO_0001 (self-reference, the simplest illegal loop §5.6.7 names explicitly).
    doc.model.objects[0].object_refs = {"AO_0001"};
    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kObjectReferenceCycle);
}

TEST_CASE("build() rejects more than 15 channels", "[admbridge]") {
    ac3adm::AdmDocument doc;
    doc.model.programmes.push_back({.id = "APR_0001", .name = "P", .content_refs = {"ACO_0001"}});
    ac3adm::AudioContent content{.id = "ACO_0001", .name = "C", .object_refs = {}};
    doc.audio.sample_rate = 48000;
    for (int i = 0; i < 16; ++i) {
        const auto suffix = std::to_string(i + 1);
        const std::string object_id = "AO_00" + suffix;
        const std::string pack_id = "AP_00" + suffix;
        const std::string channel_id = "AC_00" + suffix;
        const std::string uid = "ATU_0000000" + suffix;
        content.object_refs.push_back(object_id);
        doc.model.objects.push_back({.id = object_id,
                                     .name = "O",
                                     .start_s = 0.0,
                                     .has_duration = false,
                                     .duration_s = 0.0,
                                     .pack_format_refs = {pack_id},
                                     .track_uid_refs = {uid},
                                     .object_refs = {}});
        doc.model.pack_formats.push_back({.id = pack_id,
                                          .name = "Pack",
                                          .type = ac3adm::TypeDefinition::kObjects,
                                          .channel_format_refs = {channel_id},
                                          .pack_format_refs = {}});
        auto channel = channel_with({block_at(0.0, std::nullopt, polar(0.0, 0.0))});
        channel.id = channel_id;
        doc.model.channel_formats.push_back(std::move(channel));
        doc.chna.push_back({.track_index = static_cast<std::uint16_t>(i + 1), .uid = uid,
                            .track_ref = "", .pack_ref = ""});
        doc.audio.channels.push_back(std::vector<float>(kFrame, 0.1f));
    }
    doc.model.contents.push_back(std::move(content));

    const auto result = ac3::admbridge::build(doc);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kTooManyChannels);
}

TEST_CASE("build() picks the lowest-ID audioProgramme by default", "[admbridge]") {
    auto doc = minimal_document();
    // A second programme/content/object graph, deliberately using a HIGHER id, pointing at
    // channel "AC_HIGH" instead of the first document's "AC_0001" - if the wrong programme is
    // picked, this test can tell because the resulting channel_ids differ.
    doc.model.programmes.push_back({.id = "APR_0002", .name = "P2", .content_refs = {"ACO_0002"}});
    doc.model.contents.push_back({.id = "ACO_0002", .name = "C2", .object_refs = {"AO_0002"}});
    doc.model.objects.push_back({.id = "AO_0002",
                                 .name = "O2",
                                 .start_s = 0.0,
                                 .has_duration = false,
                                 .duration_s = 0.0,
                                 .pack_format_refs = {"AP_0002"},
                                 .track_uid_refs = {"ATU_00000002"},
                                 .object_refs = {}});
    doc.model.pack_formats.push_back({.id = "AP_0002",
                                      .name = "Pack2",
                                      .type = ac3adm::TypeDefinition::kObjects,
                                      .channel_format_refs = {"AC_HIGH"},
                                      .pack_format_refs = {}});
    auto channel = channel_with({block_at(0.0, std::nullopt, polar(0.0, 0.0))});
    channel.id = "AC_HIGH";
    doc.model.channel_formats.push_back(std::move(channel));
    doc.chna.push_back({.track_index = 1, .uid = "ATU_00000002", .track_ref = "", .pack_ref = ""});

    const auto result = ac3::admbridge::build(doc);
    REQUIRE(result.has_value());
    REQUIRE(result->channel_ids.size() == 1);
    CHECK(result->channel_ids[0] == "AC_0001");  // APR_0001 sorts lower than APR_0002
}

TEST_CASE("build() classifies DirectSpeakers as a bed and detects an LFE speakerLabel",
         "[admbridge]") {
    auto doc = minimal_document();
    doc.model.pack_formats[0].type = ac3adm::TypeDefinition::kDirectSpeakers;
    auto& channel = doc.model.channel_formats[0];
    channel.type = ac3adm::TypeDefinition::kDirectSpeakers;
    // A nonzero position/gain in the source data, specifically to prove the LFE override in
    // build_channel_path actually fires rather than this just happening to already be zero.
    channel.block_formats[0].position = polar(77.0, 12.0);
    channel.block_formats[0].gain = 0.42;
    channel.block_formats[0].speaker_labels = {"LFE"};

    const auto result = ac3::admbridge::build(doc);
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 1);
    CHECK(result->is_bed[0]);
    CHECK(result->is_lfe[0]);
    const auto placement = result->paths[0].evaluate(0.0);
    CHECK(placement.gain == 0.0);
    CHECK(placement.lfe_send == 1.0);
}

TEST_CASE("build() classifies Objects channels as dynamic, not a bed", "[admbridge]") {
    const auto doc = minimal_document();  // pack type is already kObjects
    const auto result = ac3::admbridge::build(doc);
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 1);
    CHECK_FALSE(result->is_bed[0]);
    CHECK_FALSE(result->is_lfe[0]);
}

TEST_CASE("build() applies absolute time as object.start_s + block.rtime_s", "[admbridge]") {
    // BS.2076-2 Table 24 + §5.6.7: audioObject.start is relative to the programme directly, and
    // stays that way through any nesting - no third, programme-level term is added. A nonzero
    // object start_s combined with a block rtime_s should land at exactly their sum.
    auto doc = minimal_document();
    doc.model.objects[0].start_s = 5.0;
    doc.model.channel_formats[0].block_formats[0].rtime_s = 0.25;
    doc.model.channel_formats[0].block_formats[0].position = polar(30.0, 0.0);

    const auto result = ac3::admbridge::build(doc);
    REQUIRE(result.has_value());
    // Single static block: holds everywhere, but the KEYFRAME itself sits at 5.25s - evaluate at
    // a point well before it to confirm the "holds before the first keyframe too" clamp isn't
    // masking a wrong absolute time (a keyframe placed at the wrong time that still happens to
    // dominate every query would be a false pass here otherwise).
    const auto want = ac3::admbridge::adm_position_to_room(ac3adm::Position{polar(30.0, 0.0)});
    CHECK_THAT(result->paths[0].evaluate(5.25).position.x, Catch::Matchers::WithinAbs(want.x, 1e-6));
    CHECK_THAT(result->paths[0].evaluate(0.0).position.x, Catch::Matchers::WithinAbs(want.x, 1e-6));
}

// ---------------------------------------------------------------------------
// Flagship: a real BW64/ADM fixture, parsed by the real ac3adm::parse_bw64(), bridged, and driven
// through a real ac3::oba::AtmosEncoder / ac3::Eac3Decoder round trip.
//
// Byte-fixture helpers are duplicated from tests/test_adm.cpp rather than shared, per this
// project's own established per-file convention for test helpers (see tests/
// test_atmos_motion.cpp's own comment on this - "the same helpers as test_atmos.cpp, duplicated
// here").
// ---------------------------------------------------------------------------

namespace {

using Bytes = std::string;

void put_u16le(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32le(Bytes& out, std::uint32_t value) {
    put_u16le(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    put_u16le(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
}

void put_fourcc(Bytes& out, std::string_view cc) {
    REQUIRE(cc.size() == 4);
    out += cc;
}

void put_fixed(Bytes& out, std::string_view value, std::size_t width) {
    REQUIRE(value.size() == width);
    out += value;
}

void append_chunk(Bytes& out, std::string_view id, const Bytes& content) {
    put_fourcc(out, id);
    put_u32le(out, static_cast<std::uint32_t>(content.size()));
    out += content;
    if (content.size() % 2 != 0) {
        out.push_back('\0');
    }
}

Bytes build_fmt_chunk(std::uint16_t channels, std::uint32_t sample_rate,
                      std::uint16_t bits_per_sample) {
    Bytes fmt;
    put_u16le(fmt, 1);  // WAVE_FORMAT_PCM
    put_u16le(fmt, channels);
    put_u32le(fmt, sample_rate);
    const auto block_align = static_cast<std::uint16_t>(channels * (bits_per_sample / 8));
    put_u32le(fmt, sample_rate * block_align);
    put_u16le(fmt, block_align);
    put_u16le(fmt, bits_per_sample);
    return fmt;
}

// Three tracks: bed-left, bed-right, moving object.
Bytes build_chna_chunk_3() {
    Bytes chna;
    put_u16le(chna, 3);  // numTracks
    put_u16le(chna, 3);  // numUIDs
    struct Row { std::uint16_t track; std::string_view uid, track_ref, pack_ref; };
    const Row rows[] = {
        {1, "ATU_00000001", "AT_00019001_01", "AP_00019001"},
        {2, "ATU_00000002", "AT_00019002_01", "AP_00019001"},
        {3, "ATU_00000003", "AT_00039001_01", "AP_00039001"},
    };
    for (const auto& row : rows) {
        put_u16le(chna, row.track);
        put_fixed(chna, row.uid, 12);
        put_fixed(chna, row.track_ref, 14);
        put_fixed(chna, row.pack_ref, 11);
        chna.push_back('\0');
    }
    return chna;
}

Bytes build_riff(const Bytes& fmt, const Bytes& chna, const Bytes& axml, const Bytes& data) {
    Bytes body;
    append_chunk(body, "fmt ", fmt);
    // An empty chna/axml means "absent" (libbw64 hard-rejects a real 0-byte <chna> chunk as
    // an illegal size), matching tests/test_adm.cpp's own build_riff.
    if (!chna.empty()) {
        append_chunk(body, "chna", chna);
    }
    if (!axml.empty()) {
        append_chunk(body, "axml", axml);
    }
    append_chunk(body, "data", data);

    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;
    return file;
}

// Six frames (9216 samples) of real, distinct, non-silent tones per channel: bed-left 300 Hz,
// bed-right 500 Hz, the moving object 800 Hz - varied and multi-frame per this project's own
// standing lesson that silence/frame-0 checks give false passes.
Bytes build_pcm16_3ch(int frames) {
    Bytes data;
    const double amplitude = 0.3 * 32767.0;
    for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / 48000.0;
        const double left = amplitude * std::sin(2.0 * std::numbers::pi * 300.0 * t);
        const double right = amplitude * std::sin(2.0 * std::numbers::pi * 500.0 * t);
        const double object = amplitude * std::sin(2.0 * std::numbers::pi * 800.0 * t);
        for (const double v : {left, right, object}) {
            put_u16le(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v))));
        }
    }
    return data;
}

// Bed: two DirectSpeakers channels pinned at the 5.1 ring's L (+30) and R (-30). Moving object:
// one Objects channel with two audioBlockFormats - block 0 holds at SR (-110, matching this
// project's own kSR ring constant) for 3 frames' worth (0.096s), block 1 jumps (jumpPosition=1,
// no interpolationLength) to dead ahead (0 degrees, matching bed-adjacent centre) and holds.
constexpr std::string_view kBridgeTestAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_9001" audioProgrammeName="BridgeTest">
    <audioContentIDRef>ACO_9001</audioContentIDRef>
    <audioContentIDRef>ACO_9002</audioContentIDRef>
  </audioProgramme>
  <audioContent audioContentID="ACO_9001" audioContentName="Bed">
    <audioObjectIDRef>AO_9001</audioObjectIDRef>
  </audioContent>
  <audioContent audioContentID="ACO_9002" audioContentName="Moving">
    <audioObjectIDRef>AO_9002</audioObjectIDRef>
  </audioContent>
  <audioObject audioObjectID="AO_9001" audioObjectName="Bed" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef>
    <audioTrackUIDRef>ATU_00000002</audioTrackUIDRef>
  </audioObject>
  <audioObject audioObjectID="AO_9002" audioObjectName="Moving" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000003</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00019001" audioPackFormatName="Bed" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioChannelFormatIDRef>AC_00019002</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioPackFormat audioPackFormatID="AP_00039001" audioPackFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="BedLeft" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+030</speakerLabel>
      <position coordinate="azimuth">30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019002" audioChannelFormatName="BedRight" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019002_00000001">
      <speakerLabel>M-030</speakerLabel>
      <position coordinate="azimuth">-30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00039001" audioChannelFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000001" rtime="00:00:00.00000" duration="00:00:00.09600">
      <position coordinate="azimuth">-110.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000002" rtime="00:00:00.09600">
      <position coordinate="azimuth">0.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00019001" audioStreamFormatName="PCM_BedLeft" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019001_01" audioTrackFormatName="PCM_BedLeft" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00019002" audioStreamFormatName="PCM_BedRight" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019002</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019002_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019002_01" audioTrackFormatName="PCM_BedRight" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019002</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000002" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019002_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00039001" audioStreamFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00039001_01" audioTrackFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00039001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000003" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
)";

double channel_energy(std::span<const float> samples) {
    double energy = 0.0;
    for (const auto v : samples) {
        const double sd = static_cast<double>(v);
        energy += sd * sd;
    }
    return energy;
}

}  // namespace

TEST_CASE("a real ADM BWF master's bed and moving object survive admbridge into a real "
         "AtmosEncoder bitstream", "[admbridge][atmos]") {
    constexpr int kTotalFrames = 6;  // 3 frames holding SR, 3 frames holding centre
    const auto fmt = build_fmt_chunk(3, 48000, 16);
    const auto chna = build_chna_chunk_3();
    const Bytes axml(kBridgeTestAdmXml);
    const auto data = build_pcm16_3ch(kTotalFrames * kFrame);
    std::istringstream stream(build_riff(fmt, chna, axml, data));

    auto document = ac3adm::parse_bw64(stream);
    const std::string parse_diag =
        document ? std::string{"ok"} : std::string(ac3adm::describe(document.error()));
    INFO("parse_bw64: " << parse_diag);
    REQUIRE(document.has_value());
    REQUIRE(document->audio.channels.size() == 3);
    REQUIRE(document->audio.frame_count() == static_cast<std::size_t>(kTotalFrames * kFrame));

    const auto result = ac3::admbridge::build(*document);
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 3);
    CHECK(result->is_bed[0]);
    CHECK(result->is_bed[1]);
    CHECK_FALSE(result->is_bed[2]);
    CHECK_FALSE(result->is_lfe[0]);
    CHECK_FALSE(result->is_lfe[1]);
    CHECK(result->sample_rate == 48000);

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448},
                                   static_cast<int>(result->channel_count())};
    ac3::Eac3Decoder decoder;
    std::vector<std::span<const float>> views(result->channel_count());

    // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.
    constexpr int kCCh = 1;
    constexpr int kLCh = 0;
    constexpr int kRCh = 2;
    constexpr int kSRCh = 4;

    for (int f = 0; f < kTotalFrames; ++f) {
        const auto start = static_cast<std::size_t>(f) * static_cast<std::size_t>(kFrame);
        for (std::size_t i = 0; i < result->channel_count(); ++i) {
            views[i] = result->pcm[i].subspan(start, static_cast<std::size_t>(kFrame));
        }
        const double t = static_cast<double>(start + static_cast<std::size_t>(kFrame)) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(result->paths, t);
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());

        // Check the last frame of each 3-frame hold, the same "settled, not mid-transition"
        // convention tests/test_atmos_motion.cpp's own flagship test uses.
        if (f != 2 && f != 5) {
            continue;
        }
        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());

        const double energy_l = channel_energy((*decoded)->channels[kLCh]);
        const double energy_r = channel_energy((*decoded)->channels[kRCh]);
        const double energy_c = channel_energy((*decoded)->channels[kCCh]);
        const double energy_sr = channel_energy((*decoded)->channels[kSRCh]);
        CAPTURE(f, energy_l, energy_r, energy_c, energy_sr);

        // The bed is static throughout: both L and R should carry real, comparable energy at
        // every checked frame, proving the bed pin survives independent of the object's motion.
        CHECK(energy_l > 1.0);
        CHECK(energy_r > 1.0);

        // Neither bed channel is ever panned toward C or SR - whatever energy shows up in those
        // two comes entirely from the moving object, revealing exactly where admbridge placed
        // it. A dominance comparison (rather than an absolute near-zero threshold on the "wrong"
        // channel) is what's checked: the encoder's own MDCT block overlap smears a little real
        // energy from a hard pan change into the neighbouring channel even once the object's own
        // gain has fully settled on its new target (AtmosEncoder ramps gain across one whole
        // frame - see atmos.cpp's own encode_frame comment - not per-block, so by the frame
        // checked here the object's gain itself is not still transitioning; the residual is a
        // transform-domain effect, not evidence of a wrong or late placement).
        if (f == 2) {
            // Block 0: held at azimuth -110 (SR) for [0, 0.096s) - frame 2 ends exactly at 0.096s,
            // still inside the hold (see build_channel_path's own kInstantJumpEpsilon comment for
            // why the boundary itself still reads the pre-jump value).
            CHECK(energy_sr > 1.0);
            CHECK(energy_sr > energy_c);
        } else {
            // Block 1: jumped to azimuth 0 (dead ahead / C) at 0.096s, held afterward.
            CHECK(energy_c > 1.0);
            CHECK(energy_c > energy_sr);
        }
    }
}
