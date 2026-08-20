#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"

namespace {

// Overwrite `count` bits at `offset` and restore both AC-3 CRCs (§7.10.1):
// crc1 precedes the first 5/8 of the frame it protects and has to be SOLVED
// for (ac3::solve_leading_crc), crc2 is a plain trailing CRC over everything
// after the sync word. Mirrors encoder.cpp's own tail-patching logic, so a
// hand-patched frame stays a legal syncframe and only the flipped bit's own
// meaning changes.
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
    const auto words = static_cast<std::uint32_t>(bytes / 2);
    const auto words58 = ac3::frame_size_58_words(words);
    const std::span<const std::byte> view{frame};
    const std::uint16_t crc1 = ac3::solve_leading_crc(view.subspan(4, 2 * words58 - 4));
    frame[2] = static_cast<std::byte>(crc1 >> 8);
    frame[3] = static_cast<std::byte>(crc1 & 0xFF);
    const std::uint16_t crc2 = ac3::crc16(view.subspan(2, bytes - 4));
    frame[bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
}

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
        const double x = static_cast<double>(input[i - kDelay]);
        const double d = static_cast<double>(decoded[i]) - x;
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

TEST_CASE("dynamic and correlated material exercises strategies and rematrixing", "[decoder]") {
    // A spectrum jump mid-frame forces multi-run exponent plans (D45/D25
    // per §8.2.8); near-mono content drives the rematrix decision. Both
    // must round-trip cleanly through the in-repo decoder's undo paths.
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    ac3::FrameDecoder decoder;
    std::vector<std::vector<float>> input(2);
    std::vector<std::vector<float>> decoded(2);
    std::vector<std::vector<float>> block(2, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(2);
    std::uint64_t n0 = 0;
    for (int f = 0; f < 4; ++f) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(n0 + static_cast<std::uint64_t>(i));
            // Tone hops every half frame; channels nearly identical (mono-ish).
            const double freq = (static_cast<int>(n) / 768) % 2 == 0 ? 400.0 : 2600.0;
            const auto mono =
                static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * freq * n / 48000.0));
            block[0][static_cast<std::size_t>(i)] = mono;
            block[1][static_cast<std::size_t>(i)] = 0.97f * mono;
        }
        n0 += ac3::kSamplesPerFrame;
        views[0] = block[0];
        views[1] = block[1];
        for (std::size_t ch = 0; ch < 2; ++ch) {
            input[ch].insert(input[ch].end(), block[ch].begin(), block[ch].end());
        }
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        const auto result = decoder.decode_frame(*frame);
        REQUIRE(result.has_value());
        for (std::size_t ch = 0; ch < 2; ++ch) {
            decoded[ch].insert(decoded[ch].end(), result->channels[ch].begin(),
                               result->channels[ch].end());
        }
    }
    for (std::size_t ch = 0; ch < 2; ++ch) {
        CAPTURE(ch);
        // Tone hops smear across block boundaries (long blocks only), so the
        // bar is lower than for the steady tones above.
        CHECK(snr_db(input[ch], decoded[ch]) > 22.0);
    }
}

TEST_CASE("coupled streams round-trip through the in-repo decoder", "[decoder][coupling]") {
    // Coupling discards inter-channel detail above the coupling frequency,
    // so the bar here is the low band (which coupling must leave alone) plus
    // the stream decoding at all. The high-band envelope is checked
    // separately by tools/checks/check_coupling.py against FFmpeg.
    using ac3::Acmod;
    for (const auto acmod : {Acmod::k2_0, Acmod::k3_2}) {
        for (const auto& [begf, endf] : {std::pair{6, 12}, std::pair{0, 15}}) {
            CAPTURE(static_cast<int>(acmod), begf, endf);
            const auto nchans =
                static_cast<std::size_t>(ac3::fullbw_channel_count(acmod));
            std::vector<double> tones(nchans);
            for (std::size_t ch = 0; ch < nchans; ++ch) {
                // All below the lowest coupling start (37 * 93.75 Hz ~ 3.4 kHz)
                // so the comparison is against content coupling preserves.
                tones[ch] = 400.0 + 200.0 * static_cast<double>(ch);
            }
            const auto rt = round_trip({.bitrate_kbps = 448,
                                        .acmod = acmod,
                                        .coupling = true,
                                        .cplbegf = begf,
                                        .cplendf = endf},
                                       tones, 3);
            for (std::size_t ch = 0; ch < nchans; ++ch) {
                CAPTURE(ch);
                CHECK(snr_db(rt.input[ch], rt.decoded[ch]) > 30.0);
            }
        }
    }
}

namespace {

// Energy at one frequency, over the steady middle of the signal.
double tone_energy(const std::vector<float>& x, double hz, double rate) {
    constexpr std::size_t kSkip = 2048;
    const std::size_t len = x.size() - 2 * kSkip;
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < len; ++i) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate;
        re += static_cast<double>(x[kSkip + i]) * std::cos(phase);
        im += static_cast<double>(x[kSkip + i]) * std::sin(phase);
    }
    return (re * re + im * im) / static_cast<double>(len * len);
}

}  // namespace

TEST_CASE("grouped coupling bands land on the bins they were measured from",
          "[decoder][coupling]") {
    // The encoder joins sub-bands into wider bands towards the top of the
    // spectrum and transmits the join pattern as cplbndstrc; the decoder
    // rebuilds the same partition from those bits alone. Agreeing on the
    // COUNT is not enough - the two sides also have to agree on which bins
    // each coordinate covers, and a stream whose bands are offset by one
    // still decodes, still passes CRC, and puts each channel's energy in the
    // wrong place.
    //
    // Each channel gets a tone in a different part of the coupled region, so
    // a coordinate applied to the wrong bins shows up as one channel
    // inheriting the other's tone.
    constexpr double kRate = 48000.0;
    constexpr double kLow = 12000.0;   // sub-band 6 of the coupled region
    constexpr double kHigh = 20000.0;  // near the top, where bands are widest
    const auto rt = round_trip(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .coupling = true}, {kLow, kHigh}, 4);

    const double left_own = tone_energy(rt.decoded[0], kLow, kRate);
    const double left_other = tone_energy(rt.decoded[0], kHigh, kRate);
    const double right_own = tone_energy(rt.decoded[1], kHigh, kRate);
    const double right_other = tone_energy(rt.decoded[1], kLow, kRate);
    CAPTURE(left_own, left_other, right_own, right_other);
    // Each channel keeps its own tone and does not acquire the other's. The
    // margin is deliberately loose - coupling shares a single channel, so
    // some leakage is the tool working as designed, not a fault.
    CHECK(10.0 * std::log10(left_own / std::max(left_other, 1e-30)) > 20.0);
    CHECK(10.0 * std::log10(right_own / std::max(right_other, 1e-30)) > 20.0);
    // And the level survives: the tone is still there at roughly its input
    // amplitude, not scaled by a neighbouring band's coordinate.
    const double reference = tone_energy(rt.input[0], kLow, kRate);
    CHECK(10.0 * std::log10(left_own / std::max(reference, 1e-30)) > -3.0);
    CHECK(10.0 * std::log10(left_own / std::max(reference, 1e-30)) < 3.0);
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

TEST_CASE("every decode error describes itself", "[decoder]") {
    // A switch that has fallen behind its enum still compiles — no warning
    // level here flags a missing case — and quietly answers "unknown decode
    // error" for whichever value was added last. Only enumerating them
    // catches that.
    constexpr std::array<ac3::DecodeError, 6> all = {
        ac3::DecodeError::kTruncated,   ac3::DecodeError::kBadSyncWord,
        ac3::DecodeError::kBadCrc,      ac3::DecodeError::kReservedValue,
        ac3::DecodeError::kUnsupported, ac3::DecodeError::kInvalidStream,
    };
    std::set<std::string_view> seen;
    for (const auto error : all) {
        CAPTURE(static_cast<int>(error));
        const auto text = ac3::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown decode error");
        // Distinct: two errors sharing a sentence would send a reader looking
        // in the wrong place.
        CHECK(seen.insert(text).second);
    }
}

TEST_CASE("a real transient triggers block switching and decodes without pre-echo",
         "[decoder][block-switching]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k2_0}};
    ac3::FrameDecoder decoder;
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());

    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    std::vector<std::span<const float>> silence_views(nchans, silence);
    // Two silent frames: the first primes history_, the second clears the
    // transient detector's own first-pass guard (which - correctly, see
    // TransientDetector's own comment - never flags a transient on the very
    // first pass it ever runs, having nothing to compare against).
    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silence_views);
        REQUIRE(frame.has_value());
        REQUIRE(decoder.decode_frame(*frame).has_value());
    }

    // Silence for the first ~5/8 of the frame, then a sudden, loud 1 kHz
    // tone - squarely inside some block's second half, which is exactly
    // what §8.2.2 defines blksw from.
    constexpr int kOnset = 960;
    std::vector<float> transient(static_cast<std::size_t>(ac3::kSamplesPerFrame), 0.0f);
    for (int n = kOnset; n < ac3::kSamplesPerFrame; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    std::vector<std::span<const float>> transient_views(nchans, transient);
    const auto frame = encoder.encode_frame(transient_views);
    REQUIRE(frame.has_value());

    const auto decoded = decoder.decode_frame(*frame);
    REQUIRE(decoded.has_value());

    bool any_switched = false;
    for (const auto& channel : decoded->blksw) {
        for (const bool sw : channel) {
            any_switched = any_switched || sw;
        }
    }
    CHECK(any_switched);

    // The transform's own 256-sample overlap means "before the onset" for
    // reconstruction purposes starts a block-length earlier than the onset
    // itself; short of that margin, near-silence should stay near-silent
    // rather than smearing the onset backward in time.
    for (std::size_t ch = 0; ch < nchans; ++ch) {
        double pre_energy = 0.0;
        for (int n = 0; n < kOnset - 256; ++n) {
            const double v =
                static_cast<double>(decoded->channels[ch][static_cast<std::size_t>(n)]);
            pre_energy += v * v;
        }
        CHECK(pre_energy < 1e-4);
    }
}

TEST_CASE("dithflag=1 substitutes dither at zero-bap bins instead of silence",
          "[decoder][dither]") {
    // §7.2.2.1.1: with every SNR offset at zero, the bit allocation goes
    // fully zero-bap - a genuinely silent frame allocates NO mantissa bits
    // anywhere. Unlike CONTRIBUTING.md's general warning against silence as
    // a test signal (which is about exercising the encoder broadly),
    // silence is exactly the right stimulus HERE: every bin in the frame is
    // bap == 0, isolating §7.3.4's dither-substitution path with nothing
    // else able to confound the result.
    //
    // This project's own encoder always writes dithflag == 0 (see
    // encoder.cpp), so the frame is patched by hand to flip block 0's
    // dithflag[0] to 1 and both CRCs are restored - legal per §7.10.1, and
    // the ONLY thing it changes is what a bap-0 mantissa in channel 0's
    // first block reconstructs as.
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192}};  // default acmod k2_0, no LFE
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(2, silence);
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());

    // Bit offset of block 0's dithflag[0], cross-checked against
    // test_encoder.cpp's own parse_block_zero: syncinfo (40) + bsi for 2/0
    // without LFE (27, through addbsie) + block 0's blksw[0..1] (2) puts
    // dithflag[0] at bit 69, immediately followed by dithflag[1].
    constexpr std::size_t kDithflagBit0 = 40 + 27 + 2;

    // Baseline: dithflag == 0 (the encoder's own default) must still decode
    // to literal zero throughout - confirms the "off" half of §7.3.4 still
    // holds after adding the "on" half.
    {
        ac3::FrameDecoder decoder;
        const auto decoded = decoder.decode_frame(*frame);
        REQUIRE(decoded.has_value());
        for (const float v : decoded->channels[0]) {
            CHECK(v == 0.0f);
        }
        for (const float v : decoded->channels[1]) {
            CHECK(v == 0.0f);
        }
    }

    auto patched = *frame;
    patch_bits(patched, kDithflagBit0, 1, 1);  // dithflag[0] = 1

    // Determinism: two independent decoder instances given the same patched
    // frame must produce bit-identical PCM - DitherGenerator is seeded the
    // same way every time, so "random-looking" does not mean "unreproducible".
    ac3::FrameDecoder decoder_a;
    const auto decoded_a = decoder_a.decode_frame(patched);
    REQUIRE(decoded_a.has_value());
    ac3::FrameDecoder decoder_b;
    const auto decoded_b = decoder_b.decode_frame(patched);
    REQUIRE(decoded_b.has_value());
    CHECK(decoded_a->channels[0] == decoded_b->channels[0]);

    // Channel 0 now has real energy (dither), channel 1's dithflag was never
    // touched and must stay exactly silent - proving the substitution is
    // scoped to precisely the bit that was flipped, not a blanket change.
    double ch0_energy = 0.0;
    for (const float v : decoded_a->channels[0]) {
        ch0_energy += static_cast<double>(v) * static_cast<double>(v);
    }
    CHECK(ch0_energy > 0.0);
    for (const float v : decoded_a->channels[1]) {
        CHECK(v == 0.0f);
    }
}

TEST_CASE("dithflag=1 on a coupled channel dithers independently of its sibling",
          "[decoder][dither][coupling]") {
    // §7.3.4: "Dither is applied after the individual channels are
    // extracted from the coupling channel. In this way, the dither applied
    // to each channel's upper frequencies is uncorrelated." A stereo,
    // silent, coupled frame puts both channels' shared high band through
    // the SAME zero-bap coupling-channel bins; each channel's own dithflag
    // must still gate its OWN independent noise there, not a shared one.
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .coupling = true}};
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(2, silence);
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());

    // Same bit layout as the plain stereo case up to block 0's dithflag:
    // coupling strategy fields live AFTER dithflag/dynrnge in block 0's own
    // syntax (§5.4.3.11 cplstre follows dynrnge), so the offset does not
    // move just because coupling is on.
    constexpr std::size_t kDithflagBit0 = 40 + 27 + 2;

    auto patched = *frame;
    patch_bits(patched, kDithflagBit0, 2, 0b11);  // dithflag[0] = dithflag[1] = 1

    ac3::FrameDecoder decoder;
    const auto decoded = decoder.decode_frame(patched);
    REQUIRE(decoded.has_value());

    // Both channels get real, and - because each draws its own independent
    // dither sample rather than sharing one scaled coupling-domain value -
    // DIFFERENT energy/noise, not a common signal scaled by each channel's
    // coordinate.
    double ch0_energy = 0.0;
    double ch1_energy = 0.0;
    bool any_differs = false;
    for (std::size_t i = 0; i < decoded->channels[0].size(); ++i) {
        const double a = static_cast<double>(decoded->channels[0][i]);
        const double b = static_cast<double>(decoded->channels[1][i]);
        ch0_energy += a * a;
        ch1_energy += b * b;
        if (std::abs(a - b) > 1e-9) {
            any_differs = true;
        }
    }
    CHECK(ch0_energy > 0.0);
    CHECK(ch1_energy > 0.0);
    CHECK(any_differs);
}
