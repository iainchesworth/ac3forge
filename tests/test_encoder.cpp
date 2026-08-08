#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/coupling.hpp"
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

// Program-like stereo with energy right across the spectrum and a DIFFERENT
// balance per channel. A single tone tells a working coupling implementation
// from a broken one about as well as silence does: below the coupling
// frequency there is nothing to share, and with identical channels every
// coordinate comes out the same whatever the encoder got wrong.
//
// The tones above the coupling frequency sit one in the middle of each of the
// nine default coupling sub-bands (bins 109..216, ~10.2-20.3 kHz), so each
// band's coordinate is set by a signal of its own rather than by its
// neighbour's skirt, and both channels carry that signal at the SAME
// frequency with different weights. Each coupled band is then exactly a
// scaled copy between the channels, which pins its magnitude ratio to the two
// weights - a fact of the material that no gain and no envelope can move.
//
// `gain` scales everything; `tremolo` adds an amplitude envelope at one cycle
// per frame, applied identically to every channel, so the level swings block
// to block while those ratios do not.
std::vector<std::vector<float>> wideband_frame(int channels, std::uint64_t start, double gain = 1.0,
                                               double tremolo = 0.0) {
    constexpr int kCoupledBands = 9;
    constexpr double kBinHz = 48000.0 / 512.0;
    std::vector<double> tones = {310.0, 1450.0, 5200.0, 8100.0};  // the baseband's share
    std::vector<double> tilt(tones.size(), 1.0);
    for (int b = 0; b < kCoupledBands; ++b) {
        tones.push_back((ac3::coupling::start_mant(6) + 12 * b + 6) * kBinHz);
        // Real program rolls off across the coupled region, and a flat one
        // would hide the very thing coupling is judged on: what an encoder
        // does with the QUIET top bands. -2 dB a band, applied to both
        // channels alike so the ratios stay put.
        tilt.push_back(std::pow(10.0, -0.10 * b));
    }
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            double value = 0.0;
            for (std::size_t t = 0; t < tones.size(); ++t) {
                // A weight pattern that walks differently for each channel, so
                // the coordinates genuinely differ band to band and channel to
                // channel instead of collapsing to one number.
                const double weight =
                    tilt[t] * 0.12 / (1.0 + static_cast<double>((t + 2 * ch) % 5));
                value += weight * std::sin(2.0 * std::numbers::pi * tones[t] * n / 48000.0);
            }
            const double envelope =
                1.0 + tremolo * std::sin(2.0 * std::numbers::pi * n /
                                         static_cast<double>(ac3::kSamplesPerFrame));
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(gain * envelope * value);
        }
    }
    return pcm;
}

// Encode `count` frames and hand back the last, so the MDCT history is real
// rather than the half-empty window the first frame sees.
std::vector<std::byte> steady_state_frame(const ac3::EncoderConfig& config, int channels,
                                          double gain = 1.0, double tremolo = 0.0, int count = 3) {
    ac3::FrameEncoder encoder{config};
    std::vector<std::byte> last;
    std::uint64_t n = 0;
    for (int f = 0; f < count; ++f) {
        const auto pcm = wideband_frame(channels, n, gain, tremolo);
        n += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        last = std::move(*frame);
    }
    return last;
}

// Everything block 0's side information gives up without decoding the frame.
// Later blocks are not reachable this way - their side information sits
// behind block 0's mantissas, whose length only the bit allocation knows -
// which is why the coupling tests below all read block 0.
struct BlockZero {
    bool cplinu = false;
    int ncplsubnd = 0;
    std::vector<int> master;                        // one per fbw channel
    std::vector<ac3::coupling::Coordinate> coords;  // [ch][bnd]
    int snroffst = 0;                               // (csnroffst << 4) | fsnroffst
};

// Parses §5.4 syncinfo/bsi/audblk far enough to reach csnroffst, for the 2/0
// no-LFE frames these tests encode.
BlockZero parse_block_zero(std::span<const std::byte> frame) {
    constexpr int kNfchans = 2;
    BlockZero out;
    ac3::BitReader r{frame};
    r.skip(40);                // syncinfo: syncword, crc1, fscod, frmsizecod
    r.skip(27);                // bsi for 2/0 without LFE, through addbsie
    r.skip(kNfchans * 2 + 1);  // blksw, dithflag, dynrnge
    r.skip(1);                 // cplstre, always 1 in block 0
    out.cplinu = r.read(1) != 0;

    int cplbegf = 0;
    int cplstrtmant = 0;
    int cplendmant = 0;
    if (out.cplinu) {
        r.skip(kNfchans);  // chincpl
        r.skip(1);         // phsflginu, 2/0 only
        cplbegf = static_cast<int>(r.read(4));
        const int cplendf = static_cast<int>(r.read(4));
        cplstrtmant = ac3::coupling::start_mant(cplbegf);
        cplendmant = std::min(ac3::coupling::end_mant(cplendf), 253);
        out.ncplsubnd = (cplendmant - cplstrtmant) / ac3::coupling::kBinsPerSubBand;
        r.skip(static_cast<std::size_t>(out.ncplsubnd - 1));  // cplbndstrc
        for (int ch = 0; ch < kNfchans; ++ch) {
            REQUIRE(r.read(1) == 1);  // cplcoe: block 0 always sends coordinates
            out.master.push_back(static_cast<int>(r.read(2)));
            for (int bnd = 0; bnd < out.ncplsubnd; ++bnd) {
                const auto exp = static_cast<std::uint8_t>(r.read(4));
                const auto mant = static_cast<std::uint8_t>(r.read(4));
                out.coords.push_back({.exp = exp, .mant = mant});
            }
        }
    }

    REQUIRE(r.read(1) == 1);  // rematstr, always sent in block 0
    const int nrematbd = !out.cplinu || cplbegf > 2 ? 4 : (cplbegf > 0 ? 3 : 2);  // §7.5.2
    r.skip(static_cast<std::size_t>(nrematbd));

    // Exponent strategies: the coupling channel first, then the fbw channels.
    if (out.cplinu) {
        r.skip(2);  // cplexpstr
    }
    std::array<ac3::ExpStrategy, kNfchans> strategy{};
    for (int ch = 0; ch < kNfchans; ++ch) {
        strategy[static_cast<std::size_t>(ch)] = static_cast<ac3::ExpStrategy>(r.read(2));
    }
    // chbwcod exists only for channels NOT in coupling; block 0 always starts
    // a fresh exponent set, so every uncoupled channel carries one.
    std::array<int, kNfchans> endmant{};
    endmant.fill(cplstrtmant);
    if (!out.cplinu) {
        for (int ch = 0; ch < kNfchans; ++ch) {
            endmant[static_cast<std::size_t>(ch)] = (static_cast<int>(r.read(6)) + 12) * 3 + 37;
        }
    }

    // Exponents, same order. The coupling channel is always D15, whose group
    // count is simply one per three coupled bins.
    if (out.cplinu) {
        r.skip(4);  // cplabsexp
        r.skip(static_cast<std::size_t>((cplendmant - cplstrtmant) / 3) * 7);
    }
    for (int ch = 0; ch < kNfchans; ++ch) {
        r.skip(4);  // exps[ch][0]
        r.skip(static_cast<std::size_t>(ac3::exponent_group_count(
                   strategy[static_cast<std::size_t>(ch)], endmant[static_cast<std::size_t>(ch)])) *
               7);
        r.skip(2);  // gainrng
    }

    REQUIRE(r.read(1) == 1);    // baie
    r.skip(2 + 2 + 2 + 2 + 3);  // sdcycod, fdcycod, sgaincod, dbpbcod, floorcod
    REQUIRE(r.read(1) == 1);    // snroffste
    const auto csnroffst = r.read(6);
    // The first fine offset belongs to the coupling channel when coupled and
    // to channel 0 otherwise; this encoder gives every stream the same one.
    const auto fsnroffst = r.read(4);
    out.snroffst = static_cast<int>((csnroffst << 4) | fsnroffst);
    REQUIRE_FALSE(r.overflowed());
    return out;
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

TEST_CASE("coupling must not cost more bits than the channels it replaces", "[encoder][coupling]") {
    // Coupling replaces two channels' high bands with one shared channel, so
    // the frame should be able to afford a HIGHER SNR offset, not a lower one.
    //
    // The way to get this backwards is to scale the shared channel up - to
    // normalise it per band, or to the region peak - which makes the
    // coordinates comfortably small but hands the allocator a channel that
    // reads as full scale. §7.2.2 measures psd absolutely, so the allocator
    // then buys the coupling channel more bits per bin than the baseband it
    // was meant to be subsidising, and the offset collapses. On the E-AC-3
    // side, where the same mistake was found, that was 27 coarse steps down
    // to 11 at 128 kbit/s; here it turns a 15-step gain into an 8-step loss.
    // The frame still decodes, and still passes every size and CRC check,
    // which is exactly why this needs its own test.
    //
    // chbwcod 48 puts the uncoupled channels' last mantissa exactly where
    // coupling ends (bin 216), so both frames code the same spectrum and the
    // comparison is about the coupling tool alone. Left to the automatic
    // bandwidth they would not agree - narrower than the coupled region at
    // 96 kbit/s, wider at 192 - and this would be measuring bandwidth.
    for (const std::uint32_t kbps : {96u, 128u, 192u}) {
        CAPTURE(kbps);
        const auto plain = steady_state_frame({.bitrate_kbps = kbps, .chbwcod = 48}, 2);
        const auto coupled = steady_state_frame({.bitrate_kbps = kbps, .coupling = true}, 2);
        const int plain_offset = parse_block_zero(plain).snroffst;
        const int coupled_offset = parse_block_zero(coupled).snroffst;
        CAPTURE(plain_offset, coupled_offset);
        CHECK(coupled_offset >= plain_offset);
    }
}

TEST_CASE("a coupling coordinate carries a ratio, not a level", "[encoder][coupling]") {
    // A coordinate is sqrt(E_ch / E_sum) times whatever scale the encoder
    // folded into the coupling channel, and that scale is never transmitted.
    // It therefore has to be one constant for the frame: coordinates go out
    // in blocks 0, 2 and 4 and are reused in 1, 3 and 5, so a scale measured
    // on one block reaches the decoder applied to the NEXT block's
    // coefficients, and the reusing blocks come back wrong by the ratio of
    // the two blocks' scales. A per-band peak - the obvious way to keep
    // coordinates small - is exactly such a scale.
    //
    // Both halves below hold the inter-channel ratios fixed and move only the
    // level, so a coordinate that moves with them is carrying a level.
    const ac3::EncoderConfig config{.bitrate_kbps = 192, .coupling = true};

    SECTION("turning the whole input down leaves them untouched") {
        // -12 dB is an exact power of two, so a level-free scale reproduces
        // the quantized coordinate bit for bit rather than merely closely.
        const auto loud = parse_block_zero(steady_state_frame(config, 2, 1.0));
        const auto quiet = parse_block_zero(steady_state_frame(config, 2, 0.25));
        REQUIRE(loud.cplinu);
        REQUIRE(loud.coords.size() == quiet.coords.size());
        CHECK(loud.master == quiet.master);
        for (std::size_t i = 0; i < loud.coords.size(); ++i) {
            CAPTURE(i, loud.coords[i].exp, loud.coords[i].mant, quiet.coords[i].exp,
                    quiet.coords[i].mant);
            CHECK(loud.coords[i].exp == quiet.coords[i].exp);
            CHECK(loud.coords[i].mant == quiet.coords[i].mant);
        }
    }

    SECTION("a level that swings block to block leaves them untouched too") {
        // The sharper case: an envelope at one cycle per frame, so each
        // block's peak differs from the last. A frame-constant scale ignores
        // it; a per-block one tracks it, and every reusing block inherits the
        // wrong one. The envelope's sidebands land a third of a bin from
        // their tone, so they stay inside their own sub-band and the ratios
        // hold to well under a quantizer step - this compares levels rather
        // than bit patterns only to leave room for that third of a bin.
        const auto steady = parse_block_zero(steady_state_frame(config, 2, 1.0));
        const auto pulsing = parse_block_zero(steady_state_frame(config, 2, 1.0, 0.6));
        REQUIRE(steady.ncplsubnd > 0);
        REQUIRE(steady.coords.size() == pulsing.coords.size());
        const int bands = steady.ncplsubnd;
        for (std::size_t i = 0; i < steady.coords.size(); ++i) {
            const int ch = static_cast<int>(i) / bands;
            const double a = ac3::coupling::decode_coordinate(
                steady.coords[i], steady.master[static_cast<std::size_t>(ch)]);
            const double b = ac3::coupling::decode_coordinate(
                pulsing.coords[i], pulsing.master[static_cast<std::size_t>(ch)]);
            const double db = 20.0 * std::log10(std::max(b, 1e-12) / std::max(a, 1e-12));
            CAPTURE(i, ch, a, b, db);
            CHECK(std::abs(db) < 0.5);  // one quantizer step is 0.26 dB
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
