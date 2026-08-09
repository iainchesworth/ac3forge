#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/spatial/spatial.hpp"

// The plan layer is what stops the two front ends from disagreeing, so what is
// checked here is mostly agreement: that a layout's channel counts match the
// channels it actually codes, that a tool token survives a round trip, and
// that a source reaches the speakers it should reach at the levels it should
// reach them.
//
// The end-to-end cases run REAL audio through the whole chain and decode it
// again. Silence would prove nothing: §7.2.2.1.1 zeroes the bit allocation for
// an all-zero channel, so a frame-layout error survives it intact.

using Catch::Approx;
using ac3::eac3::chanmap::Location;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// A tone at a distinct frequency per channel, so a decoded channel can be
// traced back to the source channel it came from.
void fill_tone(std::vector<float>& out, double hz, double amplitude, std::uint64_t n0) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(
            amplitude * std::sin(kTwoPi * hz * static_cast<double>(n0 + i) / 48000.0));
    }
}

[[nodiscard]] double rms(std::span<const float> samples) {
    double sum = 0.0;
    for (const float value : samples) {
        sum += static_cast<double>(value) * value;
    }
    return samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(samples.size()));
}

// Energy a source channel puts into just the independent substream's channels,
// which is the group that has to be a self-sufficient rendering on its own.
[[nodiscard]] double bed_energy_from(ac3::plan::LayoutId id, const ac3::plan::Routing& routing,
                                     int source) {
    const auto coded = ac3::plan::coded_channels(id);
    double sum = 0.0;
    for (std::size_t c = 0; c < coded.size(); ++c) {
        if (!coded[c].bed) {
            continue;
        }
        const double g = routing.at(static_cast<int>(c), source);
        sum += g * g;
    }
    return sum;
}

}  // namespace

// ---------------------------------------------------------------------------
// The layout table
// ---------------------------------------------------------------------------

TEST_CASE("every layout codes as many channels as it claims") {
    for (const auto& info : ac3::plan::kLayouts) {
        const auto coded = ac3::plan::coded_channels(info.id);
        INFO("layout " << info.name);
        CHECK(static_cast<int>(coded.size()) == info.transmitted);
        CHECK(static_cast<int>(ac3::plan::coded_channel_names(info.id).size()) ==
              info.transmitted);

        // A dependent's channels are the ones its chanmap names, so the
        // dependent count and the coded channels have to tell the same story.
        int substreams = 0;
        for (const auto& channel : coded) {
            substreams = std::max(substreams, channel.substream);
        }
        CHECK(substreams == info.dependents);
    }
}

TEST_CASE("rendered channel counts match what a decoder gets back") {
    // A dependent may replace a bed channel rather than add one, which is why
    // 7.1 codes ten channels for eight speakers. Counting the distinct
    // locations is the only way to check the claim.
    for (const auto& info : ac3::plan::kLayouts) {
        const auto coded = ac3::plan::coded_channels(info.id);
        std::vector<Location> distinct;
        for (const auto& channel : coded) {
            if (std::ranges::find(distinct, channel.location) == distinct.end()) {
                distinct.push_back(channel.location);
            }
        }
        INFO("layout " << info.name);
        CHECK(static_cast<int>(distinct.size()) == info.rendered);
    }
}

TEST_CASE("a replaced bed channel is named apart from the one replacing it") {
    const auto names = ac3::plan::coded_channel_names(ac3::plan::LayoutId::k71);
    REQUIRE(names.size() == 10);
    CHECK(names[3] == "Ls (bed)");
    CHECK(names[4] == "Rs (bed)");
    CHECK(names[6] == "Ls");
    CHECK(names[7] == "Rs");
    // 5.1.2 adds channels rather than replacing any, so nothing is marked.
    for (const auto& name : ac3::plan::coded_channel_names(ac3::plan::LayoutId::k512)) {
        CHECK(name.find("(bed)") == std::string::npos);
    }
}

TEST_CASE("every layout name parses back to itself") {
    for (const auto& info : ac3::plan::kLayouts) {
        const auto parsed = ac3::plan::parse_layout(info.name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == info.id);
    }
    CHECK_FALSE(ac3::plan::parse_layout("714b").has_value());
    CHECK_FALSE(ac3::plan::parse_layout("").has_value());
}

TEST_CASE("the layout list a front end prints is the list the parser accepts") {
    // These two drifting apart is exactly the failure the table exists to stop:
    // a usage line offering a layout the parser rejects, or hiding one it takes.
    const auto listed = ac3::plan::layout_names(ac3::plan::Codec::kAc3);
    CHECK(listed.find("714") == std::string::npos);  // AC-3 has no substreams
    CHECK(listed.find("51") != std::string::npos);

    const auto wide = ac3::plan::layout_names(ac3::plan::Codec::kEac3);
    for (const auto& info : ac3::plan::kLayouts) {
        INFO("layout " << info.name);
        CHECK(wide.find(std::string{info.name}) != std::string::npos);
    }
}

TEST_CASE("Table E2.5 location lists parse and format symmetrically") {
    // Every case round-trips through parse_channels/format_channels exactly
    // as it read - format_channels emits Table E2.5 bit order, so these are
    // already written in that order.
    for (const auto* text : {"L,C,R", "L,C,R,LFE", "L,C,R,Vhl,Vhr,LFE",
                             "L,C,R,Vhc,Lts,Rts,LFE", "Vhc,LFE2,LFE"}) {
        const auto mask = ac3::plan::parse_channels(text);
        REQUIRE(mask.has_value());
        CHECK(ac3::plan::format_channels(*mask) == text);
    }
    // Naming only one half of a pair location is not expressible - Table
    // E2.5 has no bit for Vhl alone.
    CHECK_FALSE(ac3::plan::parse_channels("L,C,R,Vhl").has_value());
    CHECK_FALSE(ac3::plan::parse_channels("").has_value());
    CHECK_FALSE(ac3::plan::parse_channels("Nope").has_value());
    CHECK_FALSE(ac3::plan::parse_channels("L,C,R,").has_value());  // trailing empty token
    CHECK_FALSE(ac3::plan::parse_channels("L,L,C,R").has_value());  // L named twice
}

// ---------------------------------------------------------------------------
// The Annex E tool token
// ---------------------------------------------------------------------------

TEST_CASE("tool tokens survive a round trip") {
    const std::vector<std::string> tokens = {"none",    "cpl",     "spx",       "aht",
                                             "cpl:4",   "spx:5",   "aht:0",     "cpl+spx",
                                             "cpl+spx+aht", "cpl:4+spx:5+aht:2"};
    for (const auto& token : tokens) {
        ac3::plan::Tools tools{};
        INFO("token " << token);
        REQUIRE(ac3::plan::parse_tools(token, tools));
        CHECK(ac3::plan::format_tools(tools) == token);
    }
    // "all" names a set rather than a spelling, so it round-trips to the set.
    ac3::plan::Tools all{};
    REQUIRE(ac3::plan::parse_tools("all", all));
    CHECK(ac3::plan::format_tools(all) == "cpl+spx+aht");
}

TEST_CASE("a tool token out of range is rejected rather than clamped") {
    // Silently clamping would leave a band edge that is not the one asked for,
    // and nothing downstream could tell.
    for (const auto* token : {"cpl:16", "spx:8", "aht:4", "atten:32", "cpl:", "wibble", "cpl+"}) {
        ac3::plan::Tools tools{};
        INFO("token " << token);
        CHECK_FALSE(ac3::plan::parse_tools(token, tools));
    }
}

TEST_CASE("spectral extension attenuation is expressible both ways") {
    ac3::plan::Tools off{};
    REQUIRE(ac3::plan::parse_tools("spx+noatten", off));
    CHECK_FALSE(off.spx_atten);
    CHECK(ac3::plan::format_tools(off) == "spx+noatten");

    ac3::plan::Tools depth{};
    REQUIRE(ac3::plan::parse_tools("spx+atten:12", depth));
    CHECK(depth.spx_atten);
    CHECK(depth.spxattencod == 12);
    CHECK(ac3::plan::format_tools(depth) == "spx+atten:12");
}

// ---------------------------------------------------------------------------
// Configs
// ---------------------------------------------------------------------------

TEST_CASE("an immersive layout is refused to AC-3 rather than silently narrowed") {
    const ac3::plan::Plan plan{.codec = ac3::plan::Codec::kAc3,
                               .layout = ac3::plan::LayoutId::k714};
    const auto error = ac3::plan::validate(plan);
    REQUIRE(error.has_value());
    CHECK(*error == ac3::plan::PlanError::kLayoutNeedsEac3);
    CHECK_FALSE(ac3::plan::carries(ac3::plan::Codec::kAc3, ac3::plan::LayoutId::k714));
    CHECK(ac3::plan::carries(ac3::plan::Codec::kEac3, ac3::plan::LayoutId::k714));
}

TEST_CASE("AC-3 takes only the Table 5.18 rates") {
    ac3::plan::Plan plan{.codec = ac3::plan::Codec::kAc3, .bitrate_kbps = 200};
    REQUIRE(ac3::plan::validate(plan).has_value());
    CHECK(*ac3::plan::validate(plan) == ac3::plan::PlanError::kBitrateNotLegal);
    // E-AC-3 signals frmsiz directly, so the same rate is expressible there.
    plan.codec = ac3::plan::Codec::kEac3;
    CHECK_FALSE(ac3::plan::validate(plan).has_value());
}

TEST_CASE("coupling is dropped where there is nothing to couple") {
    ac3::plan::Plan plan{.codec = ac3::plan::Codec::kAc3, .layout = ac3::plan::LayoutId::kMono};
    plan.tools.coupling = true;
    CHECK_FALSE(ac3::plan::ac3_config(plan).coupling);
    plan.layout = ac3::plan::LayoutId::kStereo;
    CHECK(ac3::plan::ac3_config(plan).coupling);
}

TEST_CASE("7.1.4 asks for two dependent substreams carrying the right channels") {
    ac3::plan::Plan plan{.codec = ac3::plan::Codec::kEac3,
                         .layout = ac3::plan::LayoutId::k714,
                         .bitrate_kbps = 640};
    const auto config = ac3::plan::eac3_config(plan);
    REQUIRE(config.dependents.size() == 2);
    CHECK(config.independent.acmod == ac3::Acmod::k3_2);
    CHECK(config.independent.lfe);
    CHECK(config.dependents[0].chanmap == ac3::eac3::chanmap::k71Rear);
    CHECK(config.dependents[1].chanmap == ac3::eac3::chanmap::kTopQuad);
    // Substreams occupy one frame period, not one frame, so a dependent gets
    // its own slice of the rate rather than a share of the independent's.
    CHECK(config.dependents[0].bitrate_kbps == 320);
    CHECK(config.independent.bitrate_kbps == 640);
}

TEST_CASE("the mixmdate group is written only when it is asked for") {
    ac3::plan::Plan plan{.codec = ac3::plan::Codec::kEac3, .layout = ac3::plan::LayoutId::k51};
    CHECK_FALSE(ac3::plan::eac3_config(plan).independent.mixing.has_value());
    plan.meta.mixmeta = true;
    plan.meta.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
    const auto config = ac3::plan::eac3_config(plan);
    REQUIRE(config.independent.mixing.has_value());
    CHECK(config.independent.mixing->lorocmixlev == ac3::meta::MixLevel::kMinus3dB);
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// Catch2 splits a filter on commas, so no test name here contains one.
TEST_CASE("a source that already is the layout is carried rather than rendered") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k51, 6,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    CHECK(routing->is_permutation());
    // A/52 Table 5.8 order L C R Ls Rs LFE, taken from a WAV's FL FR FC LFE
    // BL BR. Getting this backwards puts the centre in a surround.
    const std::vector<int> expected = {0, 2, 1, 4, 5, 3};
    for (int coded = 0; coded < 6; ++coded) {
        INFO("coded channel " << coded);
        CHECK(routing->at(coded, expected[static_cast<std::size_t>(coded)]) == 1.0);
    }
}

TEST_CASE("stereo in and stereo out is the identity") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::kStereo, 2,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    CHECK(routing->is_permutation());
    CHECK(routing->at(0, 0) == 1.0);
    CHECK(routing->at(1, 1) == 1.0);
}

TEST_CASE("a narrow source does not invent the channels it lacks") {
    // Upmixing stereo into a 5.1 ring would put energy in a centre and
    // surrounds the source never had. The honest answer is silence there, and
    // the meters then show it.
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k51, 2,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    CHECK(routing->at(0, 0) == Approx(1.0));  // L <- L
    CHECK(routing->at(2, 1) == Approx(1.0));  // R <- R
    for (const int coded : {1, 3, 4, 5}) {    // C, Ls, Rs, LFE
        INFO("coded channel " << coded);
        CHECK(routing->at(coded, 0) == Approx(0.0));
        CHECK(routing->at(coded, 1) == Approx(0.0));
    }
}

TEST_CASE("folding 5.1 to stereo follows 7.8 rather than a panner") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::kStereo, 6,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    // §7.8's Lo/Ro sends each surround to its OWN side only. A pairwise pan
    // would bleed it across both, which is audibly a different mix.
    const int wav_bl = 4;
    const int wav_br = 5;
    CHECK(routing->at(0, wav_bl) > 0.0);
    CHECK(routing->at(1, wav_bl) == Approx(0.0));
    CHECK(routing->at(1, wav_br) > 0.0);
    CHECK(routing->at(0, wav_br) == Approx(0.0));
    // The centre reaches both sides equally.
    const int wav_fc = 2;
    CHECK(routing->at(0, wav_fc) == Approx(routing->at(1, wav_fc)));
    CHECK(routing->at(0, wav_fc) > 0.0);
}

TEST_CASE("the surround downmix level actually changes the fold") {
    const auto quiet = ac3::plan::route(ac3::plan::LayoutId::kStereo, 6,
                                        ac3::meta::CentreMixLevel::kMinus4_5dB,
                                        ac3::meta::SurroundMixLevel::kMinus6dB);
    const auto loud = ac3::plan::route(ac3::plan::LayoutId::kStereo, 6,
                                       ac3::meta::CentreMixLevel::kMinus4_5dB,
                                       ac3::meta::SurroundMixLevel::kMinus3dB);
    const auto off = ac3::plan::route(ac3::plan::LayoutId::kStereo, 6,
                                      ac3::meta::CentreMixLevel::kMinus4_5dB,
                                      ac3::meta::SurroundMixLevel::kSilent);
    REQUIRE(quiet.has_value());
    REQUIRE(loud.has_value());
    REQUIRE(off.has_value());
    CHECK(loud->at(0, 4) > quiet->at(0, 4));
    CHECK(off->at(0, 4) == Approx(0.0));
}

TEST_CASE("the LFE is routed by name and never panned") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k514, 6,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    const auto coded = ac3::plan::coded_channels(ac3::plan::LayoutId::k514);
    const int wav_lfe = 3;
    for (std::size_t c = 0; c < coded.size(); ++c) {
        const bool lfe = coded[c].location == Location::kLfe;
        INFO("coded channel " << c);
        // The LFE carries the source's LFE at unity and nothing else; no other
        // channel receives any of it.
        CHECK(routing->at(static_cast<int>(c), wav_lfe) == Approx(lfe ? 1.0 : 0.0));
        if (lfe) {
            for (int s = 0; s < routing->source_channels; ++s) {
                if (s != wav_lfe) {
                    CHECK(routing->at(static_cast<int>(c), s) == Approx(0.0));
                }
            }
        }
    }
}

TEST_CASE("the bed stays a self-sufficient rendering of the whole programme") {
    // §E1.3.1: a decoder that ignores every dependent still has to hear the
    // programme. So each directional source channel must arrive in the bed at
    // full energy even when a dependent will later replace the channel it
    // landed on. The source here is flat 5.1 - a height channel deliberately
    // does NOT reach the bed, which the ceiling test above covers.
    for (const auto id : {ac3::plan::LayoutId::k71, ac3::plan::LayoutId::k512,
                          ac3::plan::LayoutId::k514, ac3::plan::LayoutId::k714}) {
        const auto routing = ac3::plan::route(id, 6, ac3::meta::CentreMixLevel::kMinus4_5dB,
                                              ac3::meta::SurroundMixLevel::kMinus6dB);
        REQUIRE(routing.has_value());
        INFO("layout " << ac3::plan::layout(id).name);
        for (int s = 0; s < routing->source_channels; ++s) {
            if (s == 3) {
                continue;  // the LFE, which has no direction to preserve
            }
            INFO("source channel " << s);
            CHECK(bed_energy_from(id, *routing, s) == Approx(1.0).margin(1e-9));
        }
    }
}

TEST_CASE("a 7.1 source reaches the rears the 5.1 bed cannot hold") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k71, 8,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    REQUIRE(routing->source_channels == 8);
    REQUIRE(routing->coded_channels == 10);

    // WAV 7.1 order is FL FR FC LFE BL BR SL SR, so the rears are 4/5 and the
    // sides 6/7.
    const int wav_bl = 4;
    const int wav_sl = 6;
    const auto coded = ac3::plan::coded_channels(ac3::plan::LayoutId::k71);
    // The dependent's Ls takes the source's side surround alone, and its Lrs
    // the source's rear: at 7.1 geometry those directions coincide exactly.
    int dep_ls = -1;
    int dep_lrs = -1;
    int bed_ls = -1;
    for (std::size_t c = 0; c < coded.size(); ++c) {
        if (coded[c].location == Location::kLeftSurround) {
            (coded[c].bed ? bed_ls : dep_ls) = static_cast<int>(c);
        } else if (coded[c].location == Location::kLrs) {
            dep_lrs = static_cast<int>(c);
        }
    }
    REQUIRE(dep_ls >= 0);
    REQUIRE(dep_lrs >= 0);
    REQUIRE(bed_ls >= 0);
    CHECK(routing->at(dep_ls, wav_sl) == Approx(1.0));
    CHECK(routing->at(dep_lrs, wav_bl) == Approx(1.0));
    // The bed's surround, meanwhile, has to carry BOTH of them: it is the only
    // place a 5.1 decoder can hear the rear content at all.
    CHECK(routing->at(bed_ls, wav_sl) > 0.0);
    CHECK(routing->at(bed_ls, wav_bl) > 0.0);
}

TEST_CASE("a source with height reaches the ceiling and nowhere else") {
    // Ten channels against a 5.1.4 target is the exact-match path, so every
    // channel should arrive at its own speaker untouched.
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k514, 10,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    CHECK(routing->is_permutation());

    // The height channels a dependent ADDS must not also be folded into the
    // bed: the dependent does not replace anything, so a 5.1.4 decoder plays
    // both and would hear the ceiling twice - once above and once on the ring.
    const auto coded = ac3::plan::coded_channels(ac3::plan::LayoutId::k514);
    const int wav_tfl = 6;  // FL FR FC LFE SL SR TFL TFR TBL TBR
    int dep_vhl = -1;
    for (std::size_t c = 0; c < coded.size(); ++c) {
        if (coded[c].location == Location::kVhl && !coded[c].bed) {
            dep_vhl = static_cast<int>(c);
        } else {
            INFO("coded channel " << c);
            CHECK(routing->at(static_cast<int>(c), wav_tfl) == Approx(0.0).margin(1e-12));
        }
    }
    REQUIRE(dep_vhl >= 0);
    CHECK(routing->at(dep_vhl, wav_tfl) == Approx(1.0));
}

TEST_CASE("a flat source leaves the ceiling silent rather than inventing height") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k514, 6,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());
    const auto coded = ac3::plan::coded_channels(ac3::plan::LayoutId::k514);
    for (std::size_t c = 0; c < coded.size(); ++c) {
        const auto location = coded[c].location;
        if (location != Location::kVhl && location != Location::kVhr &&
            location != Location::kLts && location != Location::kRts) {
            continue;
        }
        for (int s = 0; s < routing->source_channels; ++s) {
            INFO("coded " << c << " source " << s);
            CHECK(routing->at(static_cast<int>(c), s) == Approx(0.0));
        }
    }
}

TEST_CASE("no speaker is sent more than full scale") {
    // Where several source channels land on one speaker their coefficients
    // sum, and a sum above unity clips. §7.8.1 answers this by attenuating
    // every coefficient equally, which is what route() has to do too: folding
    // a 5.1.4 source's ceiling into a 7.1.4 bed measured +1.5 dBFS without it.
    for (const auto& info : ac3::plan::kLayouts) {
        for (const std::size_t channels : {std::size_t{2}, std::size_t{6}, std::size_t{8},
                                           std::size_t{10}, std::size_t{12}}) {
            const auto routing =
                ac3::plan::route(info.id, channels, ac3::meta::CentreMixLevel::kMinus3dB,
                                 ac3::meta::SurroundMixLevel::kMinus3dB);
            REQUIRE(routing.has_value());
            for (int c = 0; c < routing->coded_channels; ++c) {
                double sum = 0.0;
                for (int s = 0; s < routing->source_channels; ++s) {
                    sum += std::abs(routing->at(c, s));
                }
                INFO("layout " << info.name << ", " << channels << " source channels, coded "
                               << c);
                CHECK(sum <= 1.0 + 1e-9);
            }
        }
    }
}

TEST_CASE("normalising the fold does not disturb a source that needs no fold") {
    // The attenuation is a global scale, so it must be exactly 1 wherever no
    // speaker is oversubscribed - otherwise every ordinary encode would come
    // out quieter than its source for no reason.
    for (const auto& info : ac3::plan::kLayouts) {
        const auto routing = ac3::plan::route(info.id,
                                              static_cast<std::size_t>(info.rendered),
                                              ac3::meta::CentreMixLevel::kMinus4_5dB,
                                              ac3::meta::SurroundMixLevel::kMinus6dB);
        REQUIRE(routing.has_value());
        INFO("layout " << info.name);
        // Every coded channel still receives some source at unity: the bed
        // channels a dependent replaces carry a fold, but nothing is scaled.
        double loudest = 0.0;
        for (int c = 0; c < routing->coded_channels; ++c) {
            for (int s = 0; s < routing->source_channels; ++s) {
                loudest = std::max(loudest, routing->at(c, s));
            }
        }
        CHECK(loudest == Approx(1.0));
    }
}

TEST_CASE("a source width no speaker layout has is refused") {
    for (const std::size_t channels : {std::size_t{0}, std::size_t{7}, std::size_t{13}}) {
        INFO(channels << " channels");
        CHECK_FALSE(ac3::plan::route(ac3::plan::LayoutId::k51, channels,
                                     ac3::meta::CentreMixLevel::kMinus4_5dB,
                                     ac3::meta::SurroundMixLevel::kMinus6dB)
                        .has_value());
    }
}

TEST_CASE("render applies the routing it was given") {
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k51, 2,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());

    constexpr std::size_t kSamples = 64;
    std::vector<std::vector<float>> source(2, std::vector<float>(kSamples));
    fill_tone(source[0], 1000.0, 0.5, 0);
    fill_tone(source[1], 3000.0, 0.5, 0);
    std::vector<std::span<const float>> in{source[0], source[1]};

    std::vector<std::vector<float>> out(6, std::vector<float>(kSamples, 7.0f));
    std::vector<std::span<float>> views;
    for (auto& channel : out) {
        views.emplace_back(channel);
    }
    ac3::plan::render(*routing, in, views, kSamples);

    CHECK(rms(out[0]) == Approx(rms(source[0])));
    CHECK(rms(out[2]) == Approx(rms(source[1])));
    // The untouched channels must be CLEARED, not left holding whatever the
    // buffer had: a stale block would encode as real audio.
    for (const int silent : {1, 3, 4, 5}) {
        INFO("coded channel " << silent);
        CHECK(rms(out[static_cast<std::size_t>(silent)]) == Approx(0.0));
    }
}

// ---------------------------------------------------------------------------
// The generalised panner
// ---------------------------------------------------------------------------

TEST_CASE("panning an arbitrary ring agrees with the 5.1 panner") {
    // pan_azimuth is the 5.1 special case of pan_ring; if they disagree, a 5.1
    // bed rendered through the plan layer would differ from one rendered
    // through the object layer for the same direction.
    const std::vector<double> ring(ac3::spatial::kSpeakerAzimuthDeg.begin(),
                                   ac3::spatial::kSpeakerAzimuthDeg.end());
    for (double azimuth = -180.0; azimuth < 180.0; azimuth += 7.0) {
        std::vector<double> gains(ring.size());
        ac3::spatial::pan_ring(azimuth, ring, gains);
        const auto reference = ac3::spatial::pan_azimuth(azimuth);
        INFO("azimuth " << azimuth);
        for (std::size_t ch = 0; ch < gains.size(); ++ch) {
            CHECK(gains[ch] == Approx(reference[ch]).margin(1e-12));
        }
    }
}

TEST_CASE("a ring pan preserves energy at every angle") {
    const std::vector<double> ring = {30.0, -30.0, 90.0, -90.0, 150.0, -150.0};
    for (double azimuth = 0.0; azimuth < 360.0; azimuth += 3.0) {
        std::vector<double> gains(ring.size());
        ac3::spatial::pan_ring(azimuth, ring, gains);
        double energy = 0.0;
        for (const double g : gains) {
            CHECK(g >= 0.0);
            energy += g * g;
        }
        INFO("azimuth " << azimuth);
        CHECK(energy == Approx(1.0).margin(1e-9));
    }
}

TEST_CASE("a direction no pair encloses is not dropped into silence") {
    // Two front speakers leave the whole rear arc unenclosed. The pairwise
    // system is singular there and solves negative on both sides, so without
    // the constant-power fallback a rear source would vanish entirely.
    const std::vector<double> pair = {30.0, -30.0};
    for (double azimuth = 100.0; azimuth <= 260.0; azimuth += 10.0) {
        std::vector<double> gains(pair.size());
        ac3::spatial::pan_ring(azimuth, pair, gains);
        double energy = 0.0;
        for (const double g : gains) {
            energy += g * g;
        }
        INFO("azimuth " << azimuth);
        CHECK(energy == Approx(1.0).margin(1e-9));
    }
    // Directly behind, it sits equally in both.
    std::vector<double> behind(pair.size());
    ac3::spatial::pan_ring(180.0, pair, behind);
    CHECK(behind[0] == Approx(behind[1]));
}

// ---------------------------------------------------------------------------
// End to end: a plan, real audio, and the stream decoded again
// ---------------------------------------------------------------------------

TEST_CASE("every E-AC-3 layout encodes real audio and decodes back to its speakers") {
    // Three frames, not one: frame 0 is a fade-in from an empty MDCT overlap
    // rather than steady state, so a layout error that only shows once the
    // overlap is primed would survive a single-frame test.
    constexpr int kFrames = 4;
    constexpr int kSkipFrames = 2;

    for (const auto& info : ac3::plan::kLayouts) {
        if (info.id == ac3::plan::LayoutId::kMono) {
            continue;  // a mono bed has no directions to check
        }
        const ac3::plan::Plan plan{.codec = ac3::plan::Codec::kEac3,
                                   .layout = info.id,
                                   .bitrate_kbps = 640};
        const auto routing =
            ac3::plan::route(info.id, static_cast<std::size_t>(info.rendered),
                             ac3::meta::CentreMixLevel::kMinus4_5dB,
                             ac3::meta::SurroundMixLevel::kMinus6dB);
        REQUIRE(routing.has_value());

        ac3::eac3::AccessUnitEncoder encoder{ac3::plan::eac3_config(plan)};
        REQUIRE(encoder.channel_count() == info.transmitted);

        const auto nsource = static_cast<std::size_t>(routing->source_channels);
        // §7.3.1 codes seven LFE mantissas and nothing above about 120 Hz, so
        // the channel that lands there has to be fed something it can carry -
        // a mid tone would come back as silence and look like a routing fault.
        int lfe_source = -1;
        {
            const auto coded_list = ac3::plan::coded_channels(info.id);
            for (std::size_t c = 0; c < coded_list.size() && lfe_source < 0; ++c) {
                if (coded_list[c].location != Location::kLfe) {
                    continue;
                }
                for (int s = 0; s < routing->source_channels; ++s) {
                    if (routing->at(static_cast<int>(c), s) > 0.0) {
                        lfe_source = s;
                        break;
                    }
                }
            }
        }
        std::vector<std::vector<float>> source(nsource,
                                               std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::vector<float>> coded(
            static_cast<std::size_t>(info.transmitted),
            std::vector<float>(ac3::kSamplesPerFrame));

        std::vector<std::byte> stream;
        std::uint64_t n0 = 0;
        for (int frame = 0; frame < kFrames; ++frame) {
            std::vector<std::span<const float>> in;
            for (std::size_t ch = 0; ch < nsource; ++ch) {
                // Well separated tones, none a harmonic of another, so a
                // channel that ends up in the wrong speaker is unmistakable.
                const double hz = static_cast<int>(ch) == lfe_source
                                      ? 55.0
                                      : 400.0 + 233.0 * static_cast<double>(ch);
                fill_tone(source[ch], hz, 0.35, n0);
                in.emplace_back(source[ch]);
            }
            std::vector<std::span<float>> out;
            for (auto& channel : coded) {
                out.emplace_back(channel);
            }
            ac3::plan::render(*routing, in, out, ac3::kSamplesPerFrame);

            std::vector<std::span<const float>> encoded;
            for (const auto& channel : coded) {
                encoded.emplace_back(channel);
            }
            const auto unit = encoder.encode_access_unit(encoded);
            INFO("layout " << info.name << " frame " << frame);
            REQUIRE(unit.has_value());
            stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
            n0 += ac3::kSamplesPerFrame;
        }

        const auto units = ac3::split_access_units(stream);
        INFO("layout " << info.name);
        REQUIRE(units.has_value());
        REQUIRE(units->size() == kFrames);

        ac3::Eac3Decoder decoder;
        std::vector<std::vector<float>> pcm;
        for (int frame = 0; frame < kFrames; ++frame) {
            const auto decoded = decoder.decode_access_unit((*units)[static_cast<std::size_t>(frame)]);
            REQUIRE(decoded.has_value());
            CHECK(static_cast<int>(decoded->channels.size()) == info.rendered);
            if (frame < kSkipFrames) {
                continue;
            }
            if (pcm.empty()) {
                pcm.resize(decoded->channels.size());
            }
            for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
                pcm[ch].insert(pcm[ch].end(), decoded->channels[ch].begin(),
                               decoded->channels[ch].end());
            }
        }

        // Every rendered channel the routing feeds must come back carrying
        // audio. This is the check silence could never make: with an all-zero
        // input the bit allocation is zero too and the frame decodes "fine"
        // whatever its layout says.
        for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
            INFO("layout " << info.name << " rendered channel " << ch);
            CHECK(rms(pcm[ch]) > 0.01);
        }
    }
}

TEST_CASE("a custom channel selection encodes real audio and decodes back to its speakers") {
    // L,C,R,LFE,Vhl,Vhr,Lts,Rts: no named layout offers this shape - since
    // Ls/Rs were never asked for, the bed narrows to 3/0+LFE rather than the
    // 3/2+LFE every preset uses, with a single dependent carrying all four
    // ceiling channels. Proving this exercises allocate() itself, not one of
    // the seven hand-picked presets.
    constexpr int kFrames = 4;
    constexpr int kSkipFrames = 2;

    const auto locations = ac3::plan::parse_channels("L,C,R,LFE,Vhl,Vhr,Lts,Rts");
    REQUIRE(locations.has_value());
    const ac3::plan::Plan plan{
        .codec = ac3::plan::Codec::kEac3, .custom_locations = locations, .bitrate_kbps = 448};
    REQUIRE_FALSE(ac3::plan::validate(plan).has_value());

    const auto cp = ac3::plan::resolve(plan);
    CHECK(cp.bed_acmod == ac3::Acmod::k3_0);
    CHECK(cp.bed_lfe);
    REQUIRE(cp.dependents.size() == 1);
    const auto rendered = ac3::plan::rendered_channel_count(cp);

    const auto routing =
        ac3::plan::route(cp, static_cast<std::size_t>(rendered),
                         ac3::meta::CentreMixLevel::kMinus4_5dB,
                         ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());

    ac3::eac3::AccessUnitEncoder encoder{ac3::plan::eac3_config(plan)};
    REQUIRE(static_cast<std::size_t>(encoder.channel_count()) ==
            ac3::plan::coded_channels(cp).size());

    // §7.3.1 codes seven LFE mantissas and nothing above about 120 Hz, so the
    // channel that lands there has to be fed something it can carry.
    int lfe_source = -1;
    {
        const auto coded_list = ac3::plan::coded_channels(cp);
        for (std::size_t c = 0; c < coded_list.size() && lfe_source < 0; ++c) {
            if (coded_list[c].location != Location::kLfe) {
                continue;
            }
            for (int s = 0; s < routing->source_channels; ++s) {
                if (routing->at(static_cast<int>(c), s) > 0.0) {
                    lfe_source = s;
                    break;
                }
            }
        }
    }

    const auto nsource = static_cast<std::size_t>(routing->source_channels);
    std::vector<std::vector<float>> source(nsource, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> coded(static_cast<std::size_t>(routing->coded_channels),
                                          std::vector<float>(ac3::kSamplesPerFrame));

    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int frame = 0; frame < kFrames; ++frame) {
        std::vector<std::span<const float>> in;
        for (std::size_t ch = 0; ch < nsource; ++ch) {
            const double hz = static_cast<int>(ch) == lfe_source
                                  ? 55.0
                                  : 400.0 + 233.0 * static_cast<double>(ch);
            fill_tone(source[ch], hz, 0.35, n0);
            in.emplace_back(source[ch]);
        }
        std::vector<std::span<float>> out;
        for (auto& channel : coded) {
            out.emplace_back(channel);
        }
        ac3::plan::render(*routing, in, out, ac3::kSamplesPerFrame);

        std::vector<std::span<const float>> encoded;
        for (const auto& channel : coded) {
            encoded.emplace_back(channel);
        }
        const auto unit = encoder.encode_access_unit(encoded);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
        n0 += ac3::kSamplesPerFrame;
    }

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == kFrames);

    ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> pcm;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto decoded = decoder.decode_access_unit((*units)[static_cast<std::size_t>(frame)]);
        REQUIRE(decoded.has_value());
        CHECK(static_cast<int>(decoded->channels.size()) == rendered);
        if (frame < kSkipFrames) {
            continue;
        }
        if (pcm.empty()) {
            pcm.resize(decoded->channels.size());
        }
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), decoded->channels[ch].begin(),
                           decoded->channels[ch].end());
        }
    }
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        INFO("rendered channel " << ch);
        CHECK(rms(pcm[ch]) > 0.01);
    }
}

TEST_CASE("an AC-3 plan encodes real audio the AC-3 decoder reads back") {
    const ac3::plan::Plan plan{.codec = ac3::plan::Codec::kAc3,
                               .layout = ac3::plan::LayoutId::k51,
                               .bitrate_kbps = 448};
    const auto routing = ac3::plan::route(ac3::plan::LayoutId::k51, 6,
                                          ac3::meta::CentreMixLevel::kMinus4_5dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB);
    REQUIRE(routing.has_value());

    ac3::FrameEncoder encoder{ac3::plan::ac3_config(plan)};
    std::vector<std::vector<float>> source(6, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> coded(6, std::vector<float>(ac3::kSamplesPerFrame));

    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int frame = 0; frame < 4; ++frame) {
        std::vector<std::span<const float>> in;
        for (std::size_t ch = 0; ch < source.size(); ++ch) {
            fill_tone(source[ch], 400.0 + 233.0 * static_cast<double>(ch), 0.35, n0);
            in.emplace_back(source[ch]);
        }
        std::vector<std::span<float>> out;
        for (auto& channel : coded) {
            out.emplace_back(channel);
        }
        ac3::plan::render(*routing, in, out, ac3::kSamplesPerFrame);

        std::vector<std::span<const float>> encoded;
        for (const auto& channel : coded) {
            encoded.emplace_back(channel);
        }
        const auto bytes = encoder.encode_frame(encoded);
        REQUIRE(bytes.has_value());
        stream.insert(stream.end(), bytes->begin(), bytes->end());
        n0 += ac3::kSamplesPerFrame;
    }

    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 4);
    ac3::FrameDecoder decoder;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        REQUIRE(decoded.has_value());
        CHECK(decoded->acmod == ac3::Acmod::k3_2);
        CHECK(decoded->lfe);
    }
}
