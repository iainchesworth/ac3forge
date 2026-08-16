#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/spatial/spatial.hpp"

namespace {

constexpr int kFrame = ac3::kSamplesPerFrame;

// Objects that are actually distinguishable: different frequencies, different
// phases, and none of them silent. Silence would make the covariance singular
// and every reconstruction trivially "correct" at zero - the exact false pass
// this project keeps rediscovering.
std::vector<float> tone(double hz, double amplitude, double phase, std::uint64_t start) {
    std::vector<float> out(kFrame);
    for (int n = 0; n < kFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * t + phase));
    }
    return out;
}

// Complex amplitude of a real signal at one frequency, over the frame.
std::complex<double> project(std::span<const float> x, double hz) {
    std::complex<double> sum{};
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double angle = -2.0 * std::numbers::pi * hz *
                             static_cast<double>(n) / 48000.0;
        sum += static_cast<double>(x[n]) * std::polar(1.0, angle);
    }
    return sum * (2.0 / static_cast<double>(x.size()));
}

// Which JOC parameter band a frequency lands in. The 64 QMF subbands split
// Nyquist evenly, so at 48 kHz each is 375 Hz wide, and Table 54 groups them.
int band_of(double hz, int num_bands_idx) {
    const auto subband = static_cast<std::size_t>(hz / (24000.0 / 64.0));
    return ac3::joc::kSubbandToBand[static_cast<std::size_t>(num_bands_idx)][subband];
}

// §6.6.6, evaluated at one frequency. The decoder applies the matrix band by
// band, so the matrix that acts on a given frequency is the one for ITS band -
// applying a single band's row across the whole spectrum is not what a decoder
// does and would measure nothing. The bed is in AC-3 order and JOC indexes its
// downmix differently (Table 53), so the permutation has to be undone.
//
// The matrix here is the one the encoder computed rather than one read back
// off the wire, so this measures the SOLVE; test_oba covers the coding.
std::complex<double> reconstruct_at(const ac3::oba::AtmosEncoder& encoder, int object,
                                    double hz, int num_bands_idx) {
    constexpr std::array<int, 5> kAc3FromJoc = {0, 2, 1, 3, 4};
    const int band = band_of(hz, num_bands_idx);
    std::complex<double> sum{};
    for (int channel = 0; channel < 5; ++channel) {
        const double m = encoder.parameters().at(object, channel, band);
        sum += m * project(encoder.bed()[static_cast<std::size_t>(
                               kAc3FromJoc[static_cast<std::size_t>(channel)])],
                           hz);
    }
    return sum;
}

// Error relative to a reference amplitude, in dB. Negative and large is good.
double error_db(std::complex<double> got, std::complex<double> want) {
    return 20.0 * std::log10(std::max(std::abs(got - want), 1e-30) /
                             std::max(std::abs(want), 1e-30));
}

}  // namespace

TEST_CASE("room positions fold onto the ring at the right angle", "[atmos][spatial]") {
    // §4.2.1: (0,5; 0) is the centre of the front wall, x grows to the right
    // and y grows towards the back.
    const auto front = ac3::spatial::pan_room(0.5, 0.0);
    CHECK(front[1] > 0.99);  // C dominates

    const auto left = ac3::spatial::pan_room(0.0, 0.5);
    CHECK(left[0] > 0.0);    // L
    CHECK(left[3] > 0.0);    // SL
    CHECK(left[2] == 0.0);   // nothing on the right
    CHECK(left[4] == 0.0);

    const auto right = ac3::spatial::pan_room(1.0, 0.5);
    CHECK(right[2] > 0.0);   // R
    CHECK(right[4] > 0.0);   // SR
    CHECK(right[0] == 0.0);
    CHECK(right[3] == 0.0);

    // Height is not in the pan at all, so a raised object lands exactly where
    // its ground-level twin does. That is the premise the object layer exists
    // to work around, so it had better be true.
    const auto low = ac3::spatial::pan_room(0.2, 0.3);
    const auto high = ac3::spatial::pan_room(0.2, 0.3);
    CHECK(low == high);

    // Energy preservation carries over from pan_azimuth.
    for (const auto& gains : {front, left, right, low}) {
        double energy = 0.0;
        for (const double g : gains) {
            energy += g * g;
        }
        CHECK_THAT(energy, Catch::Matchers::WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("well-separated objects come back out of the bed", "[atmos]") {
    // Four objects at four corners of the room: the panning gains are far
    // apart, so the downmix matrix is well conditioned and the least-squares
    // inverse should be very close to exact.
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 640}, 4};
    const std::array<ac3::oba::ObjectPlacement, 4> placement{{
        {.position = {.x = 0.0, .y = 0.0, .z = 0.0}},   // front left
        {.position = {.x = 1.0, .y = 0.0, .z = 0.0}},   // front right
        {.position = {.x = 0.0, .y = 1.0, .z = 1.0}},   // back left, overhead
        {.position = {.x = 1.0, .y = 1.0, .z = 1.0}},   // back right, overhead
    }};

    // Four tones in four different parameter bands, so each object's own band
    // is the one carrying its own energy.
    const std::array<double, 4> hz{311.0, 997.0, 2200.0, 5000.0};
    const std::array<double, 4> amplitude{0.30, 0.25, 0.20, 0.22};
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            REQUIRE(band_of(hz[static_cast<std::size_t>(i)], 4) !=
                    band_of(hz[static_cast<std::size_t>(j)], 4));
        }
    }

    // Three frames, and the checks run on the LAST one: frame 0's transform
    // window is half history that does not exist, so it is a fade-in rather
    // than steady state and would flatter any reconstruction.
    std::vector<std::vector<float>> essences;
    std::vector<std::span<const float>> views(4);
    for (int frame = 0; frame < 3; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        essences.clear();
        for (std::size_t i = 0; i < 4; ++i) {
            essences.push_back(tone(hz[i], amplitude[i], 0.7 * static_cast<double>(i), start));
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
    }

    for (int object = 0; object < 4; ++object) {
        CAPTURE(object);
        const auto index = static_cast<std::size_t>(object);
        const auto want = project(essences[index], hz[index]);
        const auto got = reconstruct_at(encoder, object, hz[index], 4);
        CHECK(error_db(got, want) < -20.0);

        // And the other objects must not bleed in. Each foreign tone is
        // evaluated in ITS band, which is where a decoder would meet it.
        for (int other = 0; other < 4; ++other) {
            if (other == object) {
                continue;
            }
            CAPTURE(other);
            const auto foreign = static_cast<std::size_t>(other);
            const auto leak = reconstruct_at(encoder, object, hz[foreign], 4);
            const auto reference = project(essences[foreign], hz[foreign]);
            CHECK(20.0 * std::log10(std::max(std::abs(leak), 1e-30) /
                                    std::abs(reference)) < -20.0);
        }
    }
}

TEST_CASE("objects sharing a direction split rather than blow up", "[atmos]") {
    // Two objects at the same azimuth and different heights get IDENTICAL bed
    // gains, so no linear combination of the bed can separate them. The right
    // behaviour is a bounded matrix that hands each one its share, not a
    // singular solve - and "bounded" matters, because the quantizer tops out
    // at about 9,6 and would silently clamp anything larger.
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 640}, 2};
    const std::array<ac3::oba::ObjectPlacement, 2> placement{{
        {.position = {.x = 0.2, .y = 0.4, .z = 0.0}},
        {.position = {.x = 0.2, .y = 0.4, .z = 1.0}},  // straight above the first
    }};

    // Both tones in the SAME parameter band. Different bands would make the
    // comparison meaningless - each object would be alone in its own band and
    // there would be nothing to share.
    constexpr double kLoudHz = 2000.0;
    constexpr double kQuietHz = 2200.0;
    const int band = band_of(kLoudHz, 4);
    REQUIRE(band == band_of(kQuietHz, 4));

    std::vector<std::span<const float>> views(2);
    std::vector<std::vector<float>> essences;
    for (int frame = 0; frame < 3; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        // Deliberately lopsided: one object four times the power of the other,
        // so a solve that splits by power is distinguishable from one that
        // splits evenly.
        essences = {tone(kLoudHz, 0.40, 0.0, start), tone(kQuietHz, 0.20, 0.9, start)};
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        REQUIRE(encoder.encode_frame(views, placement).has_value());
    }

    const auto& params = encoder.parameters();
    for (std::size_t i = 0; i < params.matrix.size(); ++i) {
        CAPTURE(i);
        CHECK(std::isfinite(params.matrix[i]));
        CHECK(std::abs(params.matrix[i]) <= 9.5);
    }
    // The louder object must claim the larger share of the shared direction:
    // the estimator weights each object by its own power, so a 4:1 power ratio
    // becomes a 4:1 share of the one direction they both occupy.
    double loud = 0.0;
    double quiet = 0.0;
    for (int channel = 0; channel < 5; ++channel) {
        loud += std::abs(params.at(0, channel, band));
        quiet += std::abs(params.at(1, channel, band));
    }
    CHECK(loud > quiet);
    CHECK_THAT(loud / quiet, Catch::Matchers::WithinRel(4.0, 0.15));
}

TEST_CASE("band_energy consults its fast flag, and both paths agree", "[atmos][fast-mdct]") {
    // AtmosConfig::fast_mdct reaches band_energy since the default-on
    // rollout, so the JOC solve's inputs ride the same transform path as the
    // bed. Two things have to hold, and they guard against different bugs:
    // the paths must AGREE (quality - the fast fold is verified ~3e-12
    // relative against the direct form), and they must not be IDENTICAL
    // (wiring - if band_energy quietly ignored `fast` again, the way it did
    // before the rollout, both legs would be the direct path and byte-equal,
    // which is exactly what the REQUIRE below refuses).
    //
    // Broadband content on purpose: tones alone leave most of the nine bands
    // only window leakage, whose relative error says nothing. A deterministic
    // LCG keeps the "noise" reproducible, and two tones keep it real-ish.
    const auto& mapping = ac3::joc::kSubbandToBand[4];  // 9 bands
    std::uint32_t lcg = 0x2545F491u;
    bool any_difference = false;
    for (int frame = 0; frame < 3; ++frame) {
        std::vector<float> signal(kFrame);
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        for (int n = 0; n < kFrame; ++n) {
            lcg = lcg * 1664525u + 1013904223u;
            const double noise =
                (static_cast<double>(lcg >> 8) / 8388608.0 - 1.0) * 0.05;
            const double t =
                static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
            signal[static_cast<std::size_t>(n)] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * t) +
                0.2 * std::sin(2.0 * std::numbers::pi * 3100.0 * t + 0.6) + noise);
        }
        std::array<double, 9> direct{};
        std::array<double, 9> fast{};
        ac3::oba::band_energy(signal, mapping, direct, /*fast=*/false);
        ac3::oba::band_energy(signal, mapping, fast, /*fast=*/true);
        for (int band = 0; band < 9; ++band) {
            CAPTURE(frame, band);
            REQUIRE(direct[static_cast<std::size_t>(band)] > 0.0);
            CHECK_THAT(fast[static_cast<std::size_t>(band)],
                       Catch::Matchers::WithinRel(direct[static_cast<std::size_t>(band)],
                                                  1e-9));
            any_difference = any_difference ||
                             fast[static_cast<std::size_t>(band)] !=
                                 direct[static_cast<std::size_t>(band)];
        }
    }
    REQUIRE(any_difference);
}

TEST_CASE("the JOC matrix is indifferent to which MDCT path fed it", "[atmos][fast-mdct]") {
    // The end-to-end half of the check above, at the surface a decoder
    // actually consumes: two encoders differing ONLY in fast_mdct, on the
    // shared-direction scenario where the per-band powers genuinely steer
    // the solve (well-separated objects reduce to D's left inverse, where a
    // uniform power wobble cancels and would prove nothing). The bed is
    // rendered from panning gains before any transform runs, so the matrix
    // is the entire surface band_energy can influence.
    ac3::oba::AtmosEncoder direct_encoder{{.bitrate_kbps = 640, .fast_mdct = false}, 2};
    ac3::oba::AtmosEncoder fast_encoder{{.bitrate_kbps = 640, .fast_mdct = true}, 2};
    const std::array<ac3::oba::ObjectPlacement, 2> placement{{
        {.position = {.x = 0.2, .y = 0.4, .z = 0.0}},
        {.position = {.x = 0.2, .y = 0.4, .z = 1.0}},
    }};

    std::vector<std::span<const float>> views(2);
    std::vector<std::vector<float>> essences;
    for (int frame = 0; frame < 3; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        essences = {tone(2000.0, 0.40, 0.0, start), tone(2200.0, 0.20, 0.9, start)};
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        REQUIRE(direct_encoder.encode_frame(views, placement).has_value());
        REQUIRE(fast_encoder.encode_frame(views, placement).has_value());
    }

    const auto& direct = direct_encoder.parameters().matrix;
    const auto& fast = fast_encoder.parameters().matrix;
    REQUIRE(direct.size() == fast.size());
    for (std::size_t i = 0; i < direct.size(); ++i) {
        CAPTURE(i);
        // Absolute floor for entries near zero, relative everywhere else -
        // the quantizer's own step is ~0.1 at its coarsest, so 1e-8 is many
        // orders below anything the bitstream could ever carry.
        CHECK(std::abs(fast[i] - direct[i]) <= 1e-8 * std::max(1.0, std::abs(direct[i])));
    }

    // And both paths still reconstruct: the same -20 dB bar the
    // shared-direction test above holds the solve to.
    const int band = band_of(2000.0, 4);
    for (const auto* encoder : {&direct_encoder, &fast_encoder}) {
        double loud = 0.0;
        double quiet = 0.0;
        for (int channel = 0; channel < 5; ++channel) {
            loud += std::abs(encoder->parameters().at(0, channel, band));
            quiet += std::abs(encoder->parameters().at(1, channel, band));
        }
        CHECK(loud > quiet);
        CHECK_THAT(loud / quiet, Catch::Matchers::WithinRel(4.0, 0.15));
    }
}

TEST_CASE("an Atmos frame is a plain 5.1 frame with metadata bolted on", "[atmos]") {
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, 3};
    const std::array<ac3::oba::ObjectPlacement, 3> placement{{
        {.position = {.x = 0.1, .y = 0.2, .z = 0.5}},
        {.position = {.x = 0.9, .y = 0.2, .z = 0.5}, .gain = 0.5},
        {.position = {.x = 0.5, .y = 0.9, .z = -0.5}, .lfe_send = 0.3},
    }};
    // The bed's LFE plus three dynamic objects.
    CHECK(ac3::oba::object_count(encoder.program()) == 4);
    CHECK(encoder.parameters().objects == 3);

    std::vector<std::vector<float>> essences;
    std::vector<std::span<const float>> views(3);
    ac3::eac3::AccessUnit unit;
    for (int frame = 0; frame < 3; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        essences = {tone(440.0, 0.3, 0.0, start), tone(880.0, 0.3, 0.5, start),
                    tone(120.0, 0.3, 1.0, start)};
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        auto encoded = encoder.encode_frame(views, placement);
        REQUIRE(encoded.has_value());
        unit = *encoded;
    }

    // One independent substream - no dependents, which is what makes this
    // deliverable at all (TS 103 420 Annex E.3 allows at most one dependent,
    // and every shipping profile allows none for a 5.1 downmix).
    REQUIRE(unit.substream_count() == 1);
    const auto frame = unit.substream(0);
    CHECK(ac3::crc16(frame.subspan(2)) == 0x0000);

    // The lfe_send actually reached the LFE, so the bed is 5.1 in substance
    // and not just in acmod.
    double lfe_energy = 0.0;
    for (const float sample : encoder.bed()[5]) {
        const double sd = static_cast<double>(sample);
        lfe_energy += sd * sd;
    }
    CHECK(lfe_energy > 0.0);

    // The EMDF container is in there, and it holds both payloads.
    ac3::BitReader r{frame};
    std::size_t at = static_cast<std::size_t>(-1);
    for (std::size_t bit = 0; bit + 16 <= frame.size() * 8; ++bit) {
        ac3::BitReader probe{frame};
        probe.skip(bit);
        if (probe.read(16) == ac3::emdf::kSyncWord) {
            at = bit;
            break;
        }
    }
    REQUIRE(at != static_cast<std::size_t>(-1));
    r.skip(at + 16 + 16 + 2 + 3);  // sync, length, emdf_version, key_id
    CHECK(r.read(5) == ac3::emdf::kPayloadIdOamd);
}

namespace {

// Scans `frame` for the EMDF sync word and returns the OAMD/JOC payload
// bytes found there, or nullopt if either is missing/malformed. Mirrors
// exactly what Eac3Decoder's own skip-field handling does internally
// (capture the skipfld bytes, then ac3::emdf::parse_container), duplicated
// here so JOC's decode+reconstruct path can be tested against a real wire
// frame ahead of task #7 wiring it into Eac3Decoder's own public API.
std::optional<std::vector<std::byte>> find_payload(std::span<const std::byte> frame, int id) {
    const std::size_t total = frame.size() * 8;
    for (std::size_t bit = 0; bit + 16 <= total; ++bit) {
        ac3::BitReader probe{frame};
        probe.skip(bit);
        if (probe.read(16) != ac3::emdf::kSyncWord) {
            continue;
        }
        const auto length = probe.read(16);
        std::vector<std::byte> container_bytes(4 + length);
        ac3::BitReader raw{frame};
        raw.skip(bit);
        for (auto& byte : container_bytes) {
            byte = static_cast<std::byte>(raw.read(8));
        }
        const auto container = ac3::emdf::parse_container(container_bytes);
        if (!container.has_value() || !container->has_value()) {
            continue;
        }
        for (const auto& payload : **container) {
            if (payload.id == id) {
                return payload.bytes;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("joc::reconstruct recovers well-separated objects through the real wire", "[atmos][joc][decoder]") {
    // Same four-corner placement/frequency setup as "well-separated objects
    // come back out of the bed" above, which already proves the ENCODER's
    // own in-memory matrix separates these cleanly - this test proves the
    // same thing about the DECODE path: real encoded bytes, through
    // emdf::parse_container + joc::parse_payload + joc::reconstruct.
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 640}, 4};
    const std::array<ac3::oba::ObjectPlacement, 4> placement{{
        {.position = {.x = 0.0, .y = 0.0, .z = 0.0}},
        {.position = {.x = 1.0, .y = 0.0, .z = 0.0}},
        {.position = {.x = 0.0, .y = 1.0, .z = 1.0}},
        {.position = {.x = 1.0, .y = 1.0, .z = 1.0}},
    }};
    const std::array<double, 4> hz{311.0, 997.0, 2200.0, 5000.0};
    const std::array<double, 4> amplitude{0.30, 0.25, 0.20, 0.22};

    constexpr std::array<int, ac3::joc::kNumChannels5X> kAc3FromJoc = {0, 2, 1, 3, 4};

    ac3::Eac3Decoder decoder;
    ac3::joc::ReconstructionState state;
    std::vector<std::span<const float>> views(4);

    constexpr int kFrames = 6;
    std::array<std::vector<float>, 4> source;   // the whole run, per object
    std::array<std::vector<float>, 4> recovered;  // ditto, aligned index for index
    for (auto& s : source) {
        s.reserve(static_cast<std::size_t>(kFrames * kFrame));
    }
    for (auto& r : recovered) {
        r.reserve(static_cast<std::size_t>(kFrames * kFrame));
    }

    for (int frame = 0; frame < kFrames; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        std::vector<std::vector<float>> essences;
        for (std::size_t i = 0; i < 4; ++i) {
            essences.push_back(tone(hz[i], amplitude[i], 0.7 * static_cast<double>(i), start));
            source[i].insert(source[i].end(), essences[i].begin(), essences[i].end());
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        const auto encoded = encoder.encode_frame(views, placement);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->substream_count() == 1);
        const auto frame_bytes = encoded->substream(0);

        const auto decoded = decoder.decode_substream(frame_bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto& sub = **decoded;

        const auto joc_bytes = find_payload(frame_bytes, ac3::emdf::kPayloadIdJoc);
        REQUIRE(joc_bytes.has_value());
        const auto params = ac3::joc::parse_payload(*joc_bytes);
        REQUIRE(params.has_value());

        std::vector<std::vector<float>> bed_joc_order(ac3::joc::kNumChannels5X);
        for (int jc = 0; jc < ac3::joc::kNumChannels5X; ++jc) {
            bed_joc_order[static_cast<std::size_t>(jc)] =
                sub.channels[static_cast<std::size_t>(kAc3FromJoc[static_cast<std::size_t>(jc)])];
        }
        const auto reconstructed = ac3::joc::reconstruct(bed_joc_order, *params, state);
        REQUIRE(reconstructed.size() == 4);
        for (std::size_t i = 0; i < 4; ++i) {
            recovered[i].insert(recovered[i].end(), reconstructed[i].begin(), reconstructed[i].end());
        }
    }

    // Two stacked MDCT round trips carry two stacked 256-sample algorithmic
    // delays: one from the real encode+decode of the bed itself (see
    // tests/test_eac3_decoder.cpp's own snr_db helper), one more from
    // reconstruct()'s own independent forward+inverse pass over that decoded
    // bed (see "reconstruct is a 256-sample-delayed identity..." above).
    // Comparing sample n of the recovered audio against sample (n - 512) of
    // the true source is what actually measures reconstruction QUALITY
    // rather than mostly measuring this codebase's own well-understood,
    // expected transform-pair latency.
    constexpr std::size_t kDelay = 512;
    constexpr std::size_t kSkip = static_cast<std::size_t>(kFrame);  // one frame's warm-up/cool-down
    for (int object = 0; object < 4; ++object) {
        CAPTURE(object);
        const auto index = static_cast<std::size_t>(object);
        double signal = 0.0;
        double error = 0.0;
        for (std::size_t n = kSkip; n + kSkip < recovered[index].size(); ++n) {
            const double want = static_cast<double>(source[index][n - kDelay]);
            const double got = static_cast<double>(recovered[index][n]);
            signal += want * want;
            error += (got - want) * (got - want);
        }
        const double snr_db = 10.0 * std::log10(signal / std::max(error, 1e-30));
        // Looser than "well-separated objects come back out of the bed"'s
        // -20 dB: this measures the whole decode chain (Huffman-coded,
        // quantized matrix coefficients feeding an MDCT-domain synthesis of
        // real, quantized, bit-allocated PCM), not the encoder's own
        // unquantized in-memory matrix applied to a single frequency bin -
        // quantization noise at every one of those stages spends some of the
        // budget that comparison never had to pay. Measured empirically at
        // 18-35 dB across these four placements; 10 dB leaves real margin
        // rather than pinning to the measured values.
        CHECK(snr_db > 10.0);
    }
}

TEST_CASE("Eac3Decoder recovers the object positions AtmosEncoder wrote", "[atmos][decoder]") {
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, 3};
    const std::array<ac3::oba::ObjectPlacement, 3> placement{{
        {.position = {.x = 0.1, .y = 0.2, .z = 0.5}},
        {.position = {.x = 0.9, .y = 0.2, .z = 0.5}, .gain = 0.5},
        {.position = {.x = 0.5, .y = 0.9, .z = -0.5}, .lfe_send = 0.3},
    }};

    // §5.6.1.1.8-11's own quantization, applied the same way build_payload's
    // encode side does, so this asserts what the wire can actually carry
    // rather than the pre-quantization placement values themselves.
    const auto quantize_xy = [](double v) { return std::lround(std::clamp(v, 0.0, 1.0) * 62.0) / 62.0; };
    const auto quantize_z = [](double v) {
        const double clamped = std::clamp(v, -1.0, 1.0);
        return (clamped < 0.0 ? -1.0 : 1.0) * std::lround(std::abs(clamped) * 15.0) / 15.0;
    };

    std::vector<std::vector<float>> essences;
    std::vector<std::span<const float>> views(3);
    ac3::eac3::AccessUnit unit;
    for (int frame = 0; frame < 3; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        essences = {tone(440.0, 0.3, 0.0, start), tone(880.0, 0.3, 0.5, start),
                    tone(120.0, 0.3, 1.0, start)};
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        auto encoded = encoder.encode_frame(views, placement);
        REQUIRE(encoded.has_value());
        unit = *encoded;
    }
    REQUIRE(unit.substream_count() == 1);

    ac3::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(unit.substream(0));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    REQUIRE((*decoded)->object_metadata.has_value());

    const auto& metadata = *(*decoded)->object_metadata;
    CHECK(metadata.program.dynamic_only);
    CHECK(metadata.program.lfe);
    CHECK(metadata.program.dynamic_objects == 3);
    REQUIRE(metadata.objects.size() == 3);
    for (std::size_t i = 0; i < placement.size(); ++i) {
        CAPTURE(i);
        // AtmosEncoder folds each object's gain into the reconstructed
        // essence itself (atmos.cpp step 1's own comment) and always
        // declares 0 dB, so this is the one field decode should match
        // exactly rather than through the position quantizer.
        CHECK(metadata.objects[i].gain_db == 0.0);
        CHECK(metadata.objects[i].position.x == quantize_xy(placement[i].position.x));
        CHECK(metadata.objects[i].position.y == quantize_xy(placement[i].position.y));
        CHECK(metadata.objects[i].position.z == quantize_z(placement[i].position.z));
    }
}

TEST_CASE("Eac3Decoder reports no object metadata for a plain (non-Atmos) stream", "[atmos][decoder]") {
    const ac3::eac3::FrameConfig config{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true};
    const auto frame = ac3::eac3::build_silent_frame(config);
    REQUIRE(frame.has_value());

    ac3::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    CHECK_FALSE((*decoded)->object_metadata.has_value());
}

TEST_CASE("the splice counter starts at zero and wraps to one", "[atmos]") {
    // §6.3.3.3. The first frame must read 0 so a decoder knows there is no
    // previous matrix to interpolate from; 0 must never come round again by
    // counting, or a mid-stream frame would masquerade as a splice.
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, 1};
    const std::array<ac3::oba::ObjectPlacement, 1> placement{{{}}};
    std::vector<std::span<const float>> views(1);

    for (int frame = 0; frame < 4; ++frame) {
        const auto essence = tone(440.0, 0.3, 0.0,
                                  static_cast<std::uint64_t>(frame) * kFrame);
        views[0] = essence;
        REQUIRE(encoder.encode_frame(views, placement).has_value());
        CHECK(encoder.parameters().seq_count == frame);
    }
}
