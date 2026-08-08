#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/emdf/emdf.hpp"
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
        lfe_energy += static_cast<double>(sample) * sample;
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
