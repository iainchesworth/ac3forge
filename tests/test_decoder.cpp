#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"

namespace {

// Multi-frame encode -> decode of per-channel tones; returns concatenated
// decoded PCM per channel (AC-3 order).
struct RoundTrip {
    std::vector<std::vector<float>> input;    // per channel, full length
    std::vector<std::vector<float>> decoded;  // per channel, full length
};

RoundTrip round_trip(const ac3::EncoderConfig& config, const std::vector<double>& tones,
                     int frames, double amplitude = 0.4) {
    ac3::FrameEncoder encoder{config};
    ac3::FrameDecoder decoder;
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(tones.size() == nchans);

    RoundTrip rt;
    rt.input.resize(nchans);
    rt.decoded.resize(nchans);
    std::vector<std::vector<float>> block(nchans,
                                          std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                block[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                    amplitude * std::sin(2.0 * std::numbers::pi * tones[ch] *
                                         static_cast<double>(n0 + static_cast<std::uint64_t>(i)) /
                                         sample_rate_hz(config.sample_rate)));
            }
            views[ch] = block[ch];
            rt.input[ch].insert(rt.input[ch].end(), block[ch].begin(), block[ch].end());
        }
        n0 += ac3::kSamplesPerFrame;
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        const auto decoded = decoder.decode_frame(*frame);
        REQUIRE(decoded.has_value());
        CHECK(decoded->acmod == config.acmod);
        CHECK(decoded->lfe == config.lfe);
        CHECK(decoded->sample_rate == config.sample_rate);
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            rt.decoded[ch].insert(rt.decoded[ch].end(), decoded->channels[ch].begin(),
                                  decoded->channels[ch].end());
        }
    }
    return rt;
}

// SNR of decoded vs input with the 256-sample encode+decode delay, skipping
// the warm-up frame at each end.
double snr_db(const std::vector<float>& input, const std::vector<float>& decoded) {
    constexpr std::size_t kDelay = 256;
    constexpr std::size_t kSkip = 1536;
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = kSkip; i + kSkip < input.size(); ++i) {
        const double x = input[i - kDelay];
        const double d = decoded[i] - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

double dominant_freq_hz(const std::vector<float>& x, double rate) {
    // Goertzel-free coarse scan: correlate against candidate bins via DFT at
    // 1 Hz steps is overkill; use zero-crossing-free spectral peak via
    // naive DFT over a small candidate set instead. Tones are known values,
    // so scan 50..2000 Hz in 10 Hz steps and refine +-5.
    double best_f = 0.0;
    double best_m = -1.0;
    const std::size_t n0 = 2048;
    const std::size_t len = std::min<std::size_t>(8192, x.size() - n0);
    for (double f = 50.0; f <= 2000.0; f += 10.0) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 0; i < len; ++i) {
            const double phase = 2.0 * std::numbers::pi * f * static_cast<double>(i) / rate;
            re += x[n0 + i] * std::cos(phase);
            im += x[n0 + i] * std::sin(phase);
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

// Threshold note: these are DIRECT sample-comparison SNRs, which include the
// tone's own amplitude/phase quantization — a stricter metric than the
// sine-fit SNR the FFmpeg oracle reports (88 dB on the same encode). The
// decoder's correctness anchor is PCM parity with FFmpeg's decoder on
// identical streams: measured max diff 7.9e-6 (~-102 dBFS), float32
// precision agreement (ac3cli decode vs ffmpeg -c:a pcm_f32le).
TEST_CASE("stereo round trip through the in-repo decoder is near-transparent", "[decoder]") {
    const auto rt = round_trip({.bitrate_kbps = 192}, {1000.0, 1000.0}, 4);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        CAPTURE(ch);
        CHECK(snr_db(rt.input[ch], rt.decoded[ch]) > 45.0);  // measured ~52
    }
}

TEST_CASE("5.1 round trip: every channel keeps its own tone", "[decoder]") {
    const std::vector<double> tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const auto rt = round_trip(
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}, tones, 4);
    for (std::size_t ch = 0; ch < tones.size(); ++ch) {
        CAPTURE(ch);
        // Channel-order lock: the decoded channel's dominant frequency must
        // be its own tone, not a neighbour's.
        CHECK(std::abs(dominant_freq_hz(rt.decoded[ch], 48000.0) - tones[ch]) < 10.0);
        // Six channels share the 448 kbps pool at full bandwidth (~75 kbps
        // each); worst measured channel ~38.7 dB on the direct metric.
        CHECK(snr_db(rt.input[ch], rt.decoded[ch]) > 34.0);
    }
}

TEST_CASE("every acmod round-trips at every sample rate", "[decoder]") {
    using ac3::Acmod;
    for (const auto sr :
         {ac3::SampleRate::k48000, ac3::SampleRate::k44100, ac3::SampleRate::k32000}) {
        for (const auto acmod : {Acmod::k1_0, Acmod::k3_0, Acmod::k2_2, Acmod::k3_2}) {
            for (const bool lfe : {false, true}) {
                CAPTURE(static_cast<int>(sr), static_cast<int>(acmod), lfe);
                const auto nchans =
                    static_cast<std::size_t>(ac3::fullbw_channel_count(acmod)) + (lfe ? 1 : 0);
                std::vector<double> tones(nchans);
                for (std::size_t ch = 0; ch < nchans; ++ch) {
                    tones[ch] = 200.0 + 150.0 * static_cast<double>(ch);
                }
                if (lfe) {
                    tones.back() = 60.0;
                }
                const auto rt = round_trip(
                    {.sample_rate = sr, .bitrate_kbps = 448, .acmod = acmod, .lfe = lfe}, tones,
                    3);
                for (std::size_t ch = 0; ch < nchans; ++ch) {
                    CAPTURE(ch);
                    CHECK(snr_db(rt.input[ch], rt.decoded[ch]) > 35.0);
                }
            }
        }
    }
}

TEST_CASE("decoder rejects corrupted streams", "[decoder]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(2, silence);
    auto frame = encoder.encode_frame(views).value();

    ac3::FrameDecoder decoder;
    SECTION("bad sync word") {
        frame[0] = std::byte{0x0C};
        CHECK(decoder.decode_frame(frame).error() == ac3::DecodeError::kBadSyncWord);
    }
    SECTION("flipped payload bit fails CRC") {
        frame[100] ^= std::byte{0x10};
        CHECK(decoder.decode_frame(frame).error() == ac3::DecodeError::kBadCrc);
    }
    SECTION("truncated") {
        CHECK(decoder.decode_frame(std::span{frame}.first(frame.size() - 2)).error() ==
              ac3::DecodeError::kTruncated);
    }
}
