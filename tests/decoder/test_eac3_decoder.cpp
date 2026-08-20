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

// The same layouts and tones the CLI emits (see eac3_layout in apps/cli).
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
                     .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0},
                     .speakers = {{Location::kLeft, 1000.0},
                                  {Location::kCentre, 800.0},
                                  {Location::kRight, 1200.0},
                                  {Location::kLeftSurround, 600.0},
                                  {Location::kRightSurround, 1400.0},
                                  {Location::kVhl, 2000.0},
                                  {Location::kVhr, 1300.0},
                                  {Location::kLfe, 60.0}}});
    cases.push_back({.name = "5.1.4",
                     .config = {.independent = bed(640), .dependents = {top}},
                     .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0,
                               1500.0, 1700.0},
                     .speakers = {{Location::kLeft, 1000.0},
                                  {Location::kCentre, 800.0},
                                  {Location::kRight, 1200.0},
                                  {Location::kLeftSurround, 600.0},
                                  {Location::kRightSurround, 1400.0},
                                  {Location::kVhl, 2000.0},
                                  {Location::kVhr, 1300.0},
                                  {Location::kLts, 1500.0},
                                  {Location::kRts, 1700.0},
                                  {Location::kLfe, 60.0}}});
    // Six new channels, one more than a single dependent can carry, so this is
    // the layout that needs two - and the one FFmpeg cannot read.
    //
    // The ceiling/side tones below (Vhl through Rts) are deliberately kept
    // under ~2 kHz: TransientDetector's 8 kHz high-pass has enough headroom
    // above that for a steady tone's post-filter peak to stay under its
    // silence gate, so a spec-faithful but frequency-sensitive basic-encoder
    // heuristic (§8.2.2's segment peak-ratio comparison, which a steady tone
    // can occasionally trip near its own analysis-window boundaries once the
    // filtered signal clears the gate) never legitimately switches a block
    // here and costs this near-transparency measurement a few dB for reasons
    // unrelated to what it is checking.
    cases.push_back({.name = "7.1.4",
                     .config = {.independent = bed(640), .dependents = {rear, top}},
                     .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 500.0, 1600.0,
                               400.0, 1800.0, 2000.0, 1300.0, 1500.0, 1700.0},
                     .speakers = {{Location::kLeft, 1000.0},
                                  {Location::kCentre, 800.0},
                                  {Location::kRight, 1200.0},
                                  {Location::kLeftSurround, 500.0},
                                  {Location::kRightSurround, 1600.0},
                                  {Location::kLrs, 400.0},
                                  {Location::kRrs, 1800.0},
                                  {Location::kVhl, 2000.0},
                                  {Location::kVhr, 1300.0},
                                  {Location::kLts, 1500.0},
                                  {Location::kRts, 1700.0},
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
        REQUIRE(decoded->has_value());
        if (rt.rendered.empty()) {
            rt.layout = (*decoded)->layout;
            rt.substreams = (*decoded)->substream_count;
            rt.rendered.resize((*decoded)->channels.size());
        }
        REQUIRE((*decoded)->channels.size() == rt.rendered.size());
        for (std::size_t ch = 0; ch < (*decoded)->channels.size(); ++ch) {
            rt.rendered[ch].insert(rt.rendered[ch].end(), (*decoded)->channels[ch].begin(),
                                   (*decoded)->channels[ch].end());
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
    // in encoder/test_eac3.cpp). The decoder's own §E3.8.2 rule - "transmission order
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

TEST_CASE("E-AC-3 dual mono codes two independent programmes, never one into the other",
         "[eac3][decoder][dual-mono]") {
    using ac3::Acmod;
    // Same shape as the AC-3 version of this test: Ch1 loud, Ch2 silent, so
    // any cross-talk - coupling switched on by mistake, a shared downmix
    // measurement, Ch1 and Ch2 swapped - shows up directly rather than
    // needing a correlation check to notice.
    // heavy2 is set explicitly alongside heavy - compre2 is Ch2's own flag,
    // not inherited from Ch1's, so leaving it unset here would (correctly)
    // silence compr2 and defeat the compr2.has_value() check below.
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 192,
                        .acmod = Acmod::kDualMono,
                        .dialnorm = 27,
                        .dialnorm2 = 18,
                        .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
                        .heavy = ac3::meta::HeavyConfig{},
                        .heavy2 = ac3::meta::HeavyConfig{}}};
    ac3::eac3::AccessUnitEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 2);

    std::vector<std::vector<float>> block(2, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(2);
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < 3; ++f) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const double t = static_cast<double>(n0 + static_cast<std::uint64_t>(i)) / 48000.0;
            block[0][static_cast<std::size_t>(i)] =
                static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 1200.0 * t));
            block[1][static_cast<std::size_t>(i)] = 0.0f;
        }
        views[0] = block[0];
        views[1] = block[1];
        n0 += ac3::kSamplesPerFrame;
        auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 3);  // no dependents, so one substream per frame

    ac3::Eac3Decoder substream_decoder;
    ac3::DecodedSubstream last_substream{};
    for (const auto& unit : *units) {
        const auto frames = ac3::split_frames(unit);
        REQUIRE(frames.has_value());
        REQUIRE(frames->size() == 1);
        const auto decoded = substream_decoder.decode_substream(frames->front());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        last_substream = **decoded;
    }
    CHECK(last_substream.acmod == Acmod::kDualMono);
    CHECK(last_substream.dialnorm == 27);
    REQUIRE(last_substream.dialnorm2.has_value());
    CHECK(*last_substream.dialnorm2 == 18);
    REQUIRE(last_substream.compr2.has_value());
    REQUIRE(last_substream.channels.size() == 2);

    double ch1_peak = 0.0;
    double ch2_peak = 0.0;
    for (const float s : last_substream.channels[0]) {
        ch1_peak = std::max(ch1_peak, std::abs(static_cast<double>(s)));
    }
    for (const float s : last_substream.channels[1]) {
        ch2_peak = std::max(ch2_peak, std::abs(static_cast<double>(s)));
    }
    CHECK(ch1_peak > 0.3);
    CHECK(ch2_peak < 0.02);

    // decode_access_unit must not invent a spatial layout for 1+1 - a fresh
    // decoder, so its overlap-add state cannot be confused with the loop
    // above's per-substream one.
    ac3::Eac3Decoder unit_decoder;
    const auto au = unit_decoder.decode_access_unit(units->back());
    REQUIRE(au.has_value());
    REQUIRE(au->has_value());
    CHECK((*au)->acmod == Acmod::kDualMono);
    CHECK((*au)->layout.count == 0);
    REQUIRE((*au)->channels.size() == 2);
}

TEST_CASE("E-AC-3 dual mono: Ch2's own heavy compression is not Ch1's, and is not assumed",
          "[eac3][decoder][dual-mono]") {
    using ac3::Acmod;
    // This decoder is default-constructed, so it never applies compr/compr2
    // to the reconstructed audio regardless of the words it reports (see
    // Eac3Decoder's DecoderConfig-driven gain; tests/meta/test_drc.cpp exercises
    // heavy_compression actually applying it, including the peak-level cross-
    // channel check the AC-3 sibling test does). This test instead stays at
    // the word level: it compares Ch2's OWN compr2 word across two encodes
    // that differ only in heavy2's ceiling - a direct within-one-encode
    // compr-vs-compr2 comparison couldn't distinguish "Ch2's controller uses
    // heavy2" from "the two channels' controllers just produce different
    // words," since Ch1 and Ch2 hear the same loud signal but start from
    // different heavy/heavy2 ceilings either way. Across two encodes, if
    // Ch2's controller were still built from `heavy` instead of `heavy2`,
    // both would produce the identical compr2 word regardless of what
    // heavy2 says.
    constexpr double kLooseCeiling = -1.0;
    constexpr double kTightCeiling = -6.0;

    auto encode_and_get_compr2 = [](double heavy2_ceiling) -> std::uint8_t {
        const ac3::eac3::AccessUnitConfig config{
            .independent = {.bitrate_kbps = 192,
                            .acmod = Acmod::kDualMono,
                            .dialnorm = 24,
                            .dialnorm2 = 24,
                            .heavy = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = kLooseCeiling},
                            .heavy2 = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = heavy2_ceiling}}};
        ac3::eac3::AccessUnitEncoder encoder{config};
        REQUIRE(encoder.channel_count() == 2);

        std::vector<float> loud(ac3::kSamplesPerFrame);
        std::uint64_t n0 = 0;
        std::vector<std::byte> stream;
        for (int f = 0; f < 4; ++f) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const double t = static_cast<double>(n0 + static_cast<std::uint64_t>(i)) / 48000.0;
                loud[static_cast<std::size_t>(i)] =
                    static_cast<float>(0.95 * std::sin(2.0 * std::numbers::pi * 1200.0 * t));
            }
            n0 += ac3::kSamplesPerFrame;
            const std::vector<std::span<const float>> views{loud, loud};
            auto unit = encoder.encode_access_unit(views);
            REQUIRE(unit.has_value());
            stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
        }

        const auto units = ac3::split_access_units(stream);
        REQUIRE(units.has_value());
        ac3::Eac3Decoder decoder;
        ac3::DecodedSubstream last_substream{};
        for (const auto& unit : *units) {
            const auto frames = ac3::split_frames(unit);
            REQUIRE(frames.has_value());
            const auto decoded = decoder.decode_substream(frames->front());
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            last_substream = **decoded;
        }
        REQUIRE(last_substream.compr2.has_value());
        return *last_substream.compr2;
    };

    const auto loose_word = encode_and_get_compr2(kLooseCeiling);
    const auto tight_word = encode_and_get_compr2(kTightCeiling);
    const double loose_db = 20.0 * std::log10(ac3::meta::compr_gain(loose_word));
    const double tight_db = 20.0 * std::log10(ac3::meta::compr_gain(tight_word));
    // The same loud signal, on the same programme 2 channel, needs
    // meaningfully more gain reduction under the tighter ceiling.
    CHECK(loose_db > tight_db + 3.0);

    // And the literal regression: heavy alone (no heavy2) must not carry
    // Ch1's compr as Ch2's compr2 too.
    const ac3::eac3::AccessUnitConfig heavy_only_config{
        .independent = {.bitrate_kbps = 192,
                        .acmod = Acmod::kDualMono,
                        .dialnorm = 24,
                        .dialnorm2 = 24,
                        .heavy = ac3::meta::HeavyConfig{.peak_ceiling_dbfs = kLooseCeiling}}};
    ac3::eac3::AccessUnitEncoder heavy_only_encoder{heavy_only_config};
    std::vector<float> loud(ac3::kSamplesPerFrame);
    std::uint64_t n0 = 0;
    std::vector<std::byte> heavy_only_stream;
    for (int f = 0; f < 4; ++f) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const double t = static_cast<double>(n0 + static_cast<std::uint64_t>(i)) / 48000.0;
            loud[static_cast<std::size_t>(i)] =
                static_cast<float>(0.95 * std::sin(2.0 * std::numbers::pi * 1200.0 * t));
        }
        n0 += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{loud, loud};
        auto unit = heavy_only_encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        heavy_only_stream.insert(heavy_only_stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    const auto heavy_only_units = ac3::split_access_units(heavy_only_stream);
    REQUIRE(heavy_only_units.has_value());
    ac3::Eac3Decoder heavy_only_decoder;
    for (const auto& unit : *heavy_only_units) {
        const auto frames = ac3::split_frames(unit);
        REQUIRE(frames.has_value());
        const auto decoded = heavy_only_decoder.decode_substream(frames->front());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        CHECK_FALSE((*decoded)->compr2.has_value());
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
    REQUIRE(lead->has_value());
    CHECK((*lead)->strmtyp == ac3::eac3::StreamType::kIndependent);
    CHECK((*lead)->substreamid == 0);
    CHECK((*lead)->acmod == ac3::Acmod::k3_2);
    CHECK((*lead)->lfe);
    CHECK(!(*lead)->chanmap.has_value());
    CHECK((*lead)->channels.size() == 6);

    const auto first = decoder.decode_substream(unit->substream(1));
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->strmtyp == ac3::eac3::StreamType::kDependent);
    // §E2.3.1.2: a dependent's id starts again at 0 in its own numbering space.
    CHECK((*first)->substreamid == 0);
    CHECK((*first)->chanmap == ac3::eac3::chanmap::k71Rear);
    CHECK((*first)->channels.size() == 4);
    // §E3.8.5: compre marks the LAST dependent of the program, so the first of
    // two must not carry it.
    CHECK(!(*first)->last_dependent);

    const auto second = decoder.decode_substream(unit->substream(2));
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK((*second)->substreamid == 1);
    CHECK((*second)->chanmap == ac3::eac3::chanmap::kTopQuad);
    CHECK((*second)->last_dependent);
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
        REQUIRE(decoded->has_value());
        REQUIRE((*decoded)->channels.size() == 2);
        rendered_l.insert(rendered_l.end(), (*decoded)->channels[0].begin(),
                          (*decoded)->channels[0].end());
        rendered_r.insert(rendered_r.end(), (*decoded)->channels[1].begin(),
                          (*decoded)->channels[1].end());
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

TEST_CASE("E-AC-3 enhanced coupling round-trips are near-transparent",
          "[eac3][decoder][coupling][enhanced_coupling]") {
    // Same shapes as standard coupling's own round-trip test above, with
    // .enhanced = true selecting §E3.5 instead of §7.4/§E3.3.
    using ac3::Acmod;
    auto cpl_stereo =
        ac3::eac3::FrameConfig{.bitrate_kbps = 192, .coupling = true, .enhanced = true};
    auto cpl_bed = bed(192);
    cpl_bed.coupling = true;
    cpl_bed.enhanced = true;
    auto cpl_spx_bed = bed(192);
    cpl_spx_bed.coupling = true;
    cpl_spx_bed.enhanced = true;
    cpl_spx_bed.spx = true;

    const std::vector<double> bed_tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const std::vector<Speaker> bed_speakers = {
        {Location::kLeft, 1000.0},         {Location::kCentre, 800.0},
        {Location::kRight, 1200.0},        {Location::kLeftSurround, 600.0},
        {Location::kRightSurround, 1400.0}, {Location::kLfe, 60.0}};

    const std::vector<LayoutCase> cases = {
        {.name = "stereo ecpl",
         .config = {.independent = cpl_stereo},
         .tones = {1000.0, 1600.0},
         .speakers = {{Location::kLeft, 1000.0}, {Location::kRight, 1600.0}}},
        {.name = "5.1 ecpl (auto ecplbegf)",
         .config = {.independent = cpl_bed},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 ecpl + spx together",
         .config = {.independent = cpl_spx_bed},
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

TEST_CASE("E-AC-3 enhanced coupling degrades gracefully when two channels share "
          "one narrow band",
          "[eac3][decoder][coupling][enhanced_coupling]") {
    // ecplbegf pinned to 0 starts the coupled region at bin 13 - enhanced
    // coupling's lowest possible sub-band, only 6 bins wide (§E3.5.2's
    // Table E3.7). With R's and Rs's tones (1200/1400 Hz, bins ~12.8/~14.9)
    // both landing inside that one band, this is an adversarial case for
    // ANY single-coordinate-per-band scheme, fit_ecpl_band included: two
    // genuinely different pure tones sharing 6 bins is more than one
    // (amplitude, angle, chaos) triple can represent exactly, whatever the
    // fit - chaos only adds statistical decorrelation, not a deterministic
    // reconstruction of two distinct per-bin phase patterns. That is a real
    // property of the coding tool itself, not a gap in this encoder's fit:
    // measured at ~6 dB after fit_ecpl_band landed, versus ~3 dB from the
    // amplitude-only fit it replaced - real, worthwhile improvement, just
    // not transparency this band width structurally cannot deliver. The
    // standard round-trip test above stays at the same 20 dB bar as
    // standard coupling for every case that does NOT force two channels
    // into one narrow band. This test exists to catch a REGRESSION (a
    // stream that stops decoding, or degrades below the real fit's own
    // floor), not to demand transparency the tool cannot deliver here.
    auto cpl_bed_pinned = bed(192);
    cpl_bed_pinned.coupling = true;
    cpl_bed_pinned.enhanced = true;
    cpl_bed_pinned.cplbegf = 0;
    const LayoutCase layout{
        .name = "5.1 ecpl (ecplbegf pinned to 0)",
        .config = {.independent = cpl_bed_pinned},
        .tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0},
        .speakers = {{Location::kLeft, 1000.0},
                     {Location::kCentre, 800.0},
                     {Location::kRight, 1200.0},
                     {Location::kLeftSurround, 600.0},
                     {Location::kRightSurround, 1400.0},
                     {Location::kLfe, 60.0}}};
    const auto rt = round_trip(layout, 5);
    REQUIRE(rt.rendered.size() == layout.speakers.size());
    for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
        CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
        // L, C, Ls and LFE's tones sit below bin 13 (uncoupled at this
        // pin), so those stay near-transparent; only R and Rs share the
        // narrow band described above.
        const bool shares_the_narrow_band = ch == 2 || ch == 4;
        CHECK(snr_db(rt.source[ch], rt.rendered[ch]) > (shares_the_narrow_band ? 5.0 : 20.0));
    }
}

TEST_CASE("E-AC-3 spectral extension round-trips are near-transparent",
          "[eac3][decoder][spx]") {
    // Four shapes spx decode has to get right: acmod 2/0 (chinspx IS
    // transmitted there, unlike coupling's phsflginu substitution), a
    // 3/2+LFE bed with every fbw channel extended, an explicit spxbegf pin
    // to exercise a different §7.5.2 rematrix-band count and copy/synthesis
    // geometry, and spx stacked with coupling together (the case that needs
    // cplendf derived from spxbegf rather than transmitted). spx_atten
    // defaults on in FrameConfig, so every case here also exercises the
    // seam notch for real, not just the copy/blend/scale path.
    using ac3::Acmod;
    auto spx_stereo = ac3::eac3::FrameConfig{.bitrate_kbps = 192, .spx = true};
    auto spx_bed = bed(192);
    spx_bed.spx = true;
    auto spx_bed_pinned = bed(192);
    spx_bed_pinned.spx = true;
    spx_bed_pinned.spxbegf = 0;
    auto cpl_spx_bed = bed(192);
    cpl_spx_bed.coupling = true;
    cpl_spx_bed.spx = true;

    const std::vector<double> bed_tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const std::vector<Speaker> bed_speakers = {
        {Location::kLeft, 1000.0},         {Location::kCentre, 800.0},
        {Location::kRight, 1200.0},        {Location::kLeftSurround, 600.0},
        {Location::kRightSurround, 1400.0}, {Location::kLfe, 60.0}};

    const std::vector<LayoutCase> cases = {
        {.name = "stereo spx",
         .config = {.independent = spx_stereo},
         .tones = {1000.0, 1600.0},
         .speakers = {{Location::kLeft, 1000.0}, {Location::kRight, 1600.0}}},
        {.name = "5.1 spx (auto spxbegf)",
         .config = {.independent = spx_bed},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 spx (spxbegf pinned to 0)",
         .config = {.independent = spx_bed_pinned},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 coupling + spx together",
         .config = {.independent = cpl_spx_bed},
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

TEST_CASE("E-AC-3 AHT round-trips are near-transparent", "[eac3][decoder][aht]") {
    // AHT is a real change of mantissa format (VQ below hebap 8, gain-
    // adaptive quantization at and above it) rather than a scale-and-copy or
    // a synthesized approximation, and it is fully specified - no unspecified
    // noise generator the way spx has - so it should decode far tighter than
    // spx's ~20 dB bar. Cases cover: auto gaqmod, each of the four gaqmod
    // pins (0 disables GAQ outright, testing pure VQ+plain-scalar; 1-3
    // exercise the three gain-word packings), and "all" (coupling + spx +
    // AHT stacked - the flagship combination this whole three-phase
    // initiative was aimed at, short only of a 7.1.4 layout here, which
    // round_trip's own real-decode-only helper already exercises for the
    // wider layouts elsewhere in this file).
    using ac3::Acmod;
    auto aht_bed = bed(192);
    aht_bed.aht = true;
    auto aht_bed_gaqmod0 = bed(192);
    aht_bed_gaqmod0.aht = true;
    aht_bed_gaqmod0.gaqmod = 0;
    auto aht_bed_gaqmod3 = bed(192);
    aht_bed_gaqmod3.aht = true;
    aht_bed_gaqmod3.gaqmod = 3;
    auto all_bed = bed(192);
    all_bed.coupling = true;
    all_bed.spx = true;
    all_bed.aht = true;

    const std::vector<double> bed_tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const std::vector<Speaker> bed_speakers = {
        {Location::kLeft, 1000.0},         {Location::kCentre, 800.0},
        {Location::kRight, 1200.0},        {Location::kLeftSurround, 600.0},
        {Location::kRightSurround, 1400.0}, {Location::kLfe, 60.0}};

    const std::vector<LayoutCase> cases = {
        {.name = "5.1 aht (auto gaqmod)",
         .config = {.independent = aht_bed},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 aht (gaqmod 0, GAQ off)",
         .config = {.independent = aht_bed_gaqmod0},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 aht (gaqmod 3)",
         .config = {.independent = aht_bed_gaqmod3},
         .tones = bed_tones,
         .speakers = bed_speakers},
        {.name = "5.1 all three tools stacked",
         .config = {.independent = all_bed},
         .tones = bed_tones,
         .speakers = bed_speakers},
    };

    for (const auto& layout : cases) {
        CAPTURE(layout.name);
        const auto rt = round_trip(layout, 5);
        REQUIRE(rt.rendered.size() == layout.speakers.size());
        for (std::size_t ch = 0; ch < layout.speakers.size(); ++ch) {
            CAPTURE(ch, ac3::eac3::chanmap::name(layout.speakers[ch].location));
            CHECK(snr_db(rt.source[ch], rt.rendered[ch]) > 30.0);
        }
    }
}

TEST_CASE("the E-AC-3 decoder rejects a truncated AHT stream", "[eac3][decoder][aht]") {
    // AHT's per-bin payload is variable-width (VQ index or six gain-
    // adaptively-quantized codewords, depending on that bin's hebap and
    // gain), unlike coupling/spx's fixed-field geometry - there is no fixed
    // bit offset to hand-patch a specific field at the way the coupling and
    // spx adversarial tests do. What every decode path shares, AHT's new one
    // included, is BitReader's own overflow guard: a stream cut short reads
    // past the end without reading out of bounds and is caught by the
    // existing overflowed() check once per block, so this proves that guard
    // still holds for the new per-bin AHT loop rather than re-deriving a
    // byte-for-byte bit layout to corrupt a single field within it.
    ac3::eac3::AccessUnitEncoder encoder{
        {.independent = {.bitrate_kbps = 448,
                         .acmod = ac3::Acmod::k3_2,
                         .lfe = true,
                         .aht = true}}};
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
    ac3::Eac3Decoder decoder;
    CHECK(decoder.decode_access_unit(std::span{unit->bytes}.first(unit->bytes.size() - 2))
              .error() == ac3::DecodeError::kTruncated);
}

TEST_CASE("the E-AC-3 decoder rejects malformed spectral extension streams",
          "[eac3][decoder][spx]") {
    // A 3/2+LFE bed with spx on and attenuation explicitly off (so the
    // frame-level chinspxatten/spxattencod block, which is variable-width
    // per channel, drops out and the bit offsets below stay fixed) and
    // nothing else, so the count is: bsi (54 bits) + audfrm (85 bits - 5
    // fewer than the coupling test's 90, since frmcplexpstr is only present
    // when some block actually couples, and none does here) + block 0's
    // dithflag(5)/dynrnge(1) prefix (6 bits) puts spxinu at bit 145,
    // followed by chinspx[0..4] (5), spxstrtf (2), spxbegf (3), spxendf (3).
    ac3::eac3::AccessUnitEncoder encoder{
        {.independent = {.bitrate_kbps = 448,
                         .acmod = ac3::Acmod::k3_2,
                         .lfe = true,
                         .spx = true,
                         .spx_atten = false}}};
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
    constexpr std::size_t kSpxinuBit = 145;
    constexpr std::size_t kSpxbegfBit = kSpxinuBit + 1 + 5 + 2;  // chinspx x5, spxstrtf
    constexpr std::size_t kSpxendfBit = kSpxbegfBit + 3;
    ac3::Eac3Decoder decoder;

    SECTION("spxbegf past spxendf collapses the extension region to nothing") {
        auto broken = whole;
        patch_bits(broken, kSpxbegfBit, 3, 7);  // begin_subbnd = 11
        patch_bits(broken, kSpxendfBit, 3, 0);  // end_subbnd = 5
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kInvalidStream);
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

    SECTION("ecplinu set on an otherwise-standard-coupling stream fails rather than misdecodes") {
        // The decoder now actually implements enhanced coupling (§E3.5)
        // rather than refusing it outright, so flipping this one bit no
        // longer hits a blanket "unsupported" check - it makes the decoder
        // read the rest of the block as enhanced coupling geometry and
        // coordinates, which this stream was never encoded as. That must
        // still fail somehow (most likely kInvalidStream from a bogus
        // sub-band range, though which exact check trips depends on the
        // specific garbage bits that follow) - what actually matters is
        // that a single corrupted bit here cannot make the rest of the
        // frame decode as something plausible. A genuine enhanced-coupling
        // round-trip - a real encoded stream, decoded successfully - is
        // covered once the encoder can produce one (see encoder/test_eac3.cpp).
        auto broken = whole;
        patch_bits(broken, kEcplinuBit, 1, 1);
        CHECK_FALSE(decoder.decode_access_unit(broken).has_value());
    }
    SECTION("cplbndstrce cleared without also removing the now-unread structure bits fails") {
        // The decoder applies Table E2.12 when cplbndstrce is 0 rather than
        // refusing it (see eac3_tools.hpp's kDefaultCplBandStructure), so
        // clearing just this one bit no longer hits a blanket "unsupported"
        // check. This encoder always transmits an explicit structure, so the
        // ncplsubnd - 1 bits it wrote are still sitting in the stream right
        // after cplbndstrce; the decoder now skips over them instead of
        // consuming them, which misaligns every field that follows. That
        // must still fail somehow - here it trips a downstream range check -
        // which is what this asserts. A genuine cplbndstrce == 0 stream (no
        // structure bits present at all) is covered by real interop
        // fixtures, not a hand-patched one from this project's own encoder.
        auto broken = whole;
        patch_bits(broken, kCplbndstrceBit, 1, 0);
        CHECK(decoder.decode_access_unit(broken).error() == ac3::DecodeError::kInvalidStream);
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

TEST_CASE("a real transient triggers block switching and decodes without pre-echo",
         "[eac3][decoder][block-switching]") {
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    ac3::Eac3Decoder decoder;
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());

    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    std::vector<std::span<const float>> silence_views(nchans, silence);
    // Two silent frames: the first primes history_, the second clears the
    // transient detector's own first-pass guard.
    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silence_views);
        REQUIRE(frame.has_value());
        REQUIRE(decoder.decode_substream(*frame).has_value());
    }

    constexpr int kOnset = 960;
    std::vector<float> transient(static_cast<std::size_t>(ac3::kSamplesPerFrame), 0.0f);
    for (int n = kOnset; n < ac3::kSamplesPerFrame; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    std::vector<std::span<const float>> transient_views(nchans, transient);
    const auto frame = encoder.encode_frame(transient_views);
    REQUIRE(frame.has_value());

    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());

    bool any_switched = false;
    for (const auto& channel : (*decoded)->blksw) {
        for (const bool sw : channel) {
            any_switched = any_switched || sw;
        }
    }
    CHECK(any_switched);

    for (std::size_t ch = 0; ch < nchans; ++ch) {
        double pre_energy = 0.0;
        for (int n = 0; n < kOnset - 256; ++n) {
            const double v =
                static_cast<double>((*decoded)->channels[ch][static_cast<std::size_t>(n)]);
            pre_energy += v * v;
        }
        CHECK(pre_energy < 1e-4);
    }
}

TEST_CASE("transient pre-noise processing holds a frame back then releases it corrected",
          "[eac3][decoder][transient_prenoise]") {
    // Same transient shape as the block-switching test above - this
    // encoder's own heuristic (see FrameConfig::transient_prenoise's doc
    // comment) signals a correction exactly where blksw also fires, so the
    // same onset exercises both.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .transient_prenoise = true}};
    ac3::Eac3Decoder decoder;
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());

    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    std::vector<std::span<const float>> silence_views(nchans, silence);
    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silence_views);
        REQUIRE(frame.has_value());
        const auto decoded = decoder.decode_substream(*frame);
        REQUIRE(decoded.has_value());
        // Silence never switches a block, so the tool never triggers here -
        // every one of these frames is ready immediately, same as a decoder
        // that never saw the tool at all.
        REQUIRE(decoded->has_value());
    }

    constexpr int kOnset = 960;
    std::vector<float> transient(static_cast<std::size_t>(ac3::kSamplesPerFrame), 0.0f);
    for (int n = kOnset; n < ac3::kSamplesPerFrame; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    std::vector<std::span<const float>> transient_views(nchans, transient);
    const auto transient_frame = encoder.encode_frame(transient_views);
    REQUIRE(transient_frame.has_value());

    // This frame turns transproce on, so decode_substream has nothing ready
    // to return yet - the correction it signals can reach back into the
    // PREVIOUS (already-decoded, silent) frame, which is only known once
    // this one has been parsed.
    const auto held = decoder.decode_substream(*transient_frame);
    REQUIRE(held.has_value());
    CHECK_FALSE(held->has_value());

    // One more frame (silent again) releases the transient frame, now with
    // its own correction applied.
    const auto after = encoder.encode_frame(silence_views);
    REQUIRE(after.has_value());
    const auto released = decoder.decode_substream(*after);
    REQUIRE(released.has_value());
    REQUIRE(released->has_value());

    for (std::size_t ch = 0; ch < nchans; ++ch) {
        double pre_energy = 0.0;
        for (int n = 0; n < kOnset - 256; ++n) {
            const double v =
                static_cast<double>((*released)->channels[ch][static_cast<std::size_t>(n)]);
            pre_energy += v * v;
        }
        CHECK(pre_energy < 1e-4);
    }

    // Once a substream identity has used the tool even once, it stays in
    // buffered mode for the rest of the stream (decode_substream's own doc
    // comment) - so the "after" frame decoded above is itself now the one
    // being held back, regardless of whether IT set transproce. flush() is
    // what a caller must call at end-of-stream to not silently lose it.
    auto remaining = decoder.flush();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining[0].channels.size() == nchans);
}

TEST_CASE("decode_access_unit queues a substream that keeps releasing while its "
          "sibling is held back, rather than overwriting the queued entry",
          "[eac3][decoder][transient_prenoise]") {
    // Only the independent substream ever sets transient_prenoise, so only IT
    // can start lagging by a frame. The dependent releases every call, same as
    // a stream that never used the tool at all - which is exactly the
    // asymmetry that a single-slot cache (rather than a per-identity queue)
    // would corrupt: the dependent's still-unconsumed release from the call
    // that triggered the hold-back must survive until the independent catches
    // up, not get replaced by the dependent's NEXT release.
    const ac3::eac3::AccessUnitConfig cfg{
        .independent = {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .transient_prenoise = true},
        .dependents = {{.bitrate_kbps = 96,
                        .acmod = ac3::Acmod::k2_0,
                        .chanmap = ac3::eac3::chanmap::k512Height}}};
    ac3::eac3::AccessUnitEncoder encoder{cfg};
    REQUIRE(encoder.channel_count() == 4);
    ac3::Eac3Decoder decoder;

    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    std::vector<std::span<const float>> silent_views(4, silence);

    // Silence never triggers the tool, so both substreams release every call
    // and the whole unit is ready immediately - same as a decoder that never
    // saw the tool at all.
    for (int f = 0; f < 2; ++f) {
        const auto unit = encoder.encode_access_unit(silent_views);
        REQUIRE(unit.has_value());
        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
    }

    constexpr int kOnset = 960;
    std::vector<float> transient(static_cast<std::size_t>(ac3::kSamplesPerFrame), 0.0f);
    for (int n = kOnset; n < ac3::kSamplesPerFrame; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    // The independent's channels carry the transient; the dependent's stay
    // silent and never set transproce themselves.
    std::vector<std::span<const float>> transient_views{transient, transient, silence, silence};
    const auto transient_unit = encoder.encode_access_unit(transient_views);
    REQUIRE(transient_unit.has_value());

    // The independent turns transproce on and holds back; the dependent
    // releases immediately as always. Because assembling the access unit
    // needs both, the whole call must come back empty - the dependent's
    // already-ready result gets queued, not discarded.
    const auto held = decoder.decode_access_unit(transient_unit->bytes);
    REQUIRE(held.has_value());
    CHECK_FALSE(held->has_value());

    const auto after_unit = encoder.encode_access_unit(silent_views);
    REQUIRE(after_unit.has_value());

    // This call's own frames both release immediately, but the correct
    // assembly pairs the independent's newly-released (transient-corrected)
    // result with the DEPENDENT'S RESULT FROM THE PREVIOUS CALL - not this
    // one. A single-slot cache would have overwritten that previous entry
    // with this call's dependent release instead of queuing it, silently
    // splicing two different time instants into one access unit.
    const auto released = decoder.decode_access_unit(after_unit->bytes);
    REQUIRE(released.has_value());
    REQUIRE(released->has_value());
    REQUIRE((*released)->channels.size() == 4);

    // Every channel is silent up to the transient, whether that is the
    // independent's corrected pre-echo region or the dependent's constant
    // silence - a wrong pairing (e.g. the "after" call's own silent frame
    // standing in for the independent) would pass this just as well, so it
    // is checked together with the energy assertion below rather than alone.
    double pre_energy = 0.0;
    double post_energy = 0.0;
    for (const auto& channel : (*released)->channels) {
        for (int n = 0; n < kOnset - 256; ++n) {
            const double v = static_cast<double>(channel[static_cast<std::size_t>(n)]);
            pre_energy += v * v;
        }
        for (int n = kOnset; n < ac3::kSamplesPerFrame; ++n) {
            const double v = static_cast<double>(channel[static_cast<std::size_t>(n)]);
            post_energy += v * v;
        }
    }
    CHECK(pre_energy < 1e-4);
    // Confirms this really is the transient access unit's data (paired
    // correctly) and not two silent frames misassembled together.
    CHECK(post_energy > 1.0);

    // Two things are still held at end-of-stream, and flush() must surface
    // both: decode_substream's OWN one-frame lag on the independent (the
    // "after" call's independent frame, buffered there and not yet released
    // to decode_access_unit at all - a 4th call would be needed for that),
    // and the dependent's matching "after" release, still queued in
    // decode_access_unit's assembly cache waiting for that independent frame
    // to catch up to it.
    auto remaining = decoder.flush();
    REQUIRE(remaining.size() == 2);
    CHECK(remaining[0].channels.size() == 2);
    CHECK(remaining[1].channels.size() == 2);
}

namespace {

// Real, two-tone audio at an arbitrary rate - separate from tone_frame()/
// round_trip() above, which bake 48 kHz into both synthesis and analysis and
// are shared by tests that have no reason to generalize them.
std::vector<std::vector<float>> fscod2_tone_pair(double rate_hz, std::uint64_t start) {
    constexpr double kFscod2Amplitude = 0.35;
    constexpr std::array<double, 2> kTones = {300.0, 700.0};  // well under any fscod2 Nyquist
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                kFscod2Amplitude * std::sin(2.0 * std::numbers::pi * kTones[ch] * n / rate_hz));
        }
    }
    return pcm;
}

double fscod2_dominant_freq_hz(const std::vector<float>& x, double rate_hz) {
    double best_f = 0.0;
    double best_m = -1.0;
    const std::size_t n0 = 512;
    const std::size_t len = std::min<std::size_t>(2048, x.size() - n0);
    for (double f = 50.0; f <= 1200.0; f += 5.0) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < len; ++i) {
            const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(i) / rate_hz;
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

}  // namespace

TEST_CASE("E-AC-3 encodes and decodes real audio at fscod2 half rates",
          "[eac3][decoder]") {
    struct Case {
        ac3::SampleRate rate;
        std::uint32_t hz;
    };
    const std::array<Case, 3> cases = {{{ac3::SampleRate::k24000, 24000},
                                        {ac3::SampleRate::k22050, 22050},
                                        {ac3::SampleRate::k16000, 16000}}};
    for (const auto& c : cases) {
        CAPTURE(c.hz);
        ac3::eac3::AccessUnitEncoder encoder{
            {.independent = {.sample_rate = c.rate, .bitrate_kbps = 192}}};
        ac3::Eac3Decoder decoder;
        std::vector<std::vector<float>> rendered(2);
        std::uint64_t n = 0;
        for (int f = 0; f < 4; ++f) {
            auto pcm = fscod2_tone_pair(static_cast<double>(c.hz), n);
            n += ac3::kSamplesPerFrame;
            const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
            const auto unit = encoder.encode_access_unit(views);
            REQUIRE(unit.has_value());
            // encode_access_unit's own bytes are already exactly one access
            // unit - no need to concatenate and re-split, unlike round_trip()
            // above, which is proving something else (that a muxed multi-unit
            // stream scans correctly).
            const auto decoded = decoder.decode_access_unit(unit->bytes);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            CHECK((*decoded)->sample_rate == c.rate);
            REQUIRE((*decoded)->channels.size() == 2);
            for (std::size_t ch = 0; ch < 2; ++ch) {
                rendered[ch].insert(rendered[ch].end(), (*decoded)->channels[ch].begin(),
                                    (*decoded)->channels[ch].end());
            }
        }
        // Real audio, not silence: §7.2.2.1.1's all-zero allocation would
        // decode perfectly whether or not fscod2's bit-allocation family
        // mapping (fscod_family()) were wired up correctly at all.
        CHECK(std::abs(fscod2_dominant_freq_hz(rendered[0], c.hz) - 300.0) < 15.0);
        CHECK(std::abs(fscod2_dominant_freq_hz(rendered[1], c.hz) - 700.0) < 15.0);
    }
}

TEST_CASE("dithflag=1 substitutes dither at zero-bap bins instead of silence (E-AC-3)",
          "[eac3][decoder][dither]") {
    // Same reasoning as decoder.cpp's own version of this test: with every
    // SNR offset at zero, §7.2.2.1.1's bit allocation is fully zero-bap for
    // genuinely silent input - every bin in the frame is bap == 0, which is
    // exactly what §7.3.4/Annex E's dithflag[ch] governs.
    //
    // This project's encoder always turns dithflage on (kDithflage in
    // eac3_frame.cpp) but writes every dithflag[ch] bit as 0 (dither off),
    // so the frame is patched by hand to flip block 0's dithflag[0] and
    // crc2 is restored - the only CRC E-AC-3 has (no crc1, unlike AC-3).
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};  // default acmod k2_0, no LFE
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(2, silence);
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());

    // Bit offset of block 0's dithflag[0], cross-checked against "E-AC-3
    // coupling places its fields where Annex E puts them" above: that test
    // reaches dithflag at bit 108 for the same minimal stereo shape PLUS
    // coupling's extra 5-bit frmcplexpstr code (present because cpl_active
    // is true there) - a config with no coupling never sends that code and
    // lands 5 bits earlier, at bit 103.
    constexpr std::size_t kDithflagBit0 = 103;

    {
        ac3::Eac3Decoder decoder;
        const auto decoded = decoder.decode_substream(*frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        for (const float v : (*decoded)->channels[0]) {
            CHECK(v == 0.0f);
        }
        for (const float v : (*decoded)->channels[1]) {
            CHECK(v == 0.0f);
        }
    }

    auto patched = *frame;
    patch_bits(patched, kDithflagBit0, 1, 1);  // dithflag[0] = 1

    // Determinism: two independent decoders on the same patched frame
    // produce bit-identical PCM.
    ac3::Eac3Decoder decoder_a;
    const auto decoded_a = decoder_a.decode_substream(patched);
    REQUIRE(decoded_a.has_value());
    REQUIRE(decoded_a->has_value());
    ac3::Eac3Decoder decoder_b;
    const auto decoded_b = decoder_b.decode_substream(patched);
    REQUIRE(decoded_b.has_value());
    REQUIRE(decoded_b->has_value());
    CHECK((*decoded_a)->channels[0] == (*decoded_b)->channels[0]);

    double ch0_energy = 0.0;
    for (const float v : (*decoded_a)->channels[0]) {
        ch0_energy += static_cast<double>(v) * static_cast<double>(v);
    }
    CHECK(ch0_energy > 0.0);
    for (const float v : (*decoded_a)->channels[1]) {
        CHECK(v == 0.0f);  // channel 1's dithflag untouched, stays literal zero
    }
}

TEST_CASE("dithflag=1 on a coupled E-AC-3 channel dithers independently of its sibling",
          "[eac3][decoder][dither][coupling]") {
    // Same point as decoder.cpp's coupled dither test: §7.3.4 requires
    // dither to be drawn independently PER RECEIVING CHANNEL after
    // decoupling, not once for the shared coupling-domain bin and then
    // scaled by each channel's coordinate. Real (not silent) per-channel
    // tones are needed here, deliberately, unlike the plain dither test
    // above: a silent channel's own coupling coordinate is legitimately
    // zero (it contributes nothing to the shared sum), which would zero out
    // dither same as anything else and prove nothing about §7.3.4 - the
    // interaction under test only shows up once the coordinate that
    // multiplies the dithered sample is itself nonzero.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 96, .coupling = true, .cplbegf = 4}};
    std::vector<float> tone0(static_cast<std::size_t>(ac3::kSamplesPerFrame));
    std::vector<float> tone1(static_cast<std::size_t>(ac3::kSamplesPerFrame));
    for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
        tone0[static_cast<std::size_t>(i)] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 5000.0 * i / 48000.0));
        tone1[static_cast<std::size_t>(i)] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 7000.0 * i / 48000.0));
    }
    const std::vector<std::span<const float>> views{tone0, tone1};
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());

    // Bit offset validated by "E-AC-3 coupling places its fields where
    // Annex E puts them" above, for this exact config (content-independent:
    // it only depends on which optional fields are structurally present).
    constexpr std::size_t kDithflagBit0 = 108;

    ac3::Eac3Decoder baseline_decoder;
    const auto baseline = baseline_decoder.decode_substream(*frame);
    REQUIRE(baseline.has_value());
    REQUIRE(baseline->has_value());

    auto patched = *frame;
    patch_bits(patched, kDithflagBit0, 2, 0b11);  // dithflag[0] = dithflag[1] = 1

    ac3::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(patched);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());

    // Turning dither on changes SOMETHING relative to the dithflag == 0
    // baseline (proving the substitution actually reached the output
    // through nonzero coupling coordinates), and channel 0 and channel 1
    // do not end up with an identical delta (proving each channel's dither
    // is its own independent draw, not one shared sample scaled by two
    // different coordinates).
    const auto& base0 = (*baseline)->channels[0];
    const auto& base1 = (*baseline)->channels[1];
    const auto& ch0 = (*decoded)->channels[0];
    const auto& ch1 = (*decoded)->channels[1];
    REQUIRE(ch0.size() == base0.size());
    REQUIRE(ch1.size() == base1.size());
    bool ch0_changed = false;
    bool ch1_changed = false;
    bool deltas_differ = false;
    for (std::size_t i = 0; i < ch0.size(); ++i) {
        const double d0 = static_cast<double>(ch0[i]) - static_cast<double>(base0[i]);
        const double d1 = static_cast<double>(ch1[i]) - static_cast<double>(base1[i]);
        if (std::abs(d0) > 1e-9) ch0_changed = true;
        if (std::abs(d1) > 1e-9) ch1_changed = true;
        if (std::abs(d0 - d1) > 1e-9) deltas_differ = true;
    }
    CHECK(ch0_changed);
    CHECK(ch1_changed);
    CHECK(deltas_differ);
}
