#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"

// The in-repo E-AC-3 decoder is 7.1.4's only oracle. FFmpeg refuses any frame
// with substreamid != 0 in ff_ac3_parse_header, and no container works around
// that, so a program with two dependent substreams cannot be checked against
// it at all. Everything narrower IS checked against FFmpeg (float32 parity,
// ~1.4e-5 worst case, tools/ scripts); these tests are what carries the
// guarantee across to the layout that has no second opinion.

namespace {

using ac3::eac3::chanmap::Location;

constexpr double kAmplitude = 0.4;

struct Speaker {
    Location location;
    double tone_hz;
};

struct LayoutCase {
    std::string_view name;
    ac3::eac3::AccessUnitConfig config;
    std::vector<double> tones;      // one per CODED channel, transmission order
    std::vector<Speaker> speakers;  // one per RENDERED channel, Table E2.5 order
};

// The bed every layout wider than stereo builds on: L C R Ls Rs LFE.
ac3::eac3::FrameConfig bed(std::uint32_t kbps) {
    return {.bitrate_kbps = kbps, .acmod = ac3::Acmod::k3_2, .lfe = true};
}

// The same layouts and tones the CLI emits (see eac3_layout in src/cli).
// Deliberately, the rear dependent's Ls/Rs tones are NOT the bed's: identical
// ones could not tell §E3.8.2's overwrite happening apart from the dependent
// being ignored altogether.
std::vector<LayoutCase> layout_cases() {
    using ac3::Acmod;
    namespace cm = ac3::eac3::chanmap;
    const ac3::eac3::FrameConfig rear{
        .bitrate_kbps = 320, .acmod = Acmod::k2_2, .chanmap = cm::k71Rear};
    const ac3::eac3::FrameConfig top{
        .bitrate_kbps = 320, .acmod = Acmod::k2_2, .chanmap = cm::kTopQuad};
    const ac3::eac3::FrameConfig height{
        .bitrate_kbps = 320, .acmod = Acmod::k2_0, .chanmap = cm::k512Height};

    const std::vector<double> bed_tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const std::vector<Speaker> bed_speakers = {
        {Location::kLeft, 1000.0},         {Location::kCentre, 800.0},
        {Location::kRight, 1200.0},        {Location::kLeftSurround, 600.0},
        {Location::kRightSurround, 1400.0}, {Location::kLfe, 60.0}};

    std::vector<LayoutCase> cases;
    cases.push_back({.name = "stereo",
                     .config = {.independent = {.bitrate_kbps = 640}},
                     .tones = {1000.0, 1000.0},
                     .speakers = {{Location::kLeft, 1000.0}, {Location::kRight, 1000.0}}});
    cases.push_back({.name = "5.1",
                     .config = {.independent = bed(640)},
                     .tones = bed_tones,
                     .speakers = bed_speakers});
    cases.push_back(
        {.name = "7.1",
         .config = {.independent = bed(640), .dependents = {rear}},
         .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 500.0, 1600.0, 400.0, 1800.0},
         .speakers = {{Location::kLeft, 1000.0},
                      {Location::kCentre, 800.0},
                      {Location::kRight, 1200.0},
                      {Location::kLeftSurround, 500.0},   // overwritten, not 600
                      {Location::kRightSurround, 1600.0},  // overwritten, not 1400
                      {Location::kLrs, 400.0},
                      {Location::kRrs, 1800.0},
                      {Location::kLfe, 60.0}}});
    cases.push_back({.name = "5.1.2",
                     .config = {.independent = bed(640), .dependents = {height}},
                     .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 2400.0},
                     .speakers = {{Location::kLeft, 1000.0},
                                  {Location::kCentre, 800.0},
                                  {Location::kRight, 1200.0},
                                  {Location::kLeftSurround, 600.0},
                                  {Location::kRightSurround, 1400.0},
                                  {Location::kVhl, 2000.0},
                                  {Location::kVhr, 2400.0},
                                  {Location::kLfe, 60.0}}});
    cases.push_back({.name = "5.1.4",
                     .config = {.independent = bed(640), .dependents = {top}},
                     .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 2400.0,
                               2800.0, 3200.0},
                     .speakers = {{Location::kLeft, 1000.0},
                                  {Location::kCentre, 800.0},
                                  {Location::kRight, 1200.0},
                                  {Location::kLeftSurround, 600.0},
                                  {Location::kRightSurround, 1400.0},
                                  {Location::kVhl, 2000.0},
                                  {Location::kVhr, 2400.0},
                                  {Location::kLts, 2800.0},
                                  {Location::kRts, 3200.0},
                                  {Location::kLfe, 60.0}}});
    // Six new channels, one more than a single dependent can carry, so this is
    // the layout that needs two - and the one FFmpeg cannot read.
    cases.push_back({.name = "7.1.4",
                     .config = {.independent = bed(640), .dependents = {rear, top}},
                     .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 500.0, 1600.0,
                               400.0, 1800.0, 2000.0, 2400.0, 2800.0, 3200.0},
                     .speakers = {{Location::kLeft, 1000.0},
                                  {Location::kCentre, 800.0},
                                  {Location::kRight, 1200.0},
                                  {Location::kLeftSurround, 500.0},
                                  {Location::kRightSurround, 1600.0},
                                  {Location::kLrs, 400.0},
                                  {Location::kRrs, 1800.0},
                                  {Location::kVhl, 2000.0},
                                  {Location::kVhr, 2400.0},
                                  {Location::kLts, 2800.0},
                                  {Location::kRts, 3200.0},
                                  {Location::kLfe, 60.0}}});
    return cases;
}

struct RoundTrip {
    ac3::eac3::chanmap::Layout layout;
    std::vector<std::vector<float>> rendered;  // per rendered channel, full length
    std::vector<std::vector<float>> source;    // per CODED channel, full length
    int substreams = 0;
};

// Encode per-channel tones, then feed the elementary stream back through
// split_access_units and the decoder - so the framing is exercised too, not
// just the frames the encoder happened to hand over.
RoundTrip round_trip(const LayoutCase& layout, int frames) {
    ac3::eac3::AccessUnitEncoder encoder{layout.config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(layout.tones.size() == nchans);

    RoundTrip rt;
    rt.source.resize(nchans);
    std::vector<std::vector<float>> block(nchans,
                                          std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                block[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                    kAmplitude * std::sin(2.0 * std::numbers::pi * layout.tones[ch] *
                                          static_cast<double>(n0 + static_cast<std::uint64_t>(i)) /
                                          48000.0));
            }
            views[ch] = block[ch];
            rt.source[ch].insert(rt.source[ch].end(), block[ch].begin(), block[ch].end());
        }
        n0 += ac3::kSamplesPerFrame;
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == static_cast<std::size_t>(frames));

    ac3::Eac3Decoder decoder;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (rt.rendered.empty()) {
            rt.layout = decoded->layout;
            rt.substreams = decoded->substream_count;
            rt.rendered.resize(decoded->channels.size());
        }
        REQUIRE(decoded->channels.size() == rt.rendered.size());
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            rt.rendered[ch].insert(rt.rendered[ch].end(), decoded->channels[ch].begin(),
                                   decoded->channels[ch].end());
        }
    }
    return rt;
}

// Coarse spectral peak over the steady-state middle of the signal. The scan
// reaches 4 kHz because the ceiling channels carry tones the AC-3 tests never
// needed to see.
double dominant_freq_hz(const std::vector<float>& x) {
    double best_f = 0.0;
    double best_m = -1.0;
    const std::size_t n0 = 2048;
    const std::size_t len = std::min<std::size_t>(8192, x.size() - n0);
    for (double f = 50.0; f <= 4000.0; f += 10.0) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < len; ++i) {
            const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(i) / 48000.0;
            re += static_cast<double>(x[n0 + i]) * std::cos(phase);
            im += static_cast<double>(x[n0 + i]) * std::sin(phase);
        }
        const double mag = re * re + im * im;
        if (mag > best_m) {
            best_m = mag;
            best_f = f;
        }
    }
    return best_f;
}

// Direct sample SNR with the 256-sample encode+decode delay, skipping the
// warm-up frame at each end.
double snr_db(const std::vector<float>& input, const std::vector<float>& decoded) {
    constexpr std::size_t kDelay = 256;
    constexpr std::size_t kSkip = 1536;
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = kSkip; i + kSkip < input.size(); ++i) {
        const double x = static_cast<double>(input[i - kDelay]);
        const double d = static_cast<double>(decoded[i]) - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

// Overwrite `count` bits at `offset` and restore crc2, so a patched frame is
// still a legal syncframe and the decoder's own checks are what reject it.
void patch_bits(std::vector<std::byte>& frame, std::size_t offset, int count,
                std::uint32_t value) {
    for (int i = 0; i < count; ++i) {
        const std::size_t bit = offset + static_cast<std::size_t>(i);
        const auto mask = static_cast<std::uint8_t>(0x80u >> (bit & 7));
        const auto set = (value >> (count - 1 - i)) & 1u;
        auto& target = frame[bit >> 3];
        target = set != 0 ? (target | std::byte{mask})
                          : (target & static_cast<std::byte>(~mask));
    }
    const auto bytes = frame.size();
    const std::uint16_t crc2 = ac3::crc16(std::span<const std::byte>{frame}.subspan(2, bytes - 4));
    frame[bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
}

}  // namespace

TEST_CASE("every E-AC-3 layout renders each tone into its own speaker", "[eac3][decoder]") {
    for (const auto& layout : layout_cases()) {
        CAPTURE(layout.name);
        const auto rt = round_trip(layout, 4);
        REQUIRE(rt.rendered.size() == layout.speakers.size());
        REQUIRE(rt.layout.count == static_cast<int>(layout.speakers.size()));
        REQUIRE(rt.substreams == static_cast<int>(layout.config.dependents.size()) + 1);
        for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
            CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
            // The rendered order is Table E2.5's bit order, so a layout that
            // decodes the right audio into the wrong slots still fails here.
            CHECK(rt.layout[static_cast<int>(ch)] == layout.speakers[ch].location);
            CHECK(std::abs(dominant_freq_hz(rt.rendered[ch]) - layout.speakers[ch].tone_hz) <
                  10.0);
        }
    }
}

TEST_CASE("7.1.4 decodes to twelve channels with the ceiling quad in place",
          "[eac3][decoder]") {
    // The point of the exercise: two dependent substreams, six channels laid
    // over a 5.1 bed, and no external decoder that will read it.
    const auto cases = layout_cases();
    const auto& layout = cases.back();
    REQUIRE(layout.name == "7.1.4");
    const auto rt = round_trip(layout, 4);
    REQUIRE(rt.rendered.size() == 12);
    REQUIRE(rt.substreams == 3);

    // The four ceiling channels and the rear pair are the ones only the
    // dependents carry; the bed cannot fake them.
    for (const auto ceiling : {Location::kVhl, Location::kVhr, Location::kLts, Location::kRts,
                               Location::kLrs, Location::kRrs}) {
        const int slot = rt.layout.index_of(ceiling);
        CAPTURE(ac3::eac3::chanmap::name(ceiling), slot);
        REQUIRE(slot >= 0);
        double peak = 0.0;
        for (const auto sample : rt.rendered[static_cast<std::size_t>(slot)]) {
            peak = std::max(peak, std::abs(static_cast<double>(sample)));
        }
        CHECK(peak > 0.3);  // the tone is at 0.4, so this is not leakage
    }
}

TEST_CASE("a programme can carry LFE and LFE2 as two distinct channels", "[eac3][decoder]") {
    using ac3::Acmod;
    namespace cm = ac3::eac3::chanmap;
    // LFE2 needs a full-bandwidth companion in its own substream (acmod
    // always contributes at least one full-bandwidth channel - see
    // chanmap::allocate/acmod_for_chanmap); Vhc plays that role here. The
    // bed carries its own LFE via lfeon as always, so the programme ends up
    // with two independent LFE-type channels. 60 Hz and 150 Hz sit in
    // different LFE coefficient bins (kLfeEndmant caps the LFE channel at
    // seven bins of ~93.75 Hz each), so both survive its restricted
    // bandwidth and stay distinguishable from each other.
    const LayoutCase layout{
        .name = "5.1 + Vhc + LFE2",
        .config = {.independent = bed(640),
                   .dependents = {{.bitrate_kbps = 320,
                                   .acmod = Acmod::k1_0,
                                   .lfe = true,
                                   .chanmap = static_cast<std::uint16_t>(cm::kVhcBit | cm::kLfe2Bit)}}},
        .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 150.0},
        .speakers = {{Location::kLeft, 1000.0},
                     {Location::kCentre, 800.0},
                     {Location::kRight, 1200.0},
                     {Location::kLeftSurround, 600.0},
                     {Location::kRightSurround, 1400.0},
                     {Location::kVhc, 2000.0},
                     {Location::kLfe2, 150.0},
                     {Location::kLfe, 60.0}}};

    const auto rt = round_trip(layout, 4);
    REQUIRE(rt.rendered.size() == layout.speakers.size());
    REQUIRE(rt.layout.count == static_cast<int>(layout.speakers.size()));
    REQUIRE(rt.substreams == 2);
    for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
        CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
        // Rendered order is Table E2.5's bit order (LFE2 at bit 14, before
        // LFE at bit 15), so this also proves LFE2 is not silently aliased
        // onto the bed's own LFE slot.
        CHECK(rt.layout[static_cast<int>(ch)] == layout.speakers[ch].location);
        CHECK(std::abs(dominant_freq_hz(rt.rendered[ch]) - layout.speakers[ch].tone_hz) < 10.0);
    }
}

TEST_CASE("two dependents that claim the same location: the later one wins",
          "[eac3][decoder]") {
    using ac3::Acmod;
    namespace cm = ac3::eac3::chanmap;
    // Nothing stops two dependents from naming the same Table E2.5 location -
    // the per-substream chanmap check (E2.3.1.8) never looks at siblings, and
    // build_silent_access_unit accepts it outright (see the encoder-side test
    // in test_eac3.cpp). The decoder's own §E3.8.2 rule - "transmission order
    // is overwrite order" (eac3_decoder.cpp) - is what actually resolves the
    // conflict. This is that rule proven with real, distinguishable audio
    // rather than just read off the comment that documents it: if the later
    // dependent did NOT win, or if the two blended, the tone check below
    // would catch it.
    const LayoutCase layout{
        .name = "duplicate Vhc claim",
        .config = {.independent = bed(640),
                   .dependents = {{.bitrate_kbps = 128, .acmod = Acmod::k1_0, .chanmap = cm::kVhcBit},
                                  {.bitrate_kbps = 128, .acmod = Acmod::k1_0, .chanmap = cm::kVhcBit}}},
        .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 2500.0},
        .speakers = {{Location::kLeft, 1000.0},
                     {Location::kCentre, 800.0},
                     {Location::kRight, 1200.0},
                     {Location::kLeftSurround, 600.0},
                     {Location::kRightSurround, 1400.0},
                     {Location::kVhc, 2500.0},  // the SECOND dependent's tone, not the first's 2000
                     {Location::kLfe, 60.0}}};

    const auto rt = round_trip(layout, 4);
    REQUIRE(rt.rendered.size() == layout.speakers.size());
    REQUIRE(rt.layout.count == static_cast<int>(layout.speakers.size()));
    REQUIRE(rt.substreams == 3);
    for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
        CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
        CHECK(rt.layout[static_cast<int>(ch)] == layout.speakers[ch].location);
        CHECK(std::abs(dominant_freq_hz(rt.rendered[ch]) - layout.speakers[ch].tone_hz) < 10.0);
    }
}

TEST_CASE("two substreams claiming primary LFE: the later one wins, not both",
          "[eac3][decoder]") {
    using ac3::Acmod;
    namespace cm = ac3::eac3::chanmap;
    // The bed's own lfeon and a dependent's chanmap can each independently
    // claim bit 15 (primary LFE): nothing stops a dependent's chanmap from
    // relabelling its lfe-type coded slot as LFE instead of LFE2, the same
    // way the LFE/LFE2 test above relabels it AS LFE2. Two substreams both
    // claiming the format's one LFE-type-per-substream slot at the SAME
    // location is the sharpest version of the overwrite footgun: get it
    // wrong and a channel goes silent with no error anywhere to explain why.
    // It does not go silent here - the later substream's LFE content plays,
    // exactly as documented in eac3_decoder.cpp - but that is a claim only
    // real, distinguishable audio through the real decoder can prove.
    const LayoutCase layout{
        .name = "duplicate primary LFE claim",
        .config = {.independent = bed(640),
                   .dependents = {{.bitrate_kbps = 128,
                                   .acmod = Acmod::k1_0,
                                   .lfe = true,
                                   .chanmap = static_cast<std::uint16_t>(cm::kVhcBit | cm::kLfeBit)}}},
        .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 150.0},
        .speakers = {{Location::kLeft, 1000.0},
                     {Location::kCentre, 800.0},
                     {Location::kRight, 1200.0},
                     {Location::kLeftSurround, 600.0},
                     {Location::kRightSurround, 1400.0},
                     {Location::kVhc, 2000.0},
                     {Location::kLfe, 150.0}}};  // the DEPENDENT's LFE tone, not the bed's 60

    const auto rt = round_trip(layout, 4);
    REQUIRE(rt.rendered.size() == layout.speakers.size());
    REQUIRE(rt.layout.count == static_cast<int>(layout.speakers.size()));
    REQUIRE(rt.substreams == 2);
    for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
        CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
        CHECK(rt.layout[static_cast<int>(ch)] == layout.speakers[ch].location);
        CHECK(std::abs(dominant_freq_hz(rt.rendered[ch]) - layout.speakers[ch].tone_hz) < 10.0);
    }
}

TEST_CASE("E-AC-3 round trips are near-transparent in every channel", "[eac3][decoder]") {
    // Real audio from frame 1 onward is the only input that can detect a
    // frame-layout error: with silence every bap is zero, so a stray bit lands
    // in zero-filled aux data and the frame still "decodes". The bar is a
    // direct sample comparison, which includes the tone's own amplitude and
    // phase quantization - a stricter metric than a sine fit.
    for (const auto& layout : layout_cases()) {
        CAPTURE(layout.name);
        const auto rt = round_trip(layout, 5);
        for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
            // Compare against the coded channel that ends up in this speaker,
            // which for an overwritten location is the DEPENDENT's input.
            std::size_t source = 0;
            bool found = false;
            std::size_t taken = 0;
            const auto find_in = [&](const ac3::eac3::FrameConfig& sub) {
                const auto locations =
                    ac3::eac3::chanmap::expand(sub.chanmap ? *sub.chanmap
                                                          : ac3::eac3::chanmap::acmod_map(
                                                                sub.acmod, sub.lfe));
                for (int i = 0; i < locations.count; ++i) {
                    if (locations[i] == layout.speakers[ch].location) {
                        source = taken + static_cast<std::size_t>(i);
                        found = true;  // later substreams win, as §E3.8.2 says
                    }
                }
                taken += static_cast<std::size_t>(locations.count);
            };
            find_in(layout.config.independent);
            for (const auto& dep : layout.config.dependents) {
                find_in(dep);
            }
            REQUIRE(found);
            CAPTURE(ch, source, ac3::eac3::chanmap::name(layout.speakers[ch].location));
            // Six channels share the bed's 640 kbps at full bandwidth; the
            // worst measured channel sits around 38 dB on this metric.
            CHECK(snr_db(rt.source[source], rt.rendered[ch]) > 33.0);
        }
    }
}

TEST_CASE("bsid at bit 40 picks the framing", "[eac3][decoder]") {
    const auto unit = ac3::eac3::build_silent_access_unit(
        {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
         .dependents = {{.bitrate_kbps = 224,
                         .acmod = ac3::Acmod::k2_2,
                         .chanmap = ac3::eac3::chanmap::k71Rear}}});
    REQUIRE(unit.has_value());
    std::vector<std::byte> stream;
    for (int i = 0; i < 3; ++i) {
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    CHECK(*ac3::stream_bsid(stream) == ac3::eac3::kBsid);
    // E-AC-3 sizes come from frmsiz, not from Table 5.18: the old
    // frmsizecod > 37 test would have rejected this outright.
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    CHECK(frames->size() == 6);
    // Access units are delimited rather than framed - a new one starts
    // wherever an independent substream does.
    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    CHECK(units->size() == 3);
    for (const auto& one : *units) {
        CHECK(one.size() == unit->bytes.size());
    }
    // An AC-3 frame decoder must refuse a bsid-16 frame rather than
    // misinterpret its header.
    ac3::FrameDecoder ac3_decoder;
    CHECK(ac3_decoder.decode_frame(unit->substream(0)).error() ==
          ac3::DecodeError::kUnsupported);
}

TEST_CASE("the dependent substream's own fields survive the round trip",
          "[eac3][decoder]") {
    const auto unit = ac3::eac3::build_silent_access_unit(
        {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
         .dependents = {{.bitrate_kbps = 224,
                         .acmod = ac3::Acmod::k2_2,
                         .chanmap = ac3::eac3::chanmap::k71Rear},
                        {.bitrate_kbps = 224,
                         .acmod = ac3::Acmod::k2_2,
                         .chanmap = ac3::eac3::chanmap::kTopQuad}}});
    REQUIRE(unit.has_value());
    ac3::Eac3Decoder decoder;

    const auto lead = decoder.decode_substream(unit->substream(0));
    REQUIRE(lead.has_value());
    CHECK(lead->strmtyp == ac3::eac3::StreamType::kIndependent);
    CHECK(lead->substreamid == 0);
    CHECK(lead->acmod == ac3::Acmod::k3_2);
    CHECK(lead->lfe);
    CHECK(!lead->chanmap.has_value());
    CHECK(lead->channels.size() == 6);

    const auto first = decoder.decode_substream(unit->substream(1));
    REQUIRE(first.has_value());
    CHECK(first->strmtyp == ac3::eac3::StreamType::kDependent);
    // §E2.3.1.2: a dependent's id starts again at 0 in its own numbering space.
    CHECK(first->substreamid == 0);
    CHECK(first->chanmap == ac3::eac3::chanmap::k71Rear);
    CHECK(first->channels.size() == 4);
    // §E3.8.5: compre marks the LAST dependent of the program, so the first of
    // two must not carry it.
    CHECK(!first->last_dependent);

    const auto second = decoder.decode_substream(unit->substream(2));
    REQUIRE(second.has_value());
    CHECK(second->substreamid == 1);
    CHECK(second->chanmap == ac3::eac3::chanmap::kTopQuad);
    CHECK(second->last_dependent);
}

namespace {

// A stereo frame that is either a single quiet tone (tiny mantissa cost) or
// several full-scale tones spread across the spectrum (large mantissa
// cost), so that consecutive access units in ONE stream come out genuinely
// different sizes - the thing CBR never had to prove the decoder handles.
std::vector<std::vector<float>> busy_or_quiet_frame(bool busy, std::uint64_t start) {
    std::vector<std::vector<float>> pcm(
        2, std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    const double tones[4] = {310.0, 2200.0, 6800.0, 13500.0};
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            double value = 0.0;
            if (busy) {
                for (std::size_t t = 0; t < 4; ++t) {
                    const double gain = 0.2 / (1.0 + static_cast<double>((t + ch) % 3));
                    value += gain * std::sin(2.0 * std::numbers::pi * tones[t] * n / 48000.0);
                }
            } else {
                value = 0.05 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0);
            }
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(value);
        }
    }
    return pcm;
}

}  // namespace

TEST_CASE("VBR access units of differing size still decode correctly",
          "[eac3][decoder][vbr]") {
    ac3::eac3::AccessUnitEncoder encoder{
        {.independent = {.bitrate_kbps = 192, .vbr = ac3::eac3::VbrConfig{.quality = 0.3}}}};
    REQUIRE(encoder.channel_count() == 2);

    std::vector<std::byte> stream;
    std::vector<std::size_t> unit_bytes;
    std::vector<float> want_l;
    std::vector<float> want_r;
    std::uint64_t n = 0;
    const std::vector<bool> busy{true, false, true, false, true};
    for (const bool b : busy) {
        auto pcm = busy_or_quiet_frame(b, n);
        n += ac3::kSamplesPerFrame;
        want_l.insert(want_l.end(), pcm[0].begin(), pcm[0].end());
        want_r.insert(want_r.end(), pcm[1].begin(), pcm[1].end());
        std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        unit_bytes.push_back(unit->bytes.size());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    // The whole point: consecutive access units in the SAME stream have
    // DIFFERENT sizes. split_access_units and the decoder below must not
    // assume otherwise - unlike every other test in this file, which never
    // exercises that because CBR never produces it.
    CHECK(unit_bytes[1] != unit_bytes[0]);

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == busy.size());

    ac3::Eac3Decoder decoder;
    std::vector<float> rendered_l;
    std::vector<float> rendered_r;
    for (const auto& access_unit : *units) {
        const auto decoded = decoder.decode_access_unit(access_unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 2);
        rendered_l.insert(rendered_l.end(), decoded->channels[0].begin(),
                          decoded->channels[0].end());
        rendered_r.insert(rendered_r.end(), decoded->channels[1].begin(),
                          decoded->channels[1].end());
    }
    CHECK(snr_db(want_l, rendered_l) > 20.0);
    CHECK(snr_db(want_r, rendered_r) > 20.0);
}

TEST_CASE("E-AC-3 coupling round-trips are near-transparent", "[eac3][decoder][coupling]") {
    // Three shapes coupling decode has to get right: acmod 2/0 (chincpl is
    // NOT transmitted there - both channels couple by definition, and
    // phsflginu takes its place), a 3/2+LFE bed with every fbw channel
    // coupled and the LFE riding alongside uncoupled, and an explicit
    // cplbegf pin (rather than the encoder's auto choice) to exercise a
    // different §7.5.2 rematrix-band count and coupling geometry.
    using ac3::Acmod;
    auto cpl_stereo = ac3::eac3::FrameConfig{.bitrate_kbps = 192, .coupling = true};
    auto cpl_bed = bed(192);
    cpl_bed.coupling = true;
    auto cpl_bed_pinned = bed(192);
    cpl_bed_pinned.coupling = true;
    cpl_bed_pinned.cplbegf = 0;

    const std::vector<double> bed_tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const std::vector<Speaker> bed_speakers = {
        {Location::kLeft, 1000.0},         {Location::kCentre, 800.0},
        {Location::kRight, 1200.0},        {Location::kLeftSurround, 600.0},
        {Location::kRightSurround, 1400.0}, {Location::kLfe, 60.0}};

    const std::vector<LayoutCase> cases = {
        {.name = "stereo cpl (phsflginu path)",
         .config = {.independent = cpl_stereo},
         .tones = {1000.0, 1600.0},
         .speakers = {{Location::kLeft, 1000.0}, {Location::kRight, 1600.0}}},
        {.name = "5.1 cpl (auto cplbegf)",
         .config = {.independent = cpl_bed},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 cpl (cplbegf pinned to 0)",
         .config = {.independent = cpl_bed_pinned},
         .tones = bed_tones,
         .speakers = bed_speakers},
    };

    for (const auto& layout : cases) {
        CAPTURE(layout.name);
        const auto rt = round_trip(layout, 5);
        REQUIRE(rt.rendered.size() == layout.speakers.size());
        for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
            CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
            CHECK(snr_db(rt.source[ch], rt.rendered[ch]) > 20.0);
        }
    }
}

TEST_CASE("the E-AC-3 decoder rejects malformed coupling streams",
          "[eac3][decoder][coupling]") {
    // A 3/2+LFE bed with coupling on and nothing else (no dependents, drc,
    // mixing or spx) so the bit offsets below - counted straight off
    // eac3_frame.cpp's emit_frame, the ONE function silent and real frames
    // both go through (see its own comment) - land exactly where this one
    // says: bsi (54 bits) + audfrm (90 bits) + block 0's dithflag(5)/
    // dynrnge(1)/spxinu(1) prefix (7 bits) puts ecplinu at bit 151, followed
    // by chincpl[0..4] (5), cplbegf (4), cplendf (4), then cplbndstrce at
    // bit 165. A silent frame never turns cplinu on (build_silent_frame
    // says so explicitly), so this needs a real, encoded tone.
    ac3::eac3::AccessUnitEncoder encoder{
        {.independent = {.bitrate_kbps = 448,
                         .acmod = ac3::Acmod::k3_2,
                         .lfe = true,
                         .coupling = true}}};
    REQUIRE(encoder.channel_count() == 6);
    std::vector<std::vector<float>> pcm(
        6, std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    const double tones[6] = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                kAmplitude * std::sin(2.0 * std::numbers::pi * tones[ch] * i / 48000.0));
        }
    }
    std::vector<std::span<const float>> views{pcm[0], pcm[1], pcm[2], pcm[3], pcm[4], pcm[5]};
    const auto unit = encoder.encode_access_unit(views);
    REQUIRE(unit.has_value());
    const std::vector<std::byte> whole = unit->bytes;
    constexpr std::size_t kEcplinuBit = 151;
    constexpr std::size_t kCplbegfBit = kEcplinuBit + 1 + 5;  // past ecplinu, chincpl x5
    constexpr std::size_t kCplendfBit = kCplbegfBit + 4;
    constexpr std::size_t kCplbndstrceBit = kCplendfBit + 4;
    ac3::Eac3Decoder decoder;

    SECTION("ecplinu set: enhanced coupling is recognised and refused") {
        auto broken = whole;
        patch_bits(broken, kEcplinuBit, 1, 1);
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kUnsupported);
    }
    SECTION("cplbndstrce cleared: the Annex E default band table is refused, not guessed at") {
        auto broken = whole;
        patch_bits(broken, kCplbndstrceBit, 1, 0);
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kUnsupported);
    }
    SECTION("cplbegf past cplendf collapses the coupled region to nothing") {
        auto broken = whole;
        patch_bits(broken, kCplbegfBit, 4, 15);
        patch_bits(broken, kCplendfBit, 4, 0);
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kInvalidStream);
    }
}

TEST_CASE("the E-AC-3 decoder rejects malformed streams", "[eac3][decoder]") {
    const auto unit = ac3::eac3::build_silent_access_unit(
        {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
         .dependents = {{.bitrate_kbps = 224,
                         .acmod = ac3::Acmod::k2_2,
                         .chanmap = ac3::eac3::chanmap::k71Rear}}});
    REQUIRE(unit.has_value());
    const std::vector<std::byte> whole = unit->bytes;
    const auto lead_bytes = unit->substream_bytes[0];
    ac3::Eac3Decoder decoder;

    SECTION("bad sync word") {
        auto broken = whole;
        broken[0] = std::byte{0x0C};
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kBadSyncWord);
    }
    SECTION("flipped payload bit fails crc2") {
        auto broken = whole;
        broken[100] ^= std::byte{0x10};
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kBadCrc);
    }
    SECTION("truncated") {
        CHECK(decoder.decode_access_unit(std::span{whole}.first(whole.size() - 2)).error() ==
              ac3::DecodeError::kTruncated);
    }
    SECTION("a frmsiz too small to cover its own header") {
        // frmsiz is an arbitrary 11-bit word count with no table to sanity
        // -check it against, so a frame may declare a size shorter than the
        // header already read out of it. The spans split_frames hands back are
        // indexed by its callers, so this must not become a short span.
        auto broken = whole;
        broken[2] = static_cast<std::byte>(std::to_integer<std::uint8_t>(broken[2]) & 0xF8);
        broken[3] = std::byte{0x00};  // frmsiz 0: one word, two bytes
        CHECK(ac3::split_frames(broken).error() == ac3::DecodeError::kInvalidStream);
    }
    SECTION("a dependent substream with no parent") {
        std::vector<std::byte> orphan{whole.begin() + lead_bytes, whole.end()};
        CHECK(ac3::split_access_units(orphan).error() == ac3::DecodeError::kInvalidStream);
        CHECK(decoder.decode_access_unit(orphan).error() == ac3::DecodeError::kInvalidStream);
    }
    SECTION("a chanmap that does not account for the coded channels") {
        // §E2.3.1.8: the locations a chanmap names must equal the channels
        // acmod and lfeon code. Nothing about the frame's shape changes, so
        // this parses perfectly and would simply put audio in the wrong
        // speakers - the decoder has to catch it explicitly.
        std::vector<std::byte> dependent{whole.begin() + lead_bytes, whole.end()};
        // chanmap sits after sync(16) strmtyp(2) substreamid(3) frmsiz(11)
        // fscod(2) numblkscod(2) acmod(3) lfeon(1) bsid(5) dialnorm(5)
        // compre(1) compr(8) chanmape(1).
        constexpr std::size_t kChanmapBit = 16 + 2 + 3 + 11 + 2 + 2 + 3 + 1 + 5 + 5 + 1 + 8 + 1;
        patch_bits(dependent, kChanmapBit, 16, ac3::eac3::chanmap::kLrsRrsBit);  // 2, not 4
        CHECK(decoder.decode_substream(dependent).error() ==
              ac3::DecodeError::kInvalidStream);
    }
}
