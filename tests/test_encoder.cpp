#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"

namespace {

std::vector<float> sine_frame(std::uint64_t& n, double freq, double amplitude) {
    std::vector<float> samples(ac3::kSamplesPerFrame);
    for (auto& s : samples) {
        s = static_cast<float>(amplitude *
                               std::sin(2.0 * std::numbers::pi * freq * n / 48000.0));
        ++n;
    }
    return samples;
}

std::expected<std::vector<std::byte>, ac3::FrameError> encode_same(
    ac3::FrameEncoder& encoder, const std::vector<float>& samples) {
    std::vector<std::span<const float>> views(
        static_cast<std::size_t>(encoder.channel_count()), samples);
    return encoder.encode_frame(views);
}

void check_frame_invariants(const std::vector<std::byte>& frame, ac3::SampleRate sr,
                            std::uint32_t kbps) {
    CHECK(frame.size() == ac3::frame_size_bytes(sr, kbps).value());
    const std::span<const std::byte> bytes{frame};
    const auto words = static_cast<std::uint32_t>(frame.size()) / 2;
    const std::uint32_t words58 = ac3::frame_size_58_words(words);
    CHECK(ac3::crc16(bytes.subspan(2, 2 * words58 - 2)) == 0x0000);
    CHECK(ac3::crc16(bytes.subspan(2)) == 0x0000);
    CHECK(std::to_integer<std::uint8_t>(bytes[0]) == 0x0B);
    CHECK(std::to_integer<std::uint8_t>(bytes[1]) == 0x77);
}

}  // namespace

TEST_CASE("encoded sine frames satisfy the frame invariants at every bitrate", "[encoder]") {
    for (const std::uint32_t kbps : {96u, 192u, 448u, 640u}) {
        CAPTURE(kbps);
        ac3::FrameEncoder encoder{{.bitrate_kbps = kbps}};
        std::uint64_t n = 0;
        for (int f = 0; f < 3; ++f) {
            const auto frame = encode_same(encoder, sine_frame(n, 1000.0, 0.5));
            REQUIRE(frame.has_value());
            check_frame_invariants(*frame, ac3::SampleRate::k48000, kbps);
        }
    }
}

TEST_CASE("every acmod with and without LFE produces valid frames", "[encoder]") {
    using ac3::Acmod;
    for (const auto acmod : {Acmod::k1_0, Acmod::k2_0, Acmod::k3_0, Acmod::k2_1, Acmod::k3_1,
                             Acmod::k2_2, Acmod::k3_2}) {
        for (const bool lfe : {false, true}) {
            CAPTURE(static_cast<int>(acmod), lfe);
            ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = acmod, .lfe = lfe}};
            std::uint64_t n = 0;
            const auto frame = encode_same(encoder, sine_frame(n, 500.0, 0.4));
            REQUIRE(frame.has_value());
            check_frame_invariants(*frame, ac3::SampleRate::k48000, 448);
        }
    }
}

TEST_CASE("44.1 kHz CBR alternates frame sizes to the exact long-run rate", "[encoder]") {
    // 448 kbps @ 44.1 kHz: ideal 975.238 words/frame -> mix of 975 and 976.
    ac3::FrameEncoder encoder{
        {.sample_rate = ac3::SampleRate::k44100, .bitrate_kbps = 448}};
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    std::uint64_t total_bytes = 0;
    int padded = 0;
    constexpr int kFrames = 84;  // one full alternation cycle (975.238... has period 21)
    for (int f = 0; f < kFrames; ++f) {
        const auto frame = encode_same(encoder, silence);
        REQUIRE(frame.has_value());
        REQUIRE((frame->size() == 1950 || frame->size() == 1952));
        padded += frame->size() == 1952 ? 1 : 0;
        total_bytes += frame->size();
    }
    CHECK(padded > 0);  // alternation actually happens
    // Exact CBR: total ideal bits = frames * kbps*1000*1536/44100; the
    // accumulator keeps the emitted total within one word of ideal.
    const double ideal_bytes = kFrames * 448000.0 * 1536.0 / 44100.0 / 8.0;
    CHECK(std::abs(static_cast<double>(total_bytes) - ideal_bytes) <= 2.0);
}

TEST_CASE("coupling produces valid frames across configurations", "[encoder][coupling]") {
    using ac3::Acmod;
    // Coupling needs >= 2 fbw channels; sweep the sub-band range including
    // the extremes, where the coded region is widest and narrowest.
    for (const auto acmod : {Acmod::k2_0, Acmod::k3_2}) {
        for (const auto [begf, endf] : {std::pair{6, 12}, std::pair{0, 15}, std::pair{12, 2}}) {
            for (const std::uint32_t kbps : {192u, 384u}) {
                CAPTURE(static_cast<int>(acmod), begf, endf, kbps);
                ac3::FrameEncoder encoder{{.bitrate_kbps = kbps,
                                           .acmod = acmod,
                                           .lfe = acmod == Acmod::k3_2,
                                           .coupling = true,
                                           .cplbegf = begf,
                                           .cplendf = endf}};
                std::uint64_t n = 0;
                for (int f = 0; f < 2; ++f) {
                    const auto frame = encode_same(encoder, sine_frame(n, 2200.0, 0.5));
                    REQUIRE(frame.has_value());
                    check_frame_invariants(*frame, ac3::SampleRate::k48000, kbps);
                }
            }
        }
    }
}

TEST_CASE("coupling below two channels is silently inactive", "[encoder][coupling]") {
    // 1/0 has nothing to couple; the encoder must fall back rather than emit
    // a coupling strategy no decoder could use.
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k1_0, .coupling = true}};
    std::uint64_t n = 0;
    const auto frame = encode_same(encoder, sine_frame(n, 1000.0, 0.5));
    REQUIRE(frame.has_value());
    check_frame_invariants(*frame, ac3::SampleRate::k48000, 192);
}

TEST_CASE("encoding is deterministic", "[encoder]") {
    ac3::FrameEncoder a{{.bitrate_kbps = 256}};
    ac3::FrameEncoder b{{.bitrate_kbps = 256}};
    std::uint64_t n1 = 0;
    std::uint64_t n2 = 0;
    for (int f = 0; f < 2; ++f) {
        const auto frame1 = encode_same(a, sine_frame(n1, 3000.0, 0.8));
        const auto frame2 = encode_same(b, sine_frame(n2, 3000.0, 0.8));
        REQUIRE(frame1.has_value());
        REQUIRE(frame2.has_value());
        CHECK(*frame1 == *frame2);
    }
}

TEST_CASE("invalid encoder configs are rejected", "[encoder]") {
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    ac3::FrameEncoder bad_rate{{.bitrate_kbps = 100}};
    CHECK(encode_same(bad_rate, silence).error() == ac3::FrameError::kInvalidBitrate);
    ac3::FrameEncoder bad_dialnorm{{.bitrate_kbps = 192, .dialnorm = 0}};
    CHECK(encode_same(bad_dialnorm, silence).error() == ac3::FrameError::kInvalidDialnorm);
}
