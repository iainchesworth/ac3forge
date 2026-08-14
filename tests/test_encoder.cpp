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
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/encoder/encoder.hpp"

namespace {

std::vector<float> sine_frame(std::uint64_t& n, double freq, double amplitude) {
    std::vector<float> samples(ac3::kSamplesPerFrame);
    for (auto& s : samples) {
        s = static_cast<float>(amplitude *
                               std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(n) / 48000.0));
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

// The coupling geometry the material below is built for: sub-bands 6..14,
// bins 109..216. The coupling tests pin the encoder to it rather than take
// its bitrate-aware defaults, so that they keep testing the coupling tool
// when those defaults move.
constexpr int kProbeCplBegf = 6;
constexpr int kProbeCplEndf = 12;
constexpr int kProbeCplSubBands = ac3::coupling::sub_band_count(kProbeCplBegf, kProbeCplEndf);
// The bandwidth code whose last mantissa is the last coupled bin, so an
// uncoupled frame can be compared with a coupled one over the same spectrum.
constexpr int kProbeChbwcod = 48;

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
    constexpr double kBinHz = 48000.0 / 512.0;
    std::vector<double> tones = {310.0, 1450.0, 5200.0, 8100.0};  // the baseband's share
    std::vector<double> tilt(tones.size(), 1.0);
    for (int b = 0; b < kProbeCplSubBands; ++b) {
        tones.push_back((ac3::coupling::start_mant(kProbeCplBegf) + 12 * b + 6) * kBinHz);
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
    int cplstrtmant = 0;                            // 0 when not coupling
    int cplendmant = 0;                             // 0 when not coupling
    int chbw_endmant = 0;                           // 0 when coupling
    ac3::coupling::BandLayout bands{};              // as cplbndstrc describes it
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
        out.cplstrtmant = cplstrtmant;
        out.cplendmant = cplendmant;
        out.ncplsubnd = (cplendmant - cplstrtmant) / ac3::coupling::kBinsPerSubBand;
        // cplbndstrc: a set bit joins that sub-band to the band before it, so
        // the coordinate count is the number of CLEAR bits plus one.
        std::array<bool, ac3::coupling::kSubBands> structure{};
        for (int bnd = 1; bnd < out.ncplsubnd; ++bnd) {
            structure[static_cast<std::size_t>(bnd)] = r.read(1) != 0;
        }
        out.bands = ac3::coupling::group_bands(cplbegf, out.ncplsubnd, structure);
        for (int ch = 0; ch < kNfchans; ++ch) {
            REQUIRE(r.read(1) == 1);  // cplcoe: block 0 always sends coordinates
            out.master.push_back(static_cast<int>(r.read(2)));
            for (int bnd = 0; bnd < out.bands.count; ++bnd) {
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
        out.chbw_endmant = endmant[0];
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
        for (const auto& [begf, endf] : {std::pair{6, 12}, std::pair{0, 15}, std::pair{12, 2}}) {
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
    // side, where the same mistake was found, csnroffst went from 27 coarse
    // steps to 11 at 128 kbit/s; here the composite offset goes from 15 steps
    // above the uncoupled frame to 8 below it. The frame still decodes, and
    // still passes every size and CRC check, which is exactly why this needs
    // its own test.
    //
    // Both frames are pinned to the same spectrum - the uncoupled one by
    // chbwcod, the coupled one by the sub-band range that ends on the same
    // bin - so this is about the coupling tool and not about how much
    // bandwidth each one chose to code.
    for (const std::uint32_t kbps : {96u, 128u, 192u}) {
        CAPTURE(kbps);
        const auto plain =
            steady_state_frame({.bitrate_kbps = kbps, .chbwcod = kProbeChbwcod}, 2);
        const auto coupled = steady_state_frame({.bitrate_kbps = kbps,
                                                 .coupling = true,
                                                 .cplbegf = kProbeCplBegf,
                                                 .cplendf = kProbeCplEndf},
                                                2);
        const int plain_offset = parse_block_zero(plain).snroffst;
        const int coupled_offset = parse_block_zero(coupled).snroffst;
        CAPTURE(plain_offset, coupled_offset);
        CHECK(coupled_offset >= plain_offset);
    }
}

TEST_CASE("the coupling band follows the bit rate", "[encoder][coupling]") {
    // Neither end of the coupled region is a constant. The START rises with
    // the per-channel rate, because a channel that can afford its own high
    // band should keep it. The END has to be the bandwidth the frame would
    // have coded anyway: with every fbw channel coupled, chbwcod is not
    // transmitted at all, so cplendf IS the bandwidth, and a fixed one would
    // make coupling a bandwidth decision as well as a cost one. The old fixed
    // pair (6 and 12) coded 20.3 kHz at every rate - 4.5 kHz more than the
    // uncoupled encoder would have kept at 96 kbit/s, paid for out of a frame
    // that could least afford it.
    int previous_start = 0;
    for (const std::uint32_t kbps : {96u, 128u, 192u, 256u, 448u}) {
        CAPTURE(kbps);
        const auto plain = parse_block_zero(steady_state_frame({.bitrate_kbps = kbps}, 2));
        const auto coupled =
            parse_block_zero(steady_state_frame({.bitrate_kbps = kbps, .coupling = true}, 2));
        REQUIRE(coupled.cplinu);
        REQUIRE_FALSE(plain.cplinu);
        CAPTURE(plain.chbw_endmant, coupled.cplstrtmant, coupled.cplendmant);

        // Never wider than the uncoupled bandwidth, and within one sub-band
        // of it - cplendf can only land on a sub-band edge.
        CHECK(coupled.cplendmant <= plain.chbw_endmant);
        CHECK(plain.chbw_endmant - coupled.cplendmant < ac3::coupling::kBinsPerSubBand);
        // Sub-band 4, bin 85, is the floor: below it coupling is trading away
        // more waveform detail than the saving is worth.
        CHECK(coupled.cplstrtmant >= ac3::coupling::start_mant(4));
        CHECK(coupled.cplstrtmant >= previous_start);  // monotone in the rate
        CHECK(coupled.ncplsubnd >= 1);
        previous_start = coupled.cplstrtmant;

        // Every default region reaches above 11 kHz, so cplbndstrc always has
        // something to join: a coordinate per sub-band up there is finer than
        // the ear and costs 8 bits a band, three times a frame per channel.
        CAPTURE(coupled.ncplsubnd, coupled.bands.count);
        CHECK(coupled.bands.count < coupled.ncplsubnd);
        CHECK(coupled.coords.size() ==
              static_cast<std::size_t>(coupled.bands.count) * 2);

        // What the defaults are for: switching coupling on must never leave
        // the frame worse off than not coupling at all. The old fixed pair
        // failed this at 96 kbit/s, where it spent 9 sub-bands of coordinates
        // on 4.5 kHz the uncoupled encoder would have dropped, and the
        // composite offset came out 14 steps DOWN. Coupling codes at most as
        // much spectrum as the plain frame here, so this is a floor, not a
        // like-for-like measurement - "coupling must not cost more bits than
        // the channels it replaces" is the matched one.
        CAPTURE(plain.snroffst, coupled.snroffst);
        CHECK(coupled.snroffst > plain.snroffst);
    }
    // And it moves: a 96 kbit/s frame and a 448 kbit/s one must not couple
    // from the same place, or the rate is not being consulted at all.
    const auto low = parse_block_zero(
        steady_state_frame({.bitrate_kbps = 96, .coupling = true}, 2));
    const auto high = parse_block_zero(
        steady_state_frame({.bitrate_kbps = 448, .coupling = true}, 2));
    CHECK(high.cplstrtmant > low.cplstrtmant);
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
    const ac3::EncoderConfig config{.bitrate_kbps = 192,
                                    .coupling = true,
                                    .cplbegf = kProbeCplBegf,
                                    .cplendf = kProbeCplEndf};

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
        REQUIRE(steady.bands.count > 0);
        REQUIRE(steady.coords.size() == pulsing.coords.size());
        const int bands = steady.bands.count;
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
    // 1+1 needs Ch2's own dialnorm; missing or out of range is exactly as
    // invalid as Ch1's own would be.
    ac3::FrameEncoder missing_dialnorm2{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::kDualMono}};
    CHECK(encode_same(missing_dialnorm2, silence).error() == ac3::FrameError::kInvalidDialnorm);
    ac3::FrameEncoder bad_dialnorm2{
        {.bitrate_kbps = 192, .dialnorm2 = 0, .acmod = ac3::Acmod::kDualMono}};
    CHECK(encode_same(bad_dialnorm2, silence).error() == ac3::FrameError::kInvalidDialnorm);
}

TEST_CASE("dual mono codes two independent programmes, never one into the other",
         "[encoder][dual-mono]") {
    using ac3::Acmod;
    // Ch1 carries a loud, genuinely wideband tone; Ch2 is silent. Any
    // cross-talk between the two - coupling turned on by mistake, a shared
    // downmix measurement, a swapped channel - shows up as either Ch1 losing
    // level or Ch2 picking some of it up, neither of which real dual mono
    // permits (§7.7.2.2: compr bounds Ch1's own signal, compr2 Ch2's, never a
    // mix of the two).
    // heavy2 is set explicitly alongside heavy - compr2e is Ch2's own flag,
    // not inherited from Ch1's, so leaving it unset here would (correctly)
    // silence compr2 and defeat the compr2.has_value() check below.
    const ac3::EncoderConfig config{.bitrate_kbps = 192,
                                    .dialnorm = 27,
                                    .dialnorm2 = 18,
                                    .acmod = Acmod::kDualMono,
                                    .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
                                    .heavy = ac3::meta::HeavyConfig{},
                                    .heavy2 = ac3::meta::HeavyConfig{}};
    ac3::FrameEncoder encoder{config};
    ac3::FrameDecoder decoder;
    std::uint64_t n = 0;
    std::vector<std::byte> last_frame;
    for (int f = 0; f < 3; ++f) {
        const auto ch1 = sine_frame(n, 1200.0, 0.8);
        std::uint64_t n2 = n - static_cast<std::uint64_t>(ac3::kSamplesPerFrame);
        const auto ch2 = sine_frame(n2, 1200.0, 0.0);  // silence, same length
        const std::vector<std::span<const float>> views{ch1, ch2};
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        check_frame_invariants(*frame, config.sample_rate, config.bitrate_kbps);
        last_frame = *frame;
    }

    const auto decoded = decoder.decode_frame(last_frame);
    REQUIRE(decoded.has_value());
    CHECK(decoded->acmod == Acmod::kDualMono);
    CHECK(decoded->dialnorm == 27);
    REQUIRE(decoded->dialnorm2.has_value());
    CHECK(*decoded->dialnorm2 == 18);
    REQUIRE(decoded->compr.has_value());   // Ch1 is loud: heavy compression engaged
    REQUIRE(decoded->compr2.has_value());  // still transmitted - §7.7.2 sends it every frame
                                           // once heavy is configured, whatever the level
    REQUIRE(decoded->channels.size() == 2);

    double ch1_peak = 0.0;
    double ch2_peak = 0.0;
    for (const float s : decoded->channels[0]) {
        ch1_peak = std::max(ch1_peak, std::abs(static_cast<double>(s)));
    }
    for (const float s : decoded->channels[1]) {
        ch2_peak = std::max(ch2_peak, std::abs(static_cast<double>(s)));
    }
    CHECK(ch1_peak > 0.3);    // Ch1's tone survived coding
    CHECK(ch2_peak < 0.02);  // Ch2 stayed silent - nothing leaked in from Ch1
}

namespace {

// Per-sample SNR between two ALREADY-DECODED signals of equal length and
// alignment (unlike test_eac3_decoder.cpp's own snr_db, which compares a
// decoded signal against its own pre-encode source and so needs the MDCT's
// algorithmic delay folded in - here both signals came out of the same
// decoder at the same delay, so no alignment shift is needed).
double decoded_snr_db(const std::vector<float>& a, const std::vector<float>& b) {
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double x = static_cast<double>(a[i]);
        const double d = static_cast<double>(b[i]) - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

}  // namespace

TEST_CASE("fast_mdct changes output only at the quantization-decision level",
         "[encoder][fast-mdct]") {
    // Phase 4's own end-to-end check: encode the SAME real (multi-tone,
    // several frames) audio once with fast_mdct off and once with it on,
    // decode both with the repo's own decoder, and compare. The fast path
    // is bit-exact for mdct512_forward itself (test_mdct_fast.cpp), but the
    // ENCODER makes discrete decisions (bap, exponent strategy, block
    // switching) off of those coefficients - a coefficient that changes by
    // 1e-13 can occasionally land on the other side of a rounding boundary
    // and flip one of those decisions, which is an audible-scale question
    // this test answers empirically rather than assuming away.
    const ac3::EncoderConfig direct_config{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
    ac3::EncoderConfig fast_config = direct_config;
    fast_config.fast_mdct = true;

    ac3::FrameEncoder direct_encoder{direct_config};
    ac3::FrameEncoder fast_encoder{fast_config};
    ac3::FrameDecoder direct_decoder;
    ac3::FrameDecoder fast_decoder;

    std::uint64_t n_left = 0;
    std::uint64_t n_right = 0;
    std::vector<float> direct_pcm;
    std::vector<float> fast_pcm;
    direct_pcm.reserve(static_cast<std::size_t>(ac3::kSamplesPerFrame) * 8);
    fast_pcm.reserve(direct_pcm.capacity());

    for (int f = 0; f < 8; ++f) {
        // Two tones summed, not a single frequency - real material spreads
        // energy (and hence bap/exponent decisions) across many more bins
        // than a pure tone does. Both encoders below see the SAME left/right
        // PCM - only fast_mdct differs between them.
        auto l1 = sine_frame(n_left, 440.0, 0.3);
        auto n_left_r = n_left - static_cast<std::uint64_t>(ac3::kSamplesPerFrame);
        auto l2 = sine_frame(n_left_r, 2500.0, 0.15);
        std::vector<float> left(l1.size());
        for (std::size_t i = 0; i < left.size(); ++i) {
            left[i] = l1[i] + l2[i];
        }
        auto r1 = sine_frame(n_right, 660.0, 0.3);
        auto n_right_r = n_right - static_cast<std::uint64_t>(ac3::kSamplesPerFrame);
        auto r2 = sine_frame(n_right_r, 3100.0, 0.15);
        std::vector<float> right(r1.size());
        for (std::size_t i = 0; i < right.size(); ++i) {
            right[i] = r1[i] + r2[i];
        }

        const std::vector<std::span<const float>> views{left, right};
        const auto direct_frame = direct_encoder.encode_frame(views);
        const auto fast_frame = fast_encoder.encode_frame(views);
        REQUIRE(direct_frame.has_value());
        REQUIRE(fast_frame.has_value());

        const auto direct_decoded = direct_decoder.decode_frame(*direct_frame);
        const auto fast_decoded = fast_decoder.decode_frame(*fast_frame);
        REQUIRE(direct_decoded.has_value());
        REQUIRE(fast_decoded.has_value());
        for (const auto& ch : direct_decoded->channels) {
            direct_pcm.insert(direct_pcm.end(), ch.begin(), ch.end());
        }
        for (const auto& ch : fast_decoded->channels) {
            fast_pcm.insert(fast_pcm.end(), ch.begin(), ch.end());
        }
    }

    const double snr = decoded_snr_db(direct_pcm, fast_pcm);
    CAPTURE(snr);
    // Quantization-decision-level differences, not a structural change: a
    // handful of bap/exponent flips a frame costs far less than 1 dB of SNR
    // against the direct-path decode. 60 dB leaves generous headroom above
    // "clearly the same audio" while still catching a real regression.
    CHECK(snr > 60.0);
}
