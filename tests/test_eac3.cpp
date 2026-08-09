#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/eac3_tools.hpp"

namespace {

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

}  // namespace

TEST_CASE("E-AC-3 frame size follows the bit rate exactly", "[eac3]") {
    // frmsiz signals the size directly, so there is no table and no 44.1 kHz
    // padding alternation: the frame is simply the exact bit budget in words.
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 192) == 384);
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k48000, 640) == 1280);
    STATIC_CHECK(ac3::eac3::frame_words(ac3::SampleRate::k32000, 192) == 576);
}

TEST_CASE("E-AC-3 header fields", "[eac3]") {
    const auto frame = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    const std::span<const std::byte> bytes{*frame};
    CHECK(frame->size() == 768);

    // Sync word, then bsi begins immediately - E-AC-3 has no crc1.
    CHECK(u8(bytes, 0) == 0x0B);
    CHECK(u8(bytes, 1) == 0x77);

    // byte 2: strmtyp(2)=0 | substreamid(3)=0 | frmsiz(11) high 3 bits.
    // frmsiz = words - 1 = 383 = 0b00101111111, so the top 3 bits are 001.
    CHECK(u8(bytes, 2) == 0x01);
    // byte 3: the remaining 8 bits of frmsiz = 0b01111111.
    CHECK(u8(bytes, 3) == 0x7F);
    // byte 4: fscod(2)=0 | numblkscod(2)=3 | acmod(3)=2 | lfeon(1)=0
    //         = 00 11 010 0
    CHECK(u8(bytes, 4) == 0x34);
    // byte 5: bsid(5)=16 | dialnorm high 3 bits (31 = 11111) = 10000 111
    CHECK(u8(bytes, 5) == 0x87);
}

TEST_CASE("E-AC-3 crc2 covers the frame after the sync word", "[eac3]") {
    for (const std::uint32_t kbps : {96u, 192u, 448u, 640u}) {
        CAPTURE(kbps);
        const auto frame = ac3::eac3::build_silent_frame({.bitrate_kbps = kbps});
        REQUIRE(frame.has_value());
        const std::span<const std::byte> bytes{*frame};
        // There is no crc1 in E-AC-3, so the only check is the trailing one:
        // the register must read zero over everything past the sync word.
        CHECK(ac3::crc16(bytes.subspan(2)) == 0x0000);
    }
}

TEST_CASE("E-AC-3 layouts and rates produce correctly sized frames", "[eac3]") {
    using ac3::Acmod;
    for (const auto acmod : {Acmod::k1_0, Acmod::k2_0, Acmod::k3_2}) {
        for (const bool lfe : {false, true}) {
            for (const std::uint32_t kbps : {96u, 192u, 448u}) {
                CAPTURE(static_cast<int>(acmod), lfe, kbps);
                const auto frame = ac3::eac3::build_silent_frame(
                    {.bitrate_kbps = kbps, .acmod = acmod, .lfe = lfe});
                if (!frame) {
                    // Legitimate: a wide layout at a low rate cannot fit even
                    // its own exponent sets. The contract is a clean refusal,
                    // never an undersized frame.
                    CHECK(frame.error() == ac3::FrameError::kInvalidBitrate);
                    continue;
                }
                CHECK(frame->size() ==
                      ac3::eac3::frame_words(ac3::SampleRate::k48000, kbps) * 2);
                CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
            }
        }
    }
}

namespace {

// The frame SNR offsets, read straight out of a stereo frame: sync(16) +
// bsi(38) + the eleven audfrm flags(12) + cplinu/cplstre(6, acmod > 1) +
// frmcplexpstr(5, only when some block couples) + frmchexpstr(2x5) +
// convexpstr(2x5) lands exactly on frmcsnroffst.
int frame_snr_offset(std::span<const std::byte> frame, bool coupled = false) {
    ac3::BitReader reader{frame};
    reader.skip(16 + 38 + 12 + 6 + (coupled ? 5 : 0) + 10 + 10);
    const auto csnroffst = reader.read(6);
    const auto fsnroffst = reader.read(4);
    return static_cast<int>((csnroffst << 4) | fsnroffst);
}

// A frame of 1 kHz at half scale in both channels.
std::vector<std::vector<float>> tone_frame(int channels, std::uint64_t start) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (auto& channel : pcm) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            channel[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0));
        }
    }
    return pcm;
}

// A genuinely QUIET frame - low overall level, not just spectrally simple.
// bit-allocation cost tracks signal level more than spectral shape, so
// pairing this with wideband_frame (below) is what actually gives VBR's
// rate control something reliably cheaper to react to; tone_frame's own 0.5
// amplitude is loud enough to cost about as much as wideband_frame despite
// being a single tone.
std::vector<std::vector<float>> quiet_frame(int channels, std::uint64_t start) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (auto& channel : pcm) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            channel[static_cast<std::size_t>(i)] =
                static_cast<float>(0.02 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0));
        }
    }
    return pcm;
}

// Program-like material with energy right across the spectrum and a DIFFERENT
// balance per channel. A pure tone would make coupling a no-op - there is
// nothing above the coupling frequency to share - and identical channels
// would make every coupling coordinate the same, so neither could tell a
// working coupling implementation from a broken one.
std::vector<std::vector<float>> wideband_frame(int channels, std::uint64_t start) {
    constexpr std::array<double, 5> kTones = {310.0, 1450.0, 5200.0, 9700.0, 15100.0};
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            double value = 0.0;
            for (std::size_t t = 0; t < kTones.size(); ++t) {
                // Each channel weights the tones differently, so the coupling
                // coordinates genuinely differ band to band and channel to
                // channel.
                const double gain = 0.18 / (1.0 + static_cast<double>((t + ch) % 4));
                value += gain * std::sin(2.0 * std::numbers::pi * kTones[t] *
                                         (1.0 + 0.01 * static_cast<double>(ch)) * n / 48000.0);
            }
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(value);
        }
    }
    return pcm;
}

// Encode `count` frames and return the last one, so the MDCT history is real
// rather than the half-empty window the first frame sees.
std::vector<std::byte> steady_state_frame(const ac3::eac3::FrameConfig& config, int channels,
                                          int count = 3) {
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::byte> last;
    std::uint64_t n = 0;
    for (int f = 0; f < count; ++f) {
        auto pcm = wideband_frame(channels, n);
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

// One FrameEncoder, fed a caller-chosen sequence of busy (wideband_frame) or
// quiet (tone_frame) content, returning every produced frame's byte size.
// tone_frame concentrates its energy in one band, so most of the spectrum
// sits near the masking floor and the allocator spends little on it;
// wideband_frame spreads real energy across five widely separated bands with
// a different balance per channel, which costs far more at the same
// quality. The two shapes exist so VBR has something to visibly react to.
std::vector<std::size_t> vbr_frame_sizes(const ac3::eac3::FrameConfig& config,
                                         std::span<const bool> busy_per_frame) {
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::size_t> sizes;
    std::uint64_t n = 0;
    for (const bool busy : busy_per_frame) {
        auto pcm = busy ? wideband_frame(2, n) : quiet_frame(2, n);
        n += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
        sizes.push_back(frame->size());
    }
    return sizes;
}

}  // namespace

TEST_CASE("E-AC-3 bamode 0 takes the Annex E allocation defaults", "[eac3]") {
    // Table E1.4's else-branch fixes floorcod at 0x7. The §8.2.12 basic
    // -encoder recommendation - what BitAllocCodes defaults to, and what the
    // AC-3 encoder rightly uses - is 4. floorcod sets the masking floor, so
    // the wrong one makes the encoder believe almost nothing costs bits: the
    // SNR search then saturates at the maximum offset and sizes the frame for
    // an allocation the decoder will not reproduce, leaving every block after
    // the first at the wrong bit offset.
    //
    // Silence cannot catch this. Zero SNR offsets trip §7.2.2.1.1, which
    // zeroes the allocation before floorcod is ever consulted, so the frame
    // decodes perfectly either way. Only real audio exercises it.
    //
    // Nor can the FIRST frame. Its analysis window is half MDCT history that
    // does not exist yet, so it is a fade-in rather than the steady-state
    // signal, and it happens not to saturate even with the wrong floorcod.
    // The frame under test has to be one the encoder reaches in flight.
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    std::uint64_t n = 0;
    for (int f = 0; f < 3; ++f) {
        auto pcm = tone_frame(2, n);
        n += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        CHECK(frame->size() == 768);
        CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
        if (f == 0) {
            continue;  // the fade-in frame proves nothing
        }
        // Saturating at 63/15 is the signature of an allocator that believes
        // everything is free: the search ran out of offsets before it ran out
        // of budget, so the frame is sized for an allocation no decoder will
        // reproduce.
        CHECK(frame_snr_offset(*frame) < 1023);
    }
}

TEST_CASE("E-AC-3 real audio fills the frame it claims", "[eac3]") {
    // The same check across the rate range, plus 5.1, since the allocation
    // scales with both the budget and the channel count.
    for (const int kbps : {192, 384, 640}) {
        ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = static_cast<std::uint32_t>(kbps)}};
        std::uint64_t n = 0;
        for (int f = 0; f < 3; ++f) {
            auto pcm = tone_frame(2, n);
            n += ac3::kSamplesPerFrame;
            const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
            const auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            CHECK(frame->size() ==
                  ac3::eac3::frame_words(ac3::SampleRate::k48000,
                                         static_cast<std::uint32_t>(kbps)) *
                      2);
            CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
            if (f > 0) {
                CHECK(frame_snr_offset(*frame) < 1023);
            }
        }
    }
}

TEST_CASE("coupling sub-bands group into bands", "[eac3][coupling]") {
    using ac3::eac3::group_bands;
    // Nothing merged: one band per sub-band, each 12 bins wide.
    const std::array<bool, 18> none{};
    const auto flat = group_bands(37, 18, 12, std::span{none});
    CHECK(flat.count == 18);
    CHECK(flat.start[0] == 37);
    CHECK(flat.start[17] == 37 + 17 * 12);
    CHECK(flat.size[17] == 12);

    // Table E2.12's default: eight ones among the eighteen entries, and entry
    // 0 is never consulted because the first sub-band always opens a band.
    const auto& def = ac3::eac3::kDefaultCplBandStructure;
    const auto merged = group_bands(37, 18, 12, std::span{def});
    const auto ones = static_cast<int>(std::count(def.begin() + 1, def.end(), true));
    CHECK(merged.count == 18 - ones);
    // §5.4.3.13's own formula for ncplbnd, independently.
    CHECK(merged.count == 10);
    // Bands tile the region exactly, with no gaps and no overlap.
    int bin = 37;
    for (int b = 0; b < merged.count; ++b) {
        CHECK(merged.start[static_cast<std::size_t>(b)] == bin);
        bin += merged.size[static_cast<std::size_t>(b)];
    }
    CHECK(bin == 37 + 18 * 12);
}

TEST_CASE("E-AC-3 coupling places its fields where Annex E puts them",
          "[eac3][coupling]") {
    // Walking the frame back out is the only way to pin field PLACEMENT: a
    // frame whose fields are one bit adrift still has the right size and a
    // valid crc2, and still "decodes" - into noise.
    const auto frame = steady_state_frame(
        {.bitrate_kbps = 192, .coupling = true, .cplbegf = 4}, 2);
    ac3::BitReader reader{frame};
    reader.skip(16 + 38);  // syncword + bsi (stereo, independent, no chanmap)
    reader.skip(12);       // expstre..spxattene (snroffststr is 2 bits)
    CHECK(reader.read(1) == 1);  // cplinu[0]; cplstre[0] is implied 1
    for (int blk = 1; blk < ac3::kBlocksPerFrame; ++blk) {
        CHECK(reader.read(1) == 0);  // cplstre[blk]: strategy set once a frame
    }
    // frmcplexpstr precedes the per-channel codes and only exists because
    // some block couples. Omitting it would shift the SNR offsets by 5 bits.
    CHECK(reader.read(5) == 0);  // frmcplexpstr: Table E2.10 row 0
    CHECK(reader.read(5) == 0);  // frmchexpstr[0]
    CHECK(reader.read(5) == 0);  // frmchexpstr[1]
    reader.skip(10);             // convexpstr[0..1]
    reader.skip(10);             // frmcsnroffst + frmfsnroffst
    CHECK(reader.read(1) == 0);  // blkstrtinfoe

    // audblk 0.
    reader.skip(2);              // dithflag[0..1]
    CHECK(reader.read(1) == 0);  // dynrnge
    CHECK(reader.read(1) == 0);  // spxinu (spxstre implied in block 0)
    CHECK(reader.read(1) == 0);  // ecplinu: standard coupling
    // chincpl is NOT transmitted in 2/0 - both channels couple by definition -
    // so phsflginu comes next.
    CHECK(reader.read(1) == 0);  // phsflginu
    CHECK(reader.read(4) == 4);  // cplbegf
    CHECK(reader.read(4) == 15); // cplendf: coupling runs to the top
    CHECK(reader.read(1) == 1);  // cplbndstrce: structure sent, not defaulted
    const int nsubnd = 3 + 15 - 4;
    int bands = 1;
    for (int sbnd = 1; sbnd < nsubnd; ++sbnd) {
        bands += reader.read(1) == 0 ? 1 : 0;
    }
    // Block 0's cplcoe is implied by firstcplcos, so coordinates follow with
    // no flag of their own - a bit AC-3 spends here and Annex E does not.
    for (int ch = 0; ch < 2; ++ch) {
        reader.skip(2);  // mstrcplco
        for (int bnd = 0; bnd < bands; ++bnd) {
            reader.skip(8);  // cplcoexp + cplcomant
        }
    }
    // cplbegf 4 is above 2, so all four rematrixing bands survive (§7.5.2.2).
    for (int bnd = 0; bnd < 4; ++bnd) {
        CHECK(reader.read(1) == 0);  // rematflg
    }
    // chbwcod is absent for a coupled channel, so the coupling channel's
    // exponents come next: cplabsexp then ncplgrps groups of 7.
    reader.skip(4 + (15 - 4 + 3) * 12 / 3 * 7);
    // Landing exactly on the first fbw channel's exponents is the assertion:
    // its absolute exponent is 4 bits and cannot exceed 15.
    CHECK(reader.read(4) <= 15);
}

TEST_CASE("coupling must not cost more bits than the channels it replaces",
          "[eac3][coupling]") {
    // Coupling replaces two channels' high bands with one shared channel, so
    // the frame should be able to afford a HIGHER SNR offset, not a lower one.
    //
    // The way to get this backwards is to scale the shared channel up - to
    // normalise it per band, or to the region peak - which makes the
    // coordinates comfortably small but hands the allocator a channel that
    // reads as full scale. psd is absolute, so the allocator then buys the
    // coupling channel more bits per bin than the baseband it was meant to be
    // subsidising, and the offset collapses: measured at 128 kbit/s, from 27
    // coarse steps to 11. The frame still decodes, and still passes every
    // size and CRC check, which is exactly why this needs its own test.
    for (const std::uint32_t kbps : {128u, 192u}) {
        CAPTURE(kbps);
        const auto plain = steady_state_frame({.bitrate_kbps = kbps}, 2);
        const auto coupled =
            steady_state_frame({.bitrate_kbps = kbps, .coupling = true}, 2);
        const int plain_offset = frame_snr_offset(plain);
        const int coupled_offset = frame_snr_offset(coupled, true);
        CAPTURE(plain_offset, coupled_offset);
        CHECK(coupled_offset >= plain_offset);
    }
}

TEST_CASE("E-AC-3 coupling fills the frame it claims", "[eac3][coupling]") {
    using ac3::Acmod;
    for (const auto acmod : {Acmod::k2_0, Acmod::k3_2}) {
        for (const std::uint32_t kbps : {128u, 192u, 448u}) {
            const int channels = ac3::fullbw_channel_count(acmod) + 1;
            CAPTURE(static_cast<int>(acmod), kbps);
            const auto frame = steady_state_frame(
                {.bitrate_kbps = kbps, .acmod = acmod, .lfe = true, .coupling = true},
                channels);
            CHECK(frame.size() == ac3::eac3::frame_words(ac3::SampleRate::k48000, kbps) * 2);
            CHECK(ac3::crc16(std::span{frame}.subspan(2)) == 0x0000);
            CHECK(frame_snr_offset(frame, true) < 1023);
        }
    }
}

TEST_CASE("coupling is refused where Annex E has no syntax for it",
          "[eac3][coupling]") {
    // §E2.2.3 gates the whole coupling element on acmod > 0x1, so a mono
    // substream has nowhere to put cplinu. Asking for it must produce a frame
    // WITHOUT coupling rather than a frame with a field the syntax cannot
    // express - which a decoder would read as part of the next element.
    const auto frame = steady_state_frame(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k1_0, .coupling = true}, 1);
    CHECK(frame.size() == ac3::eac3::frame_words(ac3::SampleRate::k48000, 192) * 2);
    CHECK(ac3::crc16(std::span{frame}.subspan(2)) == 0x0000);
    ac3::BitReader reader{frame};
    // Mono bsi is 38 bits as for stereo; the audfrm flags follow, and with
    // acmod 0x1 the next field is frmchexpstr, NOT cplinu.
    reader.skip(16 + 38 + 12);
    CHECK(reader.read(5) == 0);  // frmchexpstr[0]
    reader.skip(5);              // convexpstr[0]
    reader.skip(10);             // frmcsnroffst + frmfsnroffst
    CHECK(reader.read(1) == 0);  // blkstrtinfoe
}

TEST_CASE("the AHT transform pair round-trips", "[eac3][aht]") {
    using ac3::eac3::aht_forward;
    using ac3::eac3::aht_inverse;
    // The forward direction is not in the standard - only the decoder's
    // inverse is - so it is derived, and a derivation that is self-consistent
    // but wrong is exactly the failure mode to guard against. Both a
    // stationary bin and an alternating one, since the interesting weight
    // (R_0) only shows up when the six blocks have a non-zero mean.
    for (const auto& blocks : {std::array<double, 6>{0.6, 0.61, 0.59, 0.6, 0.62, 0.58},
                               std::array<double, 6>{0.5, -0.5, 0.5, -0.5, 0.5, -0.5},
                               std::array<double, 6>{0.9, 0.0, 0.0, 0.0, 0.0, 0.0}}) {
        std::array<double, 6> coefficients{};
        std::array<double, 6> back{};
        aht_forward(blocks, coefficients);
        aht_inverse(coefficients, back);
        for (std::size_t m = 0; m < 6; ++m) {
            CAPTURE(m, blocks[m], back[m]);
            CHECK(std::abs(back[m] - blocks[m]) < 1e-12);
        }
        // Nothing may leave the range a mantissa can hold, whatever the six
        // blocks do inside it.
        for (const double value : coefficients) {
            CHECK(std::abs(value) < 1.0);
        }
    }
    // A constant bin is the whole point: six equal coefficients have to
    // collapse into the first one and nothing else.
    const std::array<double, 6> flat{0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    std::array<double, 6> concentrated{};
    aht_forward(flat, concentrated);
    CHECK(std::abs(concentrated[0] - 0.5) < 1e-12);
    for (std::size_t j = 1; j < 6; ++j) {
        CAPTURE(j);
        CHECK(std::abs(concentrated[j]) < 1e-12);
    }
}

TEST_CASE("the AHT synthesis basis is orthogonal with equal norms",
          "[eac3][aht]") {
    using ac3::eac3::aht_inverse;
    // This is the test that has to exist, because round-tripping cannot catch
    // the mistake worth catching. §E3.4.5's weights are printed with radical
    // signs that a text extraction of the PDF drops, and reading them as 2
    // and 1/2 instead of sqrt(2) and 1/sqrt(2) still gives a valid, exactly
    // invertible transform - it just weights j = 0 differently from the rest,
    // so an AHT channel comes back 3 dB quiet in every coefficient except the
    // ones whose phase repeats block to block.
    //
    // What separates the two readings is a property, not a round trip: the
    // basis columns all have the same norm. Under the misreading j = 0 has
    // norm-squared 6 and the others 12.
    std::array<std::array<double, 6>, 6> basis{};
    for (std::size_t j = 0; j < 6; ++j) {
        std::array<double, 6> unit{};
        unit[j] = 1.0;
        aht_inverse(unit, basis[j]);
    }
    for (std::size_t j = 0; j < 6; ++j) {
        double norm = 0.0;
        for (const double value : basis[j]) {
            norm += value * value;
        }
        CAPTURE(j, norm);
        CHECK(std::abs(norm - 6.0) < 1e-9);
        for (std::size_t k = j + 1; k < 6; ++k) {
            double dot = 0.0;
            for (std::size_t m = 0; m < 6; ++m) {
                dot += basis[j][m] * basis[k][m];
            }
            CAPTURE(k, dot);
            CHECK(std::abs(dot) < 1e-9);
        }
    }
}

TEST_CASE("the GAQ quantizers match Table E3.5's shape", "[eac3][aht][gaq]") {
    using ac3::eac3::aht_mantissa_bits;
    using ac3::eac3::aht_quantize_mantissa;
    for (int hebap = 8; hebap <= 19; ++hebap) {
        const int m = aht_mantissa_bits(hebap);
        for (const int gain : {1, 2, 4}) {
            CAPTURE(hebap, m, gain);
            const int small_bits = gain == 1 ? m : (gain == 2 ? m - 1 : m - 2);
            const int large_bits = gain == 1 ? 0 : (gain == 2 ? m - 1 : m);
            const std::uint32_t tag =
                std::uint32_t{1} << static_cast<unsigned>(small_bits - 1);
            // Table E3.5's codeword lengths, and the reconstruction never
            // straying further than half a step of whichever quantizer it
            // landed in.
            const double large_step = gain == 2 ? 1.0 / ((1 << (m - 1)) - 1)
                                                : 3.0 / ((1 << (m + 1)) - 2);
            const double step = gain == 1 ? 2.0 / ((1 << m) - 1)
                                          : std::max(1.0 / (1 << (m - 1)), large_step);
            for (int i = -200; i <= 200; ++i) {
                const double value = i / 201.0;
                const auto code = aht_quantize_mantissa(value, m, gain);
                CHECK(code.bits == small_bits);
                CHECK(code.escape_bits == (code.escape_bits > 0 ? large_bits : 0));
                // A small codeword may never collide with the escape tag, or
                // the decoder reads a value as an escape and every bit after
                // it in the channel is misaligned.
                if (gain != 1 && code.escape_bits == 0) {
                    CHECK(code.code != tag);
                }
                if (code.escape_bits > 0) {
                    CHECK(code.code == tag);
                }
                CAPTURE(value, code.recon);
                CHECK(std::abs(code.recon - value) <= step);
                CHECK(std::abs(code.recon) < 1.0);
            }
        }
    }
}

TEST_CASE("GAQ reconstruction agrees with Table E3.6", "[eac3][aht][gaq]") {
    using ac3::eac3::aht_quantize_mantissa;
    // The encoder derives its quantizers rather than transcribing the
    // standard's remapping constants, so the derivation has to be anchored to
    // them somewhere. tools/gen_aht_tables.py checks all 120; these are the
    // two rows that pin the shape - the dead zone's edge and its step - at
    // the narrowest quantizer, where the constants are least forgiving.
    //
    // hebap 8, m = 3. Gk = 2 large: a = 0xd555, b = 0x4000 -> y = (2/3)x + 1/2,
    // so the four points are +-{1/2, 5/6}. Gk = 4 large: a = 0xedb7,
    // b = 0x2000 -> y = (6/7)x + 1/4, points +-{1/4, ...} stepping by 3/14.
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    const auto two = aht_quantize_mantissa(0.9, 3, 2);
    CHECK(two.escape_bits == 2);
    CHECK(near(two.recon, 0.5 + 1.0 / 3.0));
    CHECK(near(aht_quantize_mantissa(0.5, 3, 2).recon, 0.5));
    CHECK(near(aht_quantize_mantissa(-0.5, 3, 2).recon, -0.5));
    CHECK(near(aht_quantize_mantissa(0.26, 3, 4).recon, 0.25));
    CHECK(near(aht_quantize_mantissa(0.9, 3, 4).recon, 0.25 + 3.0 * (3.0 / 14.0)));
    // Unity gain is AC-3's symmetric quantizer: 2^m - 1 levels of 2/(2^m - 1).
    CHECK(near(aht_quantize_mantissa(1.0, 3, 1).recon, 6.0 / 7.0));
    CHECK(near(aht_quantize_mantissa(0.3, 3, 1).recon, 2.0 / 7.0));
}

TEST_CASE("GAQ bit accounting matches what it emits", "[eac3][aht][gaq]") {
    using ac3::eac3::aht_bin_gaq_bits;
    using ac3::eac3::aht_mantissa_bits;
    using ac3::eac3::aht_quantize_mantissa;
    // The rate search sizes the frame from aht_bin_gaq_bits and the packer
    // emits from aht_quantize_mantissa. If those two ever disagree the frame
    // is the wrong size, so they are checked against each other directly.
    const std::array<std::array<double, 6>, 4> cases{{
        {0.9, 0.1, -0.05, 0.02, -0.3, 0.6},   // one large, mostly small
        {0.02, -0.01, 0.03, 0.0, -0.02, 0.01},  // all small
        {0.9, -0.8, 0.7, -0.95, 0.85, -0.75},   // all large
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    }};
    for (int hebap = 8; hebap <= 19; ++hebap) {
        const int m = aht_mantissa_bits(hebap);
        for (const int gain : {1, 2, 4}) {
            for (const auto& values : cases) {
                CAPTURE(hebap, gain);
                int emitted = 0;
                for (const double value : values) {
                    const auto code = aht_quantize_mantissa(value, m, gain);
                    emitted += code.bits + code.escape_bits;
                }
                CHECK(emitted == aht_bin_gaq_bits(values, m, gain));
            }
        }
    }
}

TEST_CASE("GAQ gain words are counted the way they are packed",
          "[eac3][aht][gaq]") {
    using ac3::eac3::aht_gaq_sections;
    // Modes 1 and 2 send a bit each; mode 3 packs three to a 5-bit word, so a
    // short final triplet still costs a whole one. Counting that wrong is a
    // frame-sizing error, not a rounding one.
    CHECK(aht_gaq_sections(7, 0) == 0);
    CHECK(aht_gaq_sections(7, 1) == 7);
    CHECK(aht_gaq_sections(7, 2) == 7);
    CHECK(aht_gaq_sections(7, 3) == 3);   // 3 + 3 + 1 padded
    CHECK(aht_gaq_sections(6, 3) == 2);
    CHECK(aht_gaq_sections(0, 3) == 0);
    // Table E3.4's three-state mapping, and that a triplet fits five bits.
    CHECK(ac3::eac3::aht_gaq_mapped(1) == 0);
    CHECK(ac3::eac3::aht_gaq_mapped(2) == 1);
    CHECK(ac3::eac3::aht_gaq_mapped(4) == 2);
    CHECK(2 * 9 + 2 * 3 + 2 < 32);
    // §E3.4.2: gain words stop at hebap 12 for mode 1 and 17 for modes 2 and 3.
    CHECK(ac3::eac3::aht_gaq_has_gain(11, 1));
    CHECK_FALSE(ac3::eac3::aht_gaq_has_gain(12, 1));
    CHECK(ac3::eac3::aht_gaq_has_gain(16, 3));
    CHECK_FALSE(ac3::eac3::aht_gaq_has_gain(17, 3));
    CHECK_FALSE(ac3::eac3::aht_gaq_has_gain(7, 3));  // that is the VQ range
}

TEST_CASE("AHT hands whole frames to block 0 and fills the frame", "[eac3][aht]") {
    using ac3::Acmod;
    // 5.1 below 192 kbit/s cannot fit its own exponent sets, whatever the
    // tools do, so it is not a case about AHT.
    for (const auto& [acmod, rates] :
         std::array<std::pair<Acmod, std::array<std::uint32_t, 3>>, 2>{
             {{Acmod::k2_0, {96u, 192u, 448u}}, {Acmod::k3_2, {192u, 384u, 448u}}}}) {
        for (const std::uint32_t kbps : rates) {
            const int channels = ac3::fullbw_channel_count(acmod) + 1;
            CAPTURE(static_cast<int>(acmod), kbps);
            const auto frame = steady_state_frame(
                {.bitrate_kbps = kbps, .acmod = acmod, .lfe = true, .aht = true},
                channels);
            CHECK(frame.size() == ac3::eac3::frame_words(ac3::SampleRate::k48000, kbps) * 2);
            CHECK(ac3::crc16(std::span{frame}.subspan(2)) == 0x0000);
            // ahte sits second in audfrm, right after expstre. The wideband
            // test material is stationary, so it must actually be on - a
            // frame that quietly declined the transform would pass every
            // other check here while testing nothing.
            ac3::BitReader reader{frame};
            reader.skip(16 + 38 + 1);
            CHECK(reader.read(1) == 1);  // ahte
        }
    }
}

TEST_CASE("all three E-AC-3 tools stack", "[eac3][aht][spx][coupling]") {
    using ac3::Acmod;
    for (const auto acmod : {Acmod::k2_0, Acmod::k3_2}) {
        for (const std::uint32_t kbps : {128u, 448u}) {
            const int channels = ac3::fullbw_channel_count(acmod) + 1;
            CAPTURE(static_cast<int>(acmod), kbps);
            const auto frame = steady_state_frame({.bitrate_kbps = kbps,
                                                   .acmod = acmod,
                                                   .lfe = true,
                                                   .coupling = true,
                                                   .spx = true,
                                                   .aht = true},
                                                  channels);
            CHECK(frame.size() == ac3::eac3::frame_words(ac3::SampleRate::k48000, kbps) * 2);
            CHECK(ac3::crc16(std::span{frame}.subspan(2)) == 0x0000);
        }
    }
}

TEST_CASE("coupling and spectral extension partition the spectrum", "[eac3][spx]") {
    using namespace ac3::eac3;
    // The failure this guards against does not look like a failure. If the
    // coupling region and the extension region disagree about where one ends
    // and the other begins, every field still lands where the decoder expects
    // and the frame parses perfectly - it just reconstructs one band twice,
    // or leaves a hole. §E3.3.1 exists precisely to make the two agree, by
    // deriving cplendf from spxbegf instead of transmitting it, and the whole
    // point is this identity.
    for (int spxbegf = 0; spxbegf <= 7; ++spxbegf) {
        CAPTURE(spxbegf);
        const int synthesis_starts = spx_band_start(spx_begin_subbnd(spxbegf));
        const int coupling_ends =
            kCplFirstBin + kCplBinsPerSubBand * (derived_cplendf(spxbegf) + 3);
        CHECK(coupling_ends == synthesis_starts);
    }
    // Both codes are non-linear at the top, which is the reason to check the
    // ends rather than trust an offset.
    CHECK(spx_begin_subbnd(5) == 7);
    CHECK(spx_begin_subbnd(6) == 9);   // not 8: the step doubles here
    CHECK(spx_end_subbnd(2) == 7);
    CHECK(spx_end_subbnd(3) == 9);     // likewise
    CHECK(spx_band_start(0) == 25);
    CHECK(spx_band_start(17) == 229);  // one past the last synthesized band
}

TEST_CASE("the SPX attenuation taps match Table E3.14", "[eac3][spx]") {
    using ac3::eac3::spx_attenuation;
    const auto near = [](double a, double b) { return std::abs(a - b) < 5e-9; };
    // Table E3.14 is 2^(-(cod + 1)(index + 1)/15) throughout, so it is derived
    // rather than transcribed. These are rows the standard happens to print as
    // exact binary fractions, which is where a wrong exponent would show up
    // first, plus the top-left and bottom-right corners.
    CHECK(near(spx_attenuation(0, 0), 0.954841604));
    CHECK(near(spx_attenuation(0, 2), 0.870550563));
    CHECK(near(spx_attenuation(4, 2), 0.5));       // (5 * 3)/15 == 1
    CHECK(near(spx_attenuation(14, 0), 0.5));      // (15 * 1)/15 == 1
    CHECK(near(spx_attenuation(9, 2), 0.25));
    CHECK(near(spx_attenuation(29, 0), 0.25));
    CHECK(near(spx_attenuation(31, 2), 0.011841536));
    // Five taps, symmetric about the middle one, deepest on the join.
    for (const int cod : {0, 7, 31}) {
        CAPTURE(cod);
        CHECK(near(spx_attenuation(cod, 3), spx_attenuation(cod, 1)));
        CHECK(near(spx_attenuation(cod, 4), spx_attenuation(cod, 0)));
        CHECK(spx_attenuation(cod, 2) < spx_attenuation(cod, 1));
        CHECK(spx_attenuation(cod, 1) < spx_attenuation(cod, 0));
        CHECK(spx_attenuation(cod, 0) < 1.0);
    }
}

TEST_CASE("the SPX notch lands on every seam and nowhere else", "[eac3][spx]") {
    using ac3::eac3::group_bands;
    using ac3::eac3::spx_apply_notch;
    using ac3::eac3::spx_attenuation;
    // Placement is the part a decoder cannot tell you about. The decoder
    // derives the notch position itself, so an encoder that filters the wrong
    // bins still produces a stream whose decoded spectrum has the dip in the
    // right place - it just compensates the band gains for bins it never
    // attenuated. Only checking the filter directly catches that.
    constexpr int kStart = 97;
    const std::array<bool, 17> flat{};
    const auto bands = group_bands(kStart, 6, 12, std::span{flat});
    std::array<bool, ac3::eac3::kMaxSubBands> wrapflag{};
    wrapflag[2] = true;  // the copy wrapped into band 2

    std::vector<double> synth(static_cast<std::size_t>(6 * 12), 1.0);
    spx_apply_notch(synth, kStart, bands, std::span{wrapflag}, 7);

    // Every bin the notch did not touch is untouched, exactly.
    const auto expected = [&](std::size_t index) {
        const auto bin = static_cast<int>(index) + kStart;
        for (const int centre : {kStart, bands.start[2]}) {
            const int tap = bin - (centre - 2);
            if (tap >= 0 && tap < ac3::eac3::kSpxAttenTaps) {
                return spx_attenuation(7, tap);
            }
        }
        return 1.0;
    };
    int touched = 0;
    for (std::size_t i = 0; i < synth.size(); ++i) {
        CAPTURE(i, static_cast<int>(i) + kStart);
        CHECK(std::abs(synth[i] - expected(i)) < 1e-12);
        touched += synth[i] != 1.0 ? 1 : 0;
    }
    // Five taps at the wrap, three at the border - the other two fall below
    // startmant, onto coded bins this buffer does not cover.
    CHECK(touched == 8);
    // Deepest exactly on the first synthesized coefficient, and on the first
    // coefficient of the band that wrapped.
    CHECK(synth[0] == spx_attenuation(7, 2));
    CHECK(synth[static_cast<std::size_t>(bands.start[2] - kStart)] ==
          spx_attenuation(7, 2));
    // A band that did not wrap has no seam and must be left alone.
    CHECK(synth[static_cast<std::size_t>(bands.start[4] - kStart)] == 1.0);

    // Switched off, nothing moves at all.
    std::vector<double> untouched(static_cast<std::size_t>(6 * 12), 1.0);
    spx_apply_notch(untouched, kStart, bands, std::span{wrapflag}, -1);
    CHECK(std::ranges::all_of(untouched, [](double v) { return v == 1.0; }));
}

TEST_CASE("E-AC-3 spectral extension places its fields where Annex E puts them",
          "[eac3][spx]") {
    const auto frame = steady_state_frame(
        {.bitrate_kbps = 192, .spx = true, .spxbegf = 4, .spxattencod = 9}, 2);
    ac3::BitReader reader{frame};
    reader.skip(16 + 38 + 11);   // bsi, then expstre..skipflde
    CHECK(reader.read(1) == 1);  // spxattene, the last of the audfrm flags
    CHECK(reader.read(1) == 0);  // cplinu[0]: coupling off
    for (int blk = 1; blk < ac3::kBlocksPerFrame; ++blk) {
        CHECK(reader.read(1) == 0);  // cplstre[blk]
    }
    // No block couples, so frmcplexpstr is absent and frmchexpstr comes next.
    CHECK(reader.read(5) == 0);
    CHECK(reader.read(5) == 0);
    reader.skip(10 + 10);        // convexpstr, then the frame SNR offsets
    // The attenuation codes are per channel and frame-constant, so they live
    // in audfrm rather than the blocks - between the SNR offsets and
    // blkstrtinfoe. Worth pinning: getting it wrong shifts every audio block
    // by twelve bits, which a decoder reads as a different tool entirely.
    for (int ch = 0; ch < 2; ++ch) {
        CHECK(reader.read(1) == 1);  // chinspxatten[ch]
        CHECK(reader.read(5) == 9);  // spxattencod[ch]
    }
    CHECK(reader.read(1) == 0);  // blkstrtinfoe

    reader.skip(2);              // dithflag[0..1]
    CHECK(reader.read(1) == 0);  // dynrnge
    CHECK(reader.read(1) == 1);  // spxinu (spxstre implied in block 0)
    CHECK(reader.read(1) == 1);  // chinspx[0]
    CHECK(reader.read(1) == 1);  // chinspx[1]
    reader.skip(2);              // spxstrtf
    CHECK(reader.read(3) == 4);  // spxbegf
    CHECK(reader.read(3) == 7);  // spxendf: synthesis runs to coefficient 229
    CHECK(reader.read(1) == 1);  // spxbndstrce
    const int begin = ac3::eac3::spx_begin_subbnd(4);
    const int end = ac3::eac3::spx_end_subbnd(7);
    int bands = 1;
    for (int sbnd = begin + 1; sbnd < end; ++sbnd) {
        bands += reader.read(1) == 0 ? 1 : 0;
    }
    // Block 0's spxcoe is implied by firstspxcos, so the coordinates follow
    // with no flag: spxblnd, mstrspxco, then 6 bits a band.
    for (int ch = 0; ch < 2; ++ch) {
        reader.skip(static_cast<std::size_t>(5 + 2 + 6 * bands));
    }
    // Coupling is off, so nothing follows before rematrixing - and with only
    // spectral extension in use spxbegf 4 leaves all four bands (§E3.3.2).
    for (int bnd = 0; bnd < 4; ++bnd) {
        CHECK(reader.read(1) == 0);  // rematflg
    }
    // chbwcod is NOT sent for a channel in spectral extension: its coded
    // bandwidth is where synthesis begins. So exponents come straight after,
    // and the first field is a 4-bit absolute exponent.
    CHECK(reader.read(4) <= 15);
}

TEST_CASE("spectral extension buys bits for the band it keeps", "[eac3][spx]") {
    // Synthesis removes coefficients from the coded spectrum outright, so
    // whatever is left must be able to afford a higher SNR offset. If it
    // cannot, the side information the tool adds is costing more than the
    // mantissas it removed, which for a handful of scale factors a frame
    // would mean something is wrong rather than merely unprofitable.
    for (const std::uint32_t kbps : {96u, 128u, 192u}) {
        CAPTURE(kbps);
        const auto plain = steady_state_frame({.bitrate_kbps = kbps}, 2);
        const auto extended = steady_state_frame({.bitrate_kbps = kbps, .spx = true}, 2);
        const int plain_offset = frame_snr_offset(plain);
        const int spx_offset = frame_snr_offset(extended);
        CAPTURE(plain_offset, spx_offset);
        CHECK(spx_offset >= plain_offset);
    }
}

TEST_CASE("E-AC-3 tools stack without desynchronising the frame", "[eac3][spx]") {
    using ac3::Acmod;
    // Coupling and spectral extension together is the configuration where the
    // boundaries have to agree: cplendf stops being transmitted and becomes a
    // function of spxbegf, and cplbegf has to be pulled down to leave the
    // coupling region non-empty.
    for (const auto acmod : {Acmod::k2_0, Acmod::k3_2}) {
        for (const int spxbegf : {0, 2, 4, 7}) {
            for (const std::uint32_t kbps : {192u, 448u}) {
                const int channels = ac3::fullbw_channel_count(acmod) + 1;
                CAPTURE(static_cast<int>(acmod), spxbegf, kbps);
                const auto frame = steady_state_frame({.bitrate_kbps = kbps,
                                                       .acmod = acmod,
                                                       .lfe = true,
                                                       .coupling = true,
                                                       .spx = true,
                                                       .spxbegf = spxbegf},
                                                      channels);
                CHECK(frame.size() ==
                      ac3::eac3::frame_words(ac3::SampleRate::k48000, kbps) * 2);
                CHECK(ac3::crc16(std::span{frame}.subspan(2)) == 0x0000);
            }
        }
    }
}

TEST_CASE("E-AC-3 encodes every supported layout", "[eac3]") {
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    auto pcm = tone_frame(6, 0);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    const auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());
    CHECK(frame->size() == ac3::eac3::frame_words(ac3::SampleRate::k48000, 448) * 2);
    CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
}

TEST_CASE("E-AC-3 VBR frame size tracks content complexity", "[eac3][vbr]") {
    // Runs of two per content type, comparing only the SECOND frame of each
    // run: the first frame after any transition has its block-0 MDCT window
    // straddling the previous run's differently-loud tail (50% overlap), so
    // its own cost is contaminated by a splice transient neither run's
    // steady-state content actually has - the same reason steady_state_frame
    // elsewhere in this file only keeps its LAST encoded frame.
    // 0.4 is comfortably inside the range wideband_frame's two channels stay
    // under the format's own frame-size ceiling without a max_kbps bound -
    // masking-model bit cost rises steeply above roughly half of the quality
    // range (see VbrConfig::quality's own comment), so this is deliberately
    // not a "high" quality value.
    const std::array<bool, 6> pattern{true, true, false, false, true, true};
    const auto sizes = vbr_frame_sizes(
        {.bitrate_kbps = 192, .vbr = ac3::eac3::VbrConfig{.quality = 0.4}}, pattern);
    CHECK(sizes[1] > sizes[3]);  // steady busy vs steady quiet
    CHECK(sizes[5] > sizes[3]);  // steady busy vs steady quiet, again
}

TEST_CASE("E-AC-3 VBR respects an explicit max_kbps ceiling", "[eac3][vbr]") {
    // Quality 1.0 against wideband content asks for far more than a tight
    // ceiling can hold, so the bounded fallback search must engage on every
    // frame and pin the size to the ceiling rather than exceed it.
    const std::array<bool, 4> pattern{true, true, true, true};
    constexpr std::uint32_t kMaxKbps = 96;
    const auto sizes = vbr_frame_sizes(
        {.bitrate_kbps = 192,
         .vbr = ac3::eac3::VbrConfig{.quality = 1.0, .max_kbps = kMaxKbps}},
        pattern);
    const auto ceiling_bytes = ac3::eac3::frame_words(ac3::SampleRate::k48000, kMaxKbps) * 2;
    bool any_pinned = false;
    for (std::size_t i = 1; i < sizes.size(); ++i) {  // skip the warm-up frame
        CHECK(sizes[i] <= ceiling_bytes);
        any_pinned = any_pinned || sizes[i] == ceiling_bytes;
    }
    CHECK(any_pinned);
}

TEST_CASE("E-AC-3 VBR respects an explicit min_kbps floor", "[eac3][vbr]") {
    // A low quality target on quiet content asks for almost nothing; the
    // floor must still be met, and finish_frame's own padding must not
    // disturb the checksum vbr_frame_sizes already verifies per frame.
    const std::array<bool, 3> pattern{false, false, false};
    constexpr std::uint32_t kMinKbps = 128;
    const auto sizes = vbr_frame_sizes(
        {.bitrate_kbps = 192,
         .vbr = ac3::eac3::VbrConfig{.quality = 0.05, .min_kbps = kMinKbps}},
        pattern);
    const auto floor_bytes = ac3::eac3::frame_words(ac3::SampleRate::k48000, kMinKbps) * 2;
    for (const auto size : sizes) {
        CHECK(size >= floor_bytes);
    }
}

TEST_CASE("E-AC-3 VBR size is monotonic in quality", "[eac3][vbr]") {
    // Same content, no bounds, two separate encoders: a higher quality
    // target must never produce a SMALLER frame than a lower one - the one
    // property that would silently invert if the quality-to-composite
    // mapping direction were ever wrong. Both values stay well under the
    // range where wideband_frame's bit cost approaches the format's own
    // ceiling (see VbrConfig::quality's own comment on that steepness).
    const std::array<bool, 3> pattern{true, true, true};
    const auto low = vbr_frame_sizes(
        {.bitrate_kbps = 192, .vbr = ac3::eac3::VbrConfig{.quality = 0.2}}, pattern);
    const auto high = vbr_frame_sizes(
        {.bitrate_kbps = 192, .vbr = ac3::eac3::VbrConfig{.quality = 0.5}}, pattern);
    CHECK(high.back() >= low.back());
}

TEST_CASE("E-AC-3 VBR pinned to CBR bounds reproduces CBR output exactly", "[eac3][vbr]") {
    // quality=1.0 with min_kbps == max_kbps == 192 forces the same fallback
    // search plain CBR runs, budgeted against the same 192 kbps ceiling - the
    // cheapest guard that the bounds-fallback path does not quietly diverge
    // from ordinary CBR rate control.
    ac3::eac3::FrameEncoder cbr_encoder{{.bitrate_kbps = 192}};
    ac3::eac3::FrameEncoder vbr_encoder{
        {.bitrate_kbps = 192,
         .vbr = ac3::eac3::VbrConfig{.quality = 1.0, .min_kbps = 192, .max_kbps = 192}}};
    std::uint64_t n = 0;
    for (int f = 0; f < 3; ++f) {
        auto pcm = wideband_frame(2, n);
        n += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto cbr_frame = cbr_encoder.encode_frame(views);
        const auto vbr_frame = vbr_encoder.encode_frame(views);
        REQUIRE(cbr_frame.has_value());
        REQUIRE(vbr_frame.has_value());
        CHECK(*cbr_frame == *vbr_frame);
    }
}

namespace {

// The canonical 7.1 access unit: a self-sufficient 5.1 bed plus a dependent
// carrying Ls, Rs, Lrs, Rrs - the spec's own worked example for chanmap.
ac3::eac3::AccessUnitConfig seven_one() {
    return {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
            .dependents = {{.bitrate_kbps = 224,
                            .acmod = ac3::Acmod::k2_2,
                            .chanmap = ac3::eac3::chanmap::k71Rear}}};
}

// The same access unit with VBR turned on for every substream, sharing one
// quality target the way Tools/Metadata already share across substreams in
// plan.cpp's eac3_config() - see Stage 2. 0.2 keeps six- and four-channel
// wideband content well under the per-substream frame-size ceiling; see
// VbrConfig::quality's own note on how steeply bit cost rises above roughly
// half of the quality range.
ac3::eac3::AccessUnitConfig seven_one_vbr() {
    auto cfg = seven_one();
    cfg.independent.vbr = ac3::eac3::VbrConfig{.quality = 0.2};
    cfg.dependents[0].vbr = ac3::eac3::VbrConfig{.quality = 0.2};
    return cfg;
}

}  // namespace

TEST_CASE("chanmap counts paired locations as two channels", "[eac3]") {
    using namespace ac3::eac3::chanmap;
    // Six of the sixteen Table E2.5 locations name a pair, so population count
    // is not channel count - and the two disagreeing is a wrong-speaker bug
    // that parses perfectly.
    CHECK(channel_count(kLeftBit) == 1);
    CHECK(channel_count(kLfeBit) == 1);
    CHECK(channel_count(kLrsRrsBit) == 2);
    CHECK(channel_count(kVhlVhrBit) == 2);
    CHECK(channel_count(static_cast<std::uint16_t>(kLeftBit | kCentreBit | kRightBit)) == 3);
    // Bit 0 is the MSB, which is the only numbering under which the spec's
    // example (bits 3, 4, 6 with acmod 2/2) comes to four channels.
    CHECK(k71Rear == 0x1A00);
    CHECK(channel_count(k71Rear) == 4);
}

namespace {
// A pair location gets exactly one bit in Table E2.5 (kLcRc, not separate
// kLc/kRc bits), so "Lc without Rc" cannot even be SPELLED as the
// std::uint16_t chanmap::allocate() and the rest of this header take - there
// is no bit position for one alone. Proven at compile time rather than by a
// runtime rejection test, because there is no runtime call this API could
// receive that would name half a pair: the type itself has nowhere to put
// it. (The higher-level string API, plan::parse_channels, accepts individual
// location NAMES before it ever reaches this type - that boundary is where a
// runtime check belongs, and test_plan.cpp's parse_channels test covers it.)
using namespace ac3::eac3::chanmap;
static_assert(std::popcount(kLcRcBit) == 1 && channel_count(kLcRcBit) == 2);
static_assert(std::popcount(kLrsRrsBit) == 1 && channel_count(kLrsRrsBit) == 2);
static_assert(std::popcount(kLsdRsdBit) == 1 && channel_count(kLsdRsdBit) == 2);
static_assert(std::popcount(kLwRwBit) == 1 && channel_count(kLwRwBit) == 2);
static_assert(std::popcount(kVhlVhrBit) == 1 && channel_count(kVhlVhrBit) == 2);
static_assert(std::popcount(kLtsRtsBit) == 1 && channel_count(kLtsRtsBit) == 2);
}  // namespace

TEST_CASE("acmod_for_chanmap picks the acmod that codes exactly the mask's channels", "[eac3]") {
    using namespace ac3::eac3::chanmap;
    // Full-bandwidth-only masks: each count picks the Table 5.8 acmod with
    // exactly that many full-bandwidth channels (never dual mono).
    CHECK(acmod_for_chanmap(kCentreBit) == std::pair{ac3::Acmod::k1_0, false});
    CHECK(acmod_for_chanmap(kLcRcBit) == std::pair{ac3::Acmod::k2_0, false});  // pair: 2 channels
    CHECK(acmod_for_chanmap(static_cast<std::uint16_t>(kLeftBit | kCentreBit | kRightBit)) ==
          std::pair{ac3::Acmod::k3_0, false});
    // Four is a genuine tie (3/1 and 2/2 both code four full-bandwidth
    // channels) - the named layouts (k71Rear, kTopQuad) already assume 2/2
    // wins, so this pins that choice rather than leaving it to whichever the
    // search order hits first by accident.
    CHECK(acmod_for_chanmap(k71Rear) == std::pair{ac3::Acmod::k2_2, false});
    CHECK(acmod_for_chanmap(kTopQuad) == std::pair{ac3::Acmod::k2_2, false});
    CHECK(acmod_for_chanmap(static_cast<std::uint16_t>(kLeftBit | kCentreBit | kRightBit |
                                                        kLeftSurroundBit | kRightSurroundBit)) ==
          std::pair{ac3::Acmod::k3_2, false});

    // An LFE-type location subtracts one from the full-bandwidth budget
    // lfeon needs to supply - LFE and LFE2 are interchangeable here, since
    // both come from the same one-bit lfeon slot.
    CHECK(acmod_for_chanmap(static_cast<std::uint16_t>(kCentreBit | kLfeBit)) ==
          std::pair{ac3::Acmod::k1_0, true});
    CHECK(acmod_for_chanmap(static_cast<std::uint16_t>(kCentreBit | kLfe2Bit)) ==
          std::pair{ac3::Acmod::k1_0, true});

    // No acmod codes zero full-bandwidth channels (Table 5.8's narrowest, 1/0,
    // is already one) or more than five (3/2), so an LFE-type location with no
    // companion, and a request for six or more, both have no answer.
    CHECK_FALSE(acmod_for_chanmap(kLfeBit).has_value());
    CHECK_FALSE(acmod_for_chanmap(kLfe2Bit).has_value());
    // lfeon is a single bit: a substream can never carry LFE and LFE2 at once,
    // however much room the full-bandwidth side would otherwise leave.
    CHECK_FALSE(acmod_for_chanmap(static_cast<std::uint16_t>(kCentreBit | kLfeBit | kLfe2Bit)).has_value());
}

TEST_CASE("allocate partitions an arbitrary channel set into a bed and dependents", "[eac3]") {
    using namespace ac3::eac3::chanmap;

    // A plain 5.1 request needs no dependent at all.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(
            kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit));
        REQUIRE(plan.has_value());
        CHECK(plan->bed_acmod == ac3::Acmod::k3_2);
        CHECK(plan->bed_lfe);
        CHECK(plan->dependents.empty());
    }

    // A request the bed cannot fully cover packs the remainder into a
    // dependent - Vhl/Vhr here, exactly what k512Height already names.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(
            kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit | kVhlVhrBit));
        REQUIRE(plan.has_value());
        CHECK(plan->bed_acmod == ac3::Acmod::k3_2);
        REQUIRE(plan->dependents.size() == 1);
        CHECK(plan->dependents[0] == kVhlVhrBit);
    }

    // No Ls/Rs in the request at all: the bed narrows to whatever Table 5.8
    // shape actually fits (3/0 here), rather than defaulting to 3/2 and
    // leaving Ls/Rs to a dependent that was never asked for.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(kLeftBit | kCentreBit | kRightBit | kLfeBit));
        REQUIRE(plan.has_value());
        CHECK(plan->bed_acmod == ac3::Acmod::k3_0);
        CHECK(plan->dependents.empty());
    }

    // Three pair locations with no LFE need six full-bandwidth channels -
    // impossible for one dependent, since Table 5.8 caps a single acmod at
    // five, whether or not it also carries an LFE-type channel. The packer
    // has to open a second dependent rather than (incorrectly) filling one to
    // six - this is the exact boundary a first version of the allocator got
    // wrong, jamming all three pairs into one dependent and producing a
    // chanmap no acmod could code.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(
            kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit | kLcRcBit | kLrsRrsBit |
            kLsdRsdBit));
        REQUIRE(plan.has_value());
        REQUIRE(plan->dependents.size() == 2);
        for (const auto dependent : plan->dependents) {
            CHECK(channel_count(dependent) <= kMaxSubstreamFullbw);
            CHECK(acmod_for_chanmap(dependent).has_value());
        }
        CHECK(channel_count(plan->dependents[0]) + channel_count(plan->dependents[1]) == 6);
    }

    // LFE2 needs a full-bandwidth companion in its OWN substream (acmod
    // always contributes at least one) - here it rides with Vhc.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(
            kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit | kVhcBit | kLfe2Bit));
        REQUIRE(plan.has_value());
        REQUIRE(plan->dependents.size() == 1);
        CHECK(plan->dependents[0] == static_cast<std::uint16_t>(kVhcBit | kLfe2Bit));
    }

    // LFE2 with nothing left to share a substream with is unsatisfiable: the
    // bed already claims every other location the request named.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(
            kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit | kLfe2Bit));
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error() == AllocationError::kOrphanLfe2);
    }

    // §E3.8.2: sixteen rendered channels is the whole-programme ceiling.
    // Seventeen is refused outright, before any partitioning is attempted.
    {
        const auto locations = static_cast<std::uint16_t>(
            kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit | kLcRcBit | kLrsRrsBit |
            kLsdRsdBit | kLwRwBit | kVhlVhrBit | kVhcBit);
        REQUIRE(channel_count(locations) == 17);
        const auto plan = allocate(locations);
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error() == AllocationError::kTooManyChannels);
    }

    // A request with no front coverage at all matches no Table 5.8 acmod -
    // there is no "surrounds only" mode for a bed to fall back to.
    {
        const auto plan = allocate(kLrsRrsBit);
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error() == AllocationError::kNoBedFit);
    }
}

TEST_CASE("allocate refuses a request no Table 5.8 mode can anchor a bed on",
          "[eac3]") {
    using namespace ac3::eac3::chanmap;
    // bed_for() takes the WIDEST acmod whose own set is a SUBSET of the
    // request, and Table 5.8's narrowest mode is exactly {C} - so ANY request
    // containing Centre always matches at least 1/0. The only way to fail is
    // a request with no Centre AND not both Left and Right together: nothing
    // in Table 5.8 codes L or R alone, or a surround without its own front
    // pair. This covers both the "ceiling/extras only" and the "incomplete
    // front shape" ways a request can fail that test.
    for (const auto locations : {
             kLeftBit,                                                   // L alone
             kRightBit,                                                   // R alone
             static_cast<std::uint16_t>(kLeftBit | kLeftSurroundBit),    // L+Ls, no R
             static_cast<std::uint16_t>(kRightBit | kRightSurroundBit),  // R+Rs, no L
             kVhlVhrBit,   // ceiling only, no front channel at all
             kLcRcBit,     // Lc/Rc only - aux locations Table 5.8 never names
         }) {
        CAPTURE(locations);
        const auto plan = allocate(locations);
        REQUIRE_FALSE(plan.has_value());
        CHECK(plan.error() == AllocationError::kNoBedFit);
    }

    // C + R with no L LOOKS just as incomplete as the cases above, but is
    // not: chanmap lets a dependent's coded channel carry any location
    // regardless of what the bed's own acmod would naturally mean, so this
    // is bed=1/0 (mono, C only) plus a one-channel dependent explicitly
    // labelled R - odd content, but a legal bitstream (§E2.3.1.8 only checks
    // that the dependent's chanmap population matches ITS OWN acmod's coded
    // count, never that it matches the bed). Proving this does NOT get
    // refused matters as much as the rejections above: over-refusing a
    // request the format can actually carry would be its own bug.
    {
        const auto plan = allocate(static_cast<std::uint16_t>(kCentreBit | kRightBit));
        REQUIRE(plan.has_value());
        CHECK(plan->bed_acmod == ac3::Acmod::k1_0);
        CHECK_FALSE(plan->bed_lfe);
        REQUIRE(plan->dependents.size() == 1);
        CHECK(plan->dependents[0] == kRightBit);
    }
}

TEST_CASE("E-AC-3 access unit concatenates its substreams", "[eac3]") {
    const auto unit = ac3::eac3::build_silent_access_unit(seven_one());
    REQUIRE(unit.has_value());
    REQUIRE(unit->substream_count() == 2);

    // The access unit is exactly its substreams end to end - concatenation IS
    // the framing, since a decoder finds each one by sync word and frmsiz.
    CHECK(unit->substream_bytes[0] == ac3::eac3::frame_words(ac3::SampleRate::k48000, 448) * 2);
    CHECK(unit->substream_bytes[1] == ac3::eac3::frame_words(ac3::SampleRate::k48000, 224) * 2);
    CHECK(unit->bytes.size() == unit->substream_bytes[0] + unit->substream_bytes[1]);
    CHECK(unit->bytes.size() == ac3::eac3::access_unit_words(seven_one()) * 2);

    // crc2 is per substream, so each has to check out on its own.
    for (std::size_t i = 0; i < unit->substream_count(); ++i) {
        const auto sub = unit->substream(i);
        CHECK(std::to_integer<std::uint8_t>(sub[0]) == 0x0B);
        CHECK(std::to_integer<std::uint8_t>(sub[1]) == 0x77);
        CHECK(ac3::crc16(sub.subspan(2)) == 0x0000);
    }
}

TEST_CASE("E-AC-3 dependent substream bsi", "[eac3]") {
    const auto unit = ac3::eac3::build_silent_access_unit(seven_one());
    REQUIRE(unit.has_value());

    // The independent substream keeps the exact layout it had before
    // substreams existed: strmtyp 0, substreamid 0, compre clear.
    ac3::BitReader lead{unit->substream(0)};
    lead.skip(16);
    CHECK(lead.read(2) == 0);  // strmtyp
    CHECK(lead.read(3) == 0);  // substreamid

    ac3::BitReader dep{unit->substream(1)};
    dep.skip(16);
    CHECK(dep.read(2) == 1);  // strmtyp: dependent
    // E2.3.1.2: a dependent's id lives in its own numbering space and starts
    // at 0 - it does not continue its parent's.
    CHECK(dep.read(3) == 0);  // substreamid
    dep.skip(11 + 2 + 2);     // frmsiz, fscod, numblkscod
    CHECK(dep.read(3) == static_cast<std::uint32_t>(ac3::Acmod::k2_2));
    CHECK(dep.read(1) == 0);  // lfeon
    dep.skip(5 + 5);          // bsid, dialnorm
    // E3.8.5: compre marks the LAST dependent of the program, and drags in an
    // 8-bit compr that 7.7.1 defines as unity at 0x00.
    CHECK(dep.read(1) == 1);  // compre
    CHECK(dep.read(8) == 0);  // compr: 0 dB
    CHECK(dep.read(1) == 1);  // chanmape
    CHECK(dep.read(16) == ac3::eac3::chanmap::k71Rear);
}

TEST_CASE("E-AC-3 access unit carries real audio in every substream", "[eac3]") {
    ac3::eac3::AccessUnitEncoder encoder{seven_one()};
    // Ten spans in: the bed's six, then the dependent's four. Two of the
    // dependent's overwrite the bed's surrounds, so eight channels come out.
    REQUIRE(encoder.channel_count() == 10);

    std::uint64_t n = 0;
    for (int f = 0; f < 3; ++f) {
        auto pcm = tone_frame(10, n);
        n += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        REQUIRE(unit->substream_count() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            CHECK(ac3::crc16(unit->substream(i).subspan(2)) == 0x0000);
        }
        if (f > 0) {
            // Both substreams run their own SNR search against their own share
            // of the rate; neither may saturate.
            CHECK(frame_snr_offset(unit->substream(0)) < 1023);
            CHECK(frame_snr_offset(unit->substream(1)) < 1023);
        }
    }
}

TEST_CASE("E-AC-3 VBR access unit lets every substream size vary on its own",
          "[eac3][vbr]") {
    ac3::eac3::AccessUnitEncoder encoder{seven_one_vbr()};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(nchans == 10);

    // Runs of two per content type, across all ten coded channels, so both
    // substreams see the same shift in complexity at the same time. Only the
    // SECOND frame of each run is compared - the first has its block-0 MDCT
    // window straddling the previous run's differently-loud tail, which
    // contaminates its cost with a splice transient neither steady-state
    // signal actually has (see the single-substream version of this test).
    const std::array<bool, 6> busy{true, true, false, false, true, true};
    std::vector<std::size_t> sub0_sizes;
    std::vector<std::size_t> sub1_sizes;
    std::uint64_t n = 0;
    for (const bool b : busy) {
        auto source = b ? wideband_frame(static_cast<int>(nchans), n)
                        : quiet_frame(static_cast<int>(nchans), n);
        n += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : source) {
            views.emplace_back(channel);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        REQUIRE(unit->substream_count() == 2);
        CHECK(ac3::crc16(unit->substream(0).subspan(2)) == 0x0000);
        CHECK(ac3::crc16(unit->substream(1).subspan(2)) == 0x0000);
        sub0_sizes.push_back(unit->substream(0).size());
        sub1_sizes.push_back(unit->substream(1).size());
    }
    // Steady busy (index 1) vs steady quiet (index 3) vs steady busy again
    // (index 5).
    CHECK(sub0_sizes[1] > sub0_sizes[3]);
    CHECK(sub1_sizes[1] > sub1_sizes[3]);
    CHECK(sub0_sizes[5] > sub0_sizes[3]);
    CHECK(sub1_sizes[5] > sub1_sizes[3]);
}

TEST_CASE("E-AC-3 rejects substream layouts it cannot express", "[eac3]") {
    using ac3::eac3::AccessUnitConfig;
    using ac3::eac3::FrameConfig;
    using ac3::eac3::StreamType;

    // E2.3.1.8: the locations a chanmap names must equal the channels acmod
    // and lfeon code. A decoder would not fail on this - it would just put
    // audio in the wrong speakers - so the encoder has to refuse it.
    auto wrong = seven_one();
    wrong.dependents[0].chanmap = ac3::eac3::chanmap::kLrsRrsBit;  // 2, not 4
    CHECK(ac3::eac3::build_silent_access_unit(wrong).error() ==
          ac3::FrameError::kInvalidChannelMap);

    // The reverse direction: a chanmap naming MORE channels than acmod codes
    // is exactly as wrong as naming fewer - E2.3.1.8 requires the two to
    // agree exactly, not merely "chanmap covers what acmod needs".
    auto over = seven_one();
    over.dependents[0].chanmap = static_cast<std::uint16_t>(
        ac3::eac3::chanmap::k71Rear | ac3::eac3::chanmap::kCsBit);  // 5, not 4
    CHECK(ac3::eac3::build_silent_access_unit(over).error() ==
          ac3::FrameError::kInvalidChannelMap);

    // Only a dependent substream may carry one.
    CHECK(ac3::eac3::build_silent_frame(
              {.chanmap = ac3::eac3::chanmap::kLeftBit})
              .error() == ac3::FrameError::kInvalidSubstream);

    // E2.3.1.2: eight dependents per independent substream, no more.
    AccessUnitConfig crowded;
    crowded.dependents.assign(
        9, {.bitrate_kbps = 32, .chanmap = ac3::eac3::chanmap::kLwRwBit});
    CHECK(ac3::eac3::build_silent_access_unit(crowded).error() ==
          ac3::FrameError::kInvalidSubstream);

    // Every substream codes the same 1536 samples, so a dependent cannot
    // disagree with its parent about the sample rate.
    AccessUnitConfig mixed;
    mixed.dependents.push_back({.sample_rate = ac3::SampleRate::k32000,
                                .bitrate_kbps = 96,
                                .chanmap = ac3::eac3::chanmap::kLwRwBit});
    CHECK(ac3::eac3::build_silent_access_unit(mixed).error() ==
          ac3::FrameError::kInvalidSubstream);

    // An access unit must begin with an independent substream.
    AccessUnitConfig headless;
    headless.independent.strmtyp = StreamType::kDependent;
    CHECK(ac3::eac3::build_silent_access_unit(headless).error() ==
          ac3::FrameError::kInvalidSubstream);
}

TEST_CASE("E-AC-3 does not reject two substreams that claim the same location",
          "[eac3]") {
    // §E2.3.1.8's chanmap check, and the one above it, only ever look at ONE
    // substream at a time - nothing compares a dependent's chanmap against
    // its SIBLINGS'. So two dependents both naming Vhc is not caught here at
    // all: §E3.8.2 says the later substream simply overwrites the earlier one
    // at decode time (proven with real, distinguishable audio in
    // test_eac3_decoder.cpp). This test is the ENCODER's half of that
    // contract: building such an access unit must succeed, not fail, and the
    // aggregate sixteen-channel ceiling (kTooManyChannels) must count the
    // shared location once, not once per substream that claims it.
    using ac3::eac3::AccessUnitConfig;
    using ac3::eac3::FrameConfig;
    namespace cm = ac3::eac3::chanmap;

    const FrameConfig claim_vhc{
        .bitrate_kbps = 32, .acmod = ac3::Acmod::k1_0, .chanmap = cm::kVhcBit};
    const AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {claim_vhc, claim_vhc}};
    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());
    CHECK(unit->substream_count() == 3);

    // A sharper version of the same point: three dependents that all claim
    // the SAME five locations would sum to 6 + 5*3 = 21 if the ceiling check
    // added every substream's count up rather than unioning the locations -
    // past sixteen - but the programme only ever renders 6 + 5 = 11 distinct
    // locations, so it has to be accepted.
    const FrameConfig claim_five{
        .bitrate_kbps = 256,
        .acmod = ac3::Acmod::k3_2,
        .chanmap = static_cast<std::uint16_t>(cm::kLcRcBit | cm::kLrsRrsBit | cm::kCsBit)};
    AccessUnitConfig triple;
    triple.independent = config.independent;
    triple.dependents = {claim_five, claim_five, claim_five};
    const auto triple_unit = ac3::eac3::build_silent_access_unit(triple);
    REQUIRE(triple_unit.has_value());
    CHECK(triple_unit->substream_count() == 4);
}

TEST_CASE("E-AC-3 rejects an access unit that renders more than sixteen channels", "[eac3]") {
    using ac3::eac3::AccessUnitConfig;
    namespace cm = ac3::eac3::chanmap;

    // Bed (6) plus two five-channel dependents plus a one-channel dependent,
    // every location distinct from the bed's and from each other, sums to
    // seventeen rendered channels - past §E3.8.2's ceiling, even though each
    // substream's OWN chanmap-vs-acmod/lfeon count agrees (kInvalidChannelMap
    // already covers that) and there are well under eight dependents. Only an
    // aggregate, whole-access-unit check catches this.
    const AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 448,
                        .acmod = ac3::Acmod::k3_2,
                        .chanmap = static_cast<std::uint16_t>(cm::kLcRcBit | cm::kLrsRrsBit | cm::kCsBit)},
                       {.bitrate_kbps = 448,
                        .acmod = ac3::Acmod::k3_2,
                        .chanmap = static_cast<std::uint16_t>(cm::kLsdRsdBit | cm::kLwRwBit | cm::kTsBit)},
                       {.bitrate_kbps = 32, .acmod = ac3::Acmod::k1_0, .chanmap = cm::kVhcBit}}};
    CHECK(ac3::eac3::build_silent_access_unit(config).error() ==
          ac3::FrameError::kTooManyChannels);

    // The same access unit minus its one-channel dependent renders exactly
    // sixteen (6 + 5 + 5) and must be accepted - the ceiling is sixteen, not
    // "fewer than seventeen minus one".
    auto sixteen = config;
    sixteen.dependents.pop_back();
    CHECK(ac3::eac3::build_silent_access_unit(sixteen).has_value());
}

TEST_CASE("E-AC-3 rejects configurations it cannot express", "[eac3]") {
    // §E2.3.1.3: frmsiz is an arbitrary 11-bit word count, not an index into
    // Table 5.18 the way AC-3's frmsizecod is - so unlike AC-3, a bitrate
    // only has to land on a legal word count, not one of the 19 nominal
    // rates (see "E-AC-3 accepts a bitrate off Table 5.18's nominal list"
    // below). 0 gives frame_words() == 0, which is not a syncframe at all;
    // something past kMaxFrameWords overflows the 11-bit field.
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 0}).error() ==
          ac3::FrameError::kInvalidBitrate);
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 2000}).error() ==
          ac3::FrameError::kInvalidBitrate);
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 192, .dialnorm = 0}).error() ==
          ac3::FrameError::kInvalidDialnorm);
    // 1+1 needs Ch2's own dialnorm2 (§E1.2); missing or out of range is
    // exactly as invalid as Ch1's own dialnorm would be.
    CHECK(ac3::eac3::build_silent_frame(
              {.bitrate_kbps = 192, .acmod = ac3::Acmod::kDualMono})
              .error() == ac3::FrameError::kInvalidDialnorm);
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 192,
                                         .acmod = ac3::Acmod::kDualMono,
                                         .dialnorm2 = 0})
              .error() == ac3::FrameError::kInvalidDialnorm);
}

TEST_CASE("E-AC-3 dual mono builds a valid silent frame once dialnorm2 is set", "[eac3]") {
    const auto frame = ac3::eac3::build_silent_frame(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::kDualMono, .dialnorm2 = 18});
    REQUIRE(frame.has_value());
    CHECK(frame->size() == 768);
}

TEST_CASE("E-AC-3 accepts a bitrate off Table 5.18's nominal list", "[eac3]") {
    // 100 kbps is not one of Table 5.18's 19 values, but E-AC-3 does not
    // index that table - it lands on a perfectly legal 200-word frame
    // (frame_words(k48000, 100) == 200), so it has to work. Real audio, not
    // silence: an all-zero frame proves the syntax packs, not that a real
    // SNR search actually fits the budget this bitrate gives it.
    REQUIRE(ac3::eac3::frame_words(ac3::SampleRate::k48000, 100) == 200);
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 100}};
    std::uint64_t n = 0;
    for (int f = 0; f < 3; ++f) {
        auto pcm = tone_frame(2, n);
        n += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        CHECK(frame->size() == 400);  // 200 words
        CHECK(ac3::crc16(std::span{*frame}.subspan(2)) == 0x0000);
    }
}
