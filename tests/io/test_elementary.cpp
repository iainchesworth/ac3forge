#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

std::vector<std::vector<float>> tone(int channels) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (auto& channel : pcm) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            channel[static_cast<std::size_t>(i)] = static_cast<float>(
                0.4 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
        }
    }
    return pcm;
}

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace

TEST_CASE("scan reads an AC-3 elementary stream", "[elementary]") {
    // Real audio, so the frames are full rather than mostly padding - a
    // sizing bug that only shows on a full frame would otherwise hide.
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    std::vector<std::byte> stream;
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    for (int f = 0; f < 4; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    CHECK(scanned->kind == ac3::io::StreamKind::kAc3);
    CHECK(scanned->sample_rate == ac3::SampleRate::k48000);
    CHECK(scanned->acmod == ac3::Acmod::k3_2);
    CHECK(scanned->lfe);
    CHECK(scanned->channels == 6);
    CHECK(scanned->access_units.size() == 4);
    // AC-3 has no substreams: one syncframe is one access unit.
    CHECK(scanned->substreams_per_unit == 1);
}

TEST_CASE("scan reads E-AC-3 substream layouts", "[elementary]") {
    using ac3::eac3::AccessUnitConfig;
    using ac3::eac3::FrameConfig;
    namespace cm = ac3::eac3::chanmap;

    const AccessUnitConfig bed{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};

    // 7.1: the dependent's Ls/Rs overwrite the bed's and Lrs/Rrs extend it, so
    // ten coded channels render as eight. Counting locations rather than
    // channels is the only way to get that right.
    AccessUnitConfig seven_one = bed;
    seven_one.dependents.push_back({.bitrate_kbps = 224,
                                    .acmod = ac3::Acmod::k2_2,
                                    .chanmap = cm::k71Rear});

    // 5.1.4: four ceiling channels, nothing replaced.
    AccessUnitConfig five_one_four = bed;
    five_one_four.dependents.push_back({.bitrate_kbps = 224,
                                        .acmod = ac3::Acmod::k2_2,
                                        .chanmap = cm::kTopQuad});

    // 7.1.4 needs two dependents - six new channels, one more than a single
    // substream can carry.
    AccessUnitConfig seven_one_four = seven_one;
    seven_one_four.dependents.push_back({.bitrate_kbps = 224,
                                         .acmod = ac3::Acmod::k2_2,
                                         .chanmap = cm::kTopQuad});

    struct Case {
        AccessUnitConfig config;
        int channels;
        std::size_t substreams;
    };
    for (const auto& [config, channels, substreams] :
         {Case{bed, 6, 1}, Case{seven_one, 8, 2}, Case{five_one_four, 10, 2},
          Case{seven_one_four, 12, 3}}) {
        const auto unit = ac3::eac3::build_silent_access_unit(config);
        REQUIRE(unit.has_value());
        std::vector<std::byte> stream;
        for (int f = 0; f < 3; ++f) {
            append(stream, unit->bytes);
        }

        const auto scanned = ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        CHECK(scanned->kind == ac3::io::StreamKind::kEac3);
        CHECK(scanned->channels == channels);
        CHECK(scanned->substreams_per_unit == substreams);
        // Substreams group into access units, not one frame each.
        CHECK(scanned->access_units.size() == 3);
        CHECK(scanned->access_units.front().size() == unit->bytes.size());
    }
}

TEST_CASE("scan tells AC-3 and E-AC-3 apart by bsid", "[elementary]") {
    // Both formats spend exactly 40 bits before bsid, which is what makes
    // identifying the stream possible without being told which it is.
    const auto legacy = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(legacy.has_value());
    const auto plus = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(plus.has_value());

    const auto a = ac3::io::scan(*legacy);
    REQUIRE(a.has_value());
    CHECK(a->kind == ac3::io::StreamKind::kAc3);

    const auto e = ac3::io::scan(*plus);
    REQUIRE(e.has_value());
    CHECK(e->kind == ac3::io::StreamKind::kEac3);

    // Both open on the same sync word, so the discrimination really is coming
    // from bsid and not from anything earlier in the frame.
    CHECK(std::to_integer<std::uint8_t>(legacy->at(0)) == 0x0B);
    CHECK(std::to_integer<std::uint8_t>(plus->at(0)) == 0x0B);
}

TEST_CASE("scan refuses what it cannot read", "[elementary]") {
    using ac3::io::ScanError;
    CHECK(ac3::io::scan({}).error() == ScanError::kEmpty);

    const auto frame = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());

    // A stream that does not start on a sync word is not a stream.
    std::vector<std::byte> unsynced{std::byte{0x00}, std::byte{0x00}};
    unsynced.insert(unsynced.end(), frame->begin(), frame->end());
    CHECK(ac3::io::scan(unsynced).error() == ScanError::kLostSync);

    // A frame cut short must be reported, not silently returned as a short
    // access unit - a muxer that packetised it would produce a broken file.
    const std::vector<std::byte> cut{frame->begin(), frame->end() - 64};
    CHECK(ac3::io::scan(cut).error() == ScanError::kTruncated);

    // Trailing garbage after a whole frame is a lost sync, not a silent stop.
    std::vector<std::byte> trailing{frame->begin(), frame->end()};
    trailing.insert(trailing.end(), 8, std::byte{0xAB});
    CHECK(ac3::io::scan(trailing).error() == ScanError::kLostSync);
}

// bsid/bsmod/bit_rate_code and the TS 103 420 addbsi Atmos marker exist
// purely for ac3::io::build_codec_config_box() (ac3/io/dec3.hpp) to build a
// spec-correct dac3/dec3 box - see tests/containers/test_mp4.cpp for the box byte
// layout itself. These check the values scan() reports for them.
TEST_CASE("scan reads bsid/bsmod/bit_rate_code straight off the bsi", "[elementary]") {
    // A/52 encoder.cpp always writes bsid 8; real audio, not silence, per
    // this project's own validation discipline - a bit offset error inside
    // bsi could otherwise still land on a plausible-looking bsmod by luck.
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    auto pcm = tone(2);
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
    std::vector<std::byte> stream;
    for (int f = 0; f < 4; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    CHECK(scanned->bsid == 8);
    CHECK(scanned->bsmod == 0);  // "main audio service: complete main" - the default
    // 192 kbps is kBitratesKbps[10] (Table 5.18); frmsizecod's own top bits
    // are exactly that index.
    CHECK(scanned->bit_rate_code == 10);
    CHECK_FALSE(scanned->oba_complexity_index.has_value());
}

TEST_CASE("scan reads the addbsi Dolby Atmos marker", "[elementary]") {
    // Real, multi-frame object audio (not build_silent_access_unit): addbsi
    // parsing has to land on the right bit offset regardless of what audio
    // rides along after it, but going through the real encoder is what this
    // project's tests do whenever an actual FrameEncoder is available for
    // the case - see "scan reads an AC-3 elementary stream" above.
    constexpr int kObjects = 2;
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
    std::vector<std::vector<float>> sources(kObjects,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (auto& source : sources) {
        views.emplace_back(source);
    }
    std::array<ac3::oba::ObjectPlacement, kObjects> placement{};
    for (auto& p : placement) {
        p = {.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }

    std::vector<std::byte> stream;
    for (int f = 0; f < 3; ++f) {
        for (int obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (f * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[static_cast<std::size_t>(obj)][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi *
                                                      (440.0 * static_cast<double>(obj + 1)) * t));
            }
        }
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        append(stream, unit->bytes);
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->oba_complexity_index.has_value());
    // §8.3.2.2: object_count is bed-first (the LFE, always present in this
    // encoder's dynamic-only program) then the dynamic objects.
    CHECK(*scanned->oba_complexity_index == kObjects + 1);
}
