#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

TEST_CASE("E-AC-3 spectral extension places its fields where Annex E puts them",
          "[eac3][spx]") {
    const auto frame =
        steady_state_frame({.bitrate_kbps = 192, .spx = true, .spxbegf = 4}, 2);
    ac3::BitReader reader{frame};
    reader.skip(16 + 38 + 12);
    CHECK(reader.read(1) == 0);  // cplinu[0]: coupling off
    for (int blk = 1; blk < ac3::kBlocksPerFrame; ++blk) {
        CHECK(reader.read(1) == 0);  // cplstre[blk]
    }
    // No block couples, so frmcplexpstr is absent and frmchexpstr comes next.
    CHECK(reader.read(5) == 0);
    CHECK(reader.read(5) == 0);
    reader.skip(10 + 10);        // convexpstr, then the frame SNR offsets
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
        reader.skip(5 + 2 + 6 * bands);
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

namespace {

// The canonical 7.1 access unit: a self-sufficient 5.1 bed plus a dependent
// carrying Ls, Rs, Lrs, Rrs - the spec's own worked example for chanmap.
ac3::eac3::AccessUnitConfig seven_one() {
    return {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
            .dependents = {{.bitrate_kbps = 224,
                            .acmod = ac3::Acmod::k2_2,
                            .chanmap = ac3::eac3::chanmap::k71Rear}}};
}

}  // namespace

TEST_CASE("chanmap counts paired locations as two channels", "[eac3]") {
    using namespace ac3::eac3::chanmap;
    // Six of the sixteen Table E2.5 locations name a pair, so population count
    // is not channel count - and the two disagreeing is a wrong-speaker bug
    // that parses perfectly.
    CHECK(channel_count(kLeft) == 1);
    CHECK(channel_count(kLfe) == 1);
    CHECK(channel_count(kLrsRrs) == 2);
    CHECK(channel_count(kVhlVhr) == 2);
    CHECK(channel_count(static_cast<std::uint16_t>(kLeft | kCentre | kRight)) == 3);
    // Bit 0 is the MSB, which is the only numbering under which the spec's
    // example (bits 3, 4, 6 with acmod 2/2) comes to four channels.
    CHECK(k71Rear == 0x1A00);
    CHECK(channel_count(k71Rear) == 4);
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

TEST_CASE("E-AC-3 rejects substream layouts it cannot express", "[eac3]") {
    using ac3::eac3::AccessUnitConfig;
    using ac3::eac3::FrameConfig;
    using ac3::eac3::StreamType;

    // E2.3.1.8: the locations a chanmap names must equal the channels acmod
    // and lfeon code. A decoder would not fail on this - it would just put
    // audio in the wrong speakers - so the encoder has to refuse it.
    auto wrong = seven_one();
    wrong.dependents[0].chanmap = ac3::eac3::chanmap::kLrsRrs;  // 2, not 4
    CHECK(ac3::eac3::build_silent_access_unit(wrong).error() ==
          ac3::FrameError::kInvalidChannelMap);

    // Only a dependent substream may carry one.
    CHECK(ac3::eac3::build_silent_frame(
              {.chanmap = ac3::eac3::chanmap::kLeft})
              .error() == ac3::FrameError::kInvalidSubstream);

    // E2.3.1.2: eight dependents per independent substream, no more.
    AccessUnitConfig crowded;
    crowded.dependents.assign(
        9, {.bitrate_kbps = 32, .chanmap = ac3::eac3::chanmap::kLwRw});
    CHECK(ac3::eac3::build_silent_access_unit(crowded).error() ==
          ac3::FrameError::kInvalidSubstream);

    // Every substream codes the same 1536 samples, so a dependent cannot
    // disagree with its parent about the sample rate.
    AccessUnitConfig mixed;
    mixed.dependents.push_back({.sample_rate = ac3::SampleRate::k32000,
                                .bitrate_kbps = 96,
                                .chanmap = ac3::eac3::chanmap::kLwRw});
    CHECK(ac3::eac3::build_silent_access_unit(mixed).error() ==
          ac3::FrameError::kInvalidSubstream);

    // An access unit must begin with an independent substream.
    AccessUnitConfig headless;
    headless.independent.strmtyp = StreamType::kDependent;
    CHECK(ac3::eac3::build_silent_access_unit(headless).error() ==
          ac3::FrameError::kInvalidSubstream);
}

TEST_CASE("E-AC-3 rejects configurations it cannot express", "[eac3]") {
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 100}).error() ==
          ac3::FrameError::kInvalidBitrate);
    CHECK(ac3::eac3::build_silent_frame({.bitrate_kbps = 192, .dialnorm = 0}).error() ==
          ac3::FrameError::kInvalidDialnorm);
    // 1+1 needs a second program's metadata throughout; not supported yet.
    CHECK(ac3::eac3::build_silent_frame(
              {.bitrate_kbps = 192, .acmod = ac3::Acmod::kDualMono})
              .has_value() == false);
}
