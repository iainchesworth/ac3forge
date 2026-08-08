#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/encoder/eac3_frame.hpp"

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
// frmchexpstr(2x5) + convexpstr(2x5) lands exactly on frmcsnroffst.
int frame_snr_offset(std::span<const std::byte> frame) {
    ac3::BitReader reader{frame};
    reader.skip(16 + 38 + 12 + 6 + 10 + 10);
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
