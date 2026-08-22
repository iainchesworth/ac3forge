#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/atmos.hpp"
#include "mp4/mp4.hpp"

// These tests read the muxer's output back with an independent ISOBMFF box
// walker rather than comparing against bytes this same code produced - the
// same reasoning as test_matroska.cpp's own header comment: a muxer checked
// only against itself proves nothing about whether a player can open the
// file.

namespace {

using Bytes = std::vector<std::byte>;

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

[[nodiscard]] std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

[[nodiscard]] std::uint32_t u32_at(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint32_t>(byte_at(data, offset)) << 24) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 8) |
           static_cast<std::uint32_t>(byte_at(data, offset + 3));
}

[[nodiscard]] std::uint16_t u16_at(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(byte_at(data, offset)) << 8) |
                                      byte_at(data, offset + 1));
}

[[nodiscard]] std::string fourcc_at(std::span<const std::byte> data, std::size_t offset) {
    std::string s(4, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>(byte_at(data, offset + i));
    }
    return s;
}

// One ISOBMFF box: size(4) + type(4) + body. `payload`/`length` describe the
// body only (offset past the 8-byte header, and size - 8).
struct Element {
    std::string type;
    std::size_t payload = 0;
    std::uint64_t length = 0;
};

// Boxes whose body is a flat run of complete child boxes covering it exactly.
// Everything else this muxer writes is either a FullBox with non-box fields
// before any child (stsd, dref, mvhd/tkhd/mdhd/hdlr/smhd/stts/stsc/stsz/
// stco) or a fixed-layout AudioSampleEntry with a config box appended after
// its own fields (the 'ec-3'/'ac-3' sample entry) - recursing into those the
// same way would misparse their leading fields as a box header, so they are
// read with their own dedicated helpers below instead.
bool is_container(const std::string& type) {
    return type == "moov" || type == "trak" || type == "mdia" || type == "minf" ||
           type == "stbl" || type == "dinf";
}

void walk(std::span<const std::byte> file, std::size_t pos, std::size_t end,
         std::vector<Element>& out) {
    while (pos < end) {
        REQUIRE(pos + 8 <= end);
        const auto size = u32_at(file, pos);
        const auto type = fourcc_at(file, pos + 4);
        // A size that overruns its parent, or undersizes its own header,
        // means the muxer wrote a bad length - exactly the failure this
        // walker exists to catch.
        REQUIRE(size >= 8);
        REQUIRE(pos + size <= end);
        out.push_back({type, pos + 8, size - 8});
        if (is_container(type)) {
            walk(file, pos + 8, pos + size, out);
        }
        pos += size;
    }
    REQUIRE(pos == end);
}

std::vector<Element> parse(std::span<const std::byte> file) {
    std::vector<Element> out;
    walk(file, 0, file.size(), out);
    return out;
}

const Element* find(const std::vector<Element>& elements, const std::string& type) {
    for (const auto& e : elements) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

std::size_t count(const std::vector<Element>& elements, const std::string& type) {
    std::size_t n = 0;
    for (const auto& e : elements) {
        if (e.type == type) {
            ++n;
        }
    }
    return n;
}

struct SampleEntryInfo {
    std::string codec_id;
    std::uint16_t channels = 0;
    std::uint32_t samplerate_fixed = 0;  // 16.16 fixed point
    std::string config_type;             // "dac3" or "dec3"
    Bytes config_payload;
};

// stsd body: version+flags(4), entry_count(4), then the sample entry box
// (ISO/IEC 14496-12 §8.5.2/§12.2.3): reserved(6)+data_reference_index(2),
// reserved(8), channelcount(2), samplesize(2), pre_defined(2), reserved(2),
// samplerate(4), then this muxer's one child configuration box.
SampleEntryInfo read_sample_entry(std::span<const std::byte> file, const Element& stsd) {
    const std::size_t entry_offset = stsd.payload + 8;
    const auto entry_size = u32_at(file, entry_offset);
    SampleEntryInfo info;
    info.codec_id = fourcc_at(file, entry_offset + 4);
    const std::size_t body = entry_offset + 8;
    info.channels = u16_at(file, body + 16);
    info.samplerate_fixed = u32_at(file, body + 24);
    const std::size_t config_offset = body + 28;
    const auto config_size = u32_at(file, config_offset);
    info.config_type = fourcc_at(file, config_offset + 4);
    REQUIRE(config_offset + config_size == entry_offset + entry_size);
    info.config_payload.assign(file.begin() + static_cast<std::ptrdiff_t>(config_offset + 8),
                               file.begin() + static_cast<std::ptrdiff_t>(config_offset + config_size));
    return info;
}

struct MdhdInfo {
    std::uint32_t timescale = 0;
    std::uint32_t duration = 0;
    std::uint16_t language = 0;
};

// mdhd body: version+flags(4), creation_time(4), modification_time(4),
// timescale(4), duration(4), language(2), pre_defined(2).
MdhdInfo read_mdhd(std::span<const std::byte> file, const Element& mdhd) {
    return {.timescale = u32_at(file, mdhd.payload + 12),
            .duration = u32_at(file, mdhd.payload + 16),
            .language = u16_at(file, mdhd.payload + 20)};
}

// stts body: version+flags(4), entry_count(4), [sample_count(4),
// sample_delta(4)] - this muxer always writes exactly one entry.
std::pair<std::uint32_t, std::uint32_t> read_stts_first(std::span<const std::byte> file,
                                                        const Element& stts) {
    return {u32_at(file, stts.payload + 8), u32_at(file, stts.payload + 12)};
}

// stsz body: version+flags(4), sample_size(4), sample_count(4),
// [entry_size(4)]*sample_count.
std::vector<std::uint32_t> read_stsz(std::span<const std::byte> file, const Element& stsz) {
    const auto sample_count = u32_at(file, stsz.payload + 8);
    std::vector<std::uint32_t> sizes(sample_count);
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        sizes[i] = u32_at(file, stsz.payload + 12 + static_cast<std::size_t>(i) * 4);
    }
    return sizes;
}

// stco body: version+flags(4), entry_count(4), [chunk_offset(4)]*entry_count.
std::vector<std::uint32_t> read_stco(std::span<const std::byte> file, const Element& stco) {
    const auto entry_count = u32_at(file, stco.payload + 4);
    std::vector<std::uint32_t> offsets(entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        offsets[i] = u32_at(file, stco.payload + 8 + static_cast<std::size_t>(i) * 4);
    }
    return offsets;
}

mp4::AudioTrack sample_track(int channels = 6) {
    return mp4::AudioTrack{.codec_id = std::string{mp4::kCodecEac3},
                           .sample_rate = 48000,
                           .channels = channels,
                           .samples_per_frame = 1536,
                           .codec_config = Bytes{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}}};
}

}  // namespace

TEST_CASE("MP4 file parses as well-formed ISOBMFF boxes", "[mp4]") {
    // walk() REQUIREs every child to fit inside its parent and every box to
    // account for exactly its own declared size - the whole correctness
    // question for a muxer that computes offsets ahead of the data itself.
    const std::vector<Bytes> frames{frame_of(1792, 0x11), frame_of(1792, 0x22),
                                    frame_of(1792, 0x33)};
    const auto file = mp4::mux(sample_track(), frames);
    REQUIRE(file.has_value());

    const auto elements = parse(*file);
    REQUIRE(find(elements, "ftyp") != nullptr);
    REQUIRE(find(elements, "moov") != nullptr);
    REQUIRE(find(elements, "mdat") != nullptr);
    REQUIRE(find(elements, "trak") != nullptr);
    REQUIRE(find(elements, "mdia") != nullptr);
    REQUIRE(find(elements, "minf") != nullptr);
    REQUIRE(find(elements, "stbl") != nullptr);
    REQUIRE(find(elements, "stsd") != nullptr);
    REQUIRE(find(elements, "stts") != nullptr);
    REQUIRE(find(elements, "stsc") != nullptr);
    REQUIRE(find(elements, "stsz") != nullptr);
    REQUIRE(find(elements, "stco") != nullptr);
    CHECK(count(elements, "trak") == 1);

    // major_brand must say isom or nothing generic will open it.
    const auto* ftyp = find(elements, "ftyp");
    CHECK(fourcc_at(*file, ftyp->payload) == "isom");
}

TEST_CASE("MP4 sample entry and mdhd describe the audio", "[mp4]") {
    const std::vector<Bytes> frames(63, frame_of(896, 0xAB));
    const auto track = sample_track(10);
    const auto file = mp4::mux(track, frames);
    REQUIRE(file.has_value());
    const auto elements = parse(*file);

    const auto* stsd = find(elements, "stsd");
    REQUIRE(stsd != nullptr);
    const auto entry = read_sample_entry(*file, *stsd);
    CHECK(entry.codec_id == "ec-3");
    CHECK(entry.channels == 10);
    CHECK(entry.samplerate_fixed == (48000U << 16));
    CHECK(entry.config_type == "dec3");
    CHECK(entry.config_payload == track.codec_config);

    const auto* mdhd = find(elements, "mdhd");
    REQUIRE(mdhd != nullptr);
    const auto md = read_mdhd(*file, *mdhd);
    CHECK(md.timescale == 48000);
    // 63 frames of 1536 samples is 96768 samples exactly - at a movie/media
    // timescale equal to the sample rate, duration IS the sample count.
    CHECK(md.duration == 63U * 1536U);
    CHECK(md.language == 0x55C4);  // "und", ISO/IEC 14496-12 §8.4.2.2's packing

    const auto* stts = find(elements, "stts");
    REQUIRE(stts != nullptr);
    const auto [sample_count, sample_delta] = read_stts_first(*file, *stts);
    CHECK(sample_count == 63);
    CHECK(sample_delta == 1536);
}

TEST_CASE("MP4 chunk offsets index mdat exactly", "[mp4]") {
    // Distinct sizes AND distinct content per frame: identical frames could
    // pass this check by accident even with every offset shifted by one
    // frame's width.
    std::vector<Bytes> frames;
    for (std::uint8_t i = 0; i < 5; ++i) {
        frames.push_back(frame_of(100 + static_cast<std::size_t>(i) * 37,
                                  static_cast<std::uint8_t>(i + 1)));
    }
    const auto file = mp4::mux(sample_track(2), frames);
    REQUIRE(file.has_value());
    const auto elements = parse(*file);

    const auto* stsz = find(elements, "stsz");
    const auto* stco = find(elements, "stco");
    REQUIRE(stsz != nullptr);
    REQUIRE(stco != nullptr);
    const auto sizes = read_stsz(*file, *stsz);
    const auto offsets = read_stco(*file, *stco);
    REQUIRE(sizes.size() == frames.size());
    REQUIRE(offsets.size() == frames.size());

    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(sizes[i] == frames[i].size());
        REQUIRE(static_cast<std::uint64_t>(offsets[i]) + sizes[i] <= file->size());
        const std::span<const std::byte> at{file->data() + offsets[i], sizes[i]};
        CHECK(std::equal(at.begin(), at.end(), frames[i].begin(), frames[i].end()));
    }

    // mdat's own declared size must cover every byte stco points into -
    // otherwise a strict reader would refuse the file even though a lax one
    // might still find the right bytes.
    const auto* mdat = find(elements, "mdat");
    REQUIRE(mdat != nullptr);
    CHECK(mdat->payload + mdat->length == file->size());
}

TEST_CASE("MP4 muxer rejects what it cannot describe", "[mp4]") {
    const std::vector<Bytes> one{frame_of(16, 0)};
    const auto track = sample_track();

    // Explicit empty span: bare {} became ambiguous when the span-of-views
    // mux overload arrived alongside the owned-list one.
    CHECK(mp4::mux(track, std::span<const Bytes>{}).error() == mp4::MuxError::kNoFrames);

    auto bad_channels = track;
    bad_channels.channels = 0;
    CHECK(mp4::mux(bad_channels, one).error() == mp4::MuxError::kInvalidTrack);

    auto bad_rate = track;
    bad_rate.sample_rate = 0;
    CHECK(mp4::mux(bad_rate, one).error() == mp4::MuxError::kInvalidTrack);

    auto bad_codec = track;
    bad_codec.codec_id = "mp4a";  // this module only knows ac-3/ec-3
    CHECK(mp4::mux(bad_codec, one).error() == mp4::MuxError::kInvalidTrack);

    auto no_config = track;
    no_config.codec_config.clear();
    CHECK(mp4::mux(no_config, one).error() == mp4::MuxError::kInvalidTrack);
}

// --- ac3::io::build_codec_config_box: the dec3/dac3 payload itself ---------
//
// These read the box's raw bytes directly rather than through mp4::mux(), so
// a bug specific to the box-payload builder (ac3/io/dec3.hpp) cannot hide
// behind the muxer's own wrapping. Real, multi-frame encoded audio throughout
// - never silence, never frame 0 alone - per CONTRIBUTING.md's validation
// discipline: bsid/bsmod/acmod/lfeon/bit_rate_code and the addbsi Atmos
// marker are bitstream fields, and a bit-offset error upstream of them would
// otherwise be invisible against an all-zero frame.

TEST_CASE("dac3 box matches a real AC-3 stream's own bsi", "[dec3]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
        const auto v = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                          static_cast<double>(n) / 48000.0));
        pcm[0][static_cast<std::size_t>(n)] = v;
        pcm[1][static_cast<std::size_t>(n)] = v;
    }
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};

    std::vector<std::byte> stream;
    for (int f = 0; f < 4; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        stream.insert(stream.end(), frame->begin(), frame->end());
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->kind == ac3::io::StreamKind::kAc3);

    const auto payload = ac3::io::build_codec_config_box(*scanned);
    // ETSI TS 102 366 Annex F §F.4: 2+5+3+3+1+5+5 = 24 bits, exactly 3 bytes.
    REQUIRE(payload.size() == 3);

    const auto b0 = byte_at(payload, 0);
    const auto b1 = byte_at(payload, 1);
    const auto b2 = byte_at(payload, 2);
    const auto fscod = static_cast<std::uint32_t>(b0 >> 6);
    const auto bsid = static_cast<std::uint32_t>((b0 >> 1) & 0x1F);
    const auto bsmod = static_cast<std::uint32_t>(((b0 & 0x1) << 2) | (b1 >> 6));
    const auto acmod = static_cast<std::uint32_t>((b1 >> 3) & 0x7);
    const auto lfeon = static_cast<std::uint32_t>((b1 >> 2) & 0x1);
    const auto bit_rate_code = static_cast<std::uint32_t>(((b1 & 0x3) << 3) | (b2 >> 5));
    const auto reserved = static_cast<std::uint32_t>(b2 & 0x1F);

    CHECK(fscod == 0);  // 48 kHz
    CHECK(bsid == 8);   // A/52 base bsid - encoder.cpp always writes it
    CHECK(bsid == static_cast<std::uint32_t>(scanned->bsid));
    CHECK(bsmod == static_cast<std::uint32_t>(scanned->bsmod));
    CHECK(acmod == static_cast<std::uint32_t>(ac3::Acmod::k2_0));
    CHECK(lfeon == 0);
    CHECK(bit_rate_code == static_cast<std::uint32_t>(scanned->bit_rate_code));
    CHECK(reserved == 0);
}

TEST_CASE("dec3 box matches a real E-AC-3 stream with no Atmos extension", "[dec3]") {
    using ac3::eac3::AccessUnitConfig;
    const AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    ac3::eac3::AccessUnitEncoder encoder{config};

    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    constexpr std::array<double, 6> kTones{440.0, 660.0, 880.0, 1100.0, 1320.0, 55.0};
    for (std::size_t ch = 0; ch < 6; ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            pcm[ch][static_cast<std::size_t>(n)] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * kTones[ch] * static_cast<double>(n) /
                              48000.0));
        }
    }
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }

    std::vector<std::byte> stream;
    for (int f = 0; f < 4; ++f) {
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->kind == ac3::io::StreamKind::kEac3);
    CHECK_FALSE(scanned->oba_complexity_index.has_value());

    const auto payload = ac3::io::build_codec_config_box(*scanned);
    // §F.6: 16 (header) + 24 (one substream, no dependents) + 8 (extension,
    // flag clear) = 48 bits = 6 bytes.
    REQUIRE(payload.size() == 6);

    // Byte 2 starts the one independent substream's own fields: fscod(2),
    // bsid(5), reserved(2), bsmod(3), acmod(3), lfeon(1), reserved(3),
    // num_dep_sub(4).
    const auto b2 = byte_at(payload, 2);
    const auto b3 = byte_at(payload, 3);
    const auto b4 = byte_at(payload, 4);
    const auto fscod = static_cast<std::uint32_t>(b2 >> 6);
    const auto bsid = static_cast<std::uint32_t>((b2 >> 1) & 0x1F);
    const auto bsmod = static_cast<std::uint32_t>((b3 >> 4) & 0x7);
    const auto acmod = static_cast<std::uint32_t>((b3 >> 1) & 0x7);
    const auto lfeon = static_cast<std::uint32_t>(b3 & 0x1);
    const auto num_dep_sub = static_cast<std::uint32_t>((b4 >> 1) & 0xF);

    CHECK(fscod == 0);  // 48 kHz family
    CHECK(bsid == static_cast<std::uint32_t>(scanned->bsid));
    CHECK(bsmod == static_cast<std::uint32_t>(scanned->bsmod));
    CHECK(acmod == static_cast<std::uint32_t>(ac3::Acmod::k3_2));
    CHECK(lfeon == 1);
    CHECK(num_dep_sub == 0);

    // The Atmos extension's flag_ec3_extension_type_a bit must read clear.
    CHECK(byte_at(payload, 5) == 0x00);
}

TEST_CASE("dec3 box signals Dolby Atmos objects", "[dec3]") {
    constexpr int kObjects = 3;
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};

    std::vector<std::vector<float>> sources(kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (auto& source : sources) {
        views.emplace_back(source);
    }
    constexpr std::array<double, kObjects> kTones{440.0, 880.0, 1320.0};
    std::array<ac3::oba::ObjectPlacement, kObjects> placement{};
    for (auto& p : placement) {
        p = {.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }

    std::vector<std::byte> stream;
    for (int f = 0; f < 4; ++f) {
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t =
                    static_cast<double>(f * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[obj][static_cast<std::size_t>(n)] = static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * kTones[obj] * t));
            }
        }
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->oba_complexity_index.has_value());
    // §5.6.4.8/§8.3.2.2: object_count is bed-first (this encoder's LFE) then
    // the dynamic objects.
    CHECK(*scanned->oba_complexity_index == kObjects + 1);

    const auto payload = ac3::io::build_codec_config_box(*scanned);
    // 16 (header) + 24 (one substream, no dependents) + 16 (extension, flag
    // set: 7 reserved + 1 flag + 8 complexity_index_type_a) = 56 bits = 7
    // bytes.
    REQUIRE(payload.size() == 7);
    // reserved(7)=0, flag_ec3_extension_type_a=1 packs to 0x01.
    CHECK(byte_at(payload, 5) == 0x01);
    CHECK(byte_at(payload, 6) == static_cast<std::uint8_t>(kObjects + 1));
}
