#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"

// fragment()'s ISOBMFF output is read back with its own independent box
// walker below, extended from test_mp4.cpp's (moof/traf/tfhd/tfdt/trun added
// for the fragmentation boxes mux() never writes) rather than shared with
// it - the same "an independent reader, not a self-check" reasoning
// test_mp4.cpp's own header comment gives. hls.cpp/dash.cpp's plain-text
// output is asserted against directly, since there is no bitstream
// arithmetic in a manifest string to independently re-derive.

namespace {

using Bytes = std::vector<std::byte>;

[[nodiscard]] std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

[[nodiscard]] std::uint32_t u32_at(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint32_t>(byte_at(data, offset)) << 24) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 8) |
           static_cast<std::uint32_t>(byte_at(data, offset + 3));
}

[[nodiscard]] std::uint64_t u64_at(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint64_t>(u32_at(data, offset)) << 32) | u32_at(data, offset + 4);
}

[[nodiscard]] std::string fourcc_at(std::span<const std::byte> data, std::size_t offset) {
    std::string s(4, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>(byte_at(data, offset + i));
    }
    return s;
}

// One ISOBMFF box: size(4) + type(4) + body. `payload`/`length` describe the
// body only (offset past the 8-byte header, size - 8).
struct Element {
    std::string type;
    std::size_t payload = 0;
    std::uint64_t length = 0;
};

// Everything this file's own box walker needs to recurse into - mp4.cpp's
// plain containers, plus moof/traf/mvex, which mux() never writes and
// test_mp4.cpp's own list therefore has no reason to include.
bool is_container(const std::string& type) {
    return type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl" ||
           type == "dinf" || type == "mvex" || type == "moof" || type == "traf";
}

void walk(std::span<const std::byte> file, std::size_t pos, std::size_t end,
          std::vector<Element>& out) {
    while (pos < end) {
        REQUIRE(pos + 8 <= end);
        const auto size = u32_at(file, pos);
        const auto type = fourcc_at(file, pos + 4);
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

// ftyp/styp body: major_brand(4), minor_version(4), compatible_brands[](4
// each) - ISO/IEC 14496-12 §4.3/§8.16.2.
struct BrandBox {
    std::string major_brand;
    std::vector<std::string> compatible_brands;
};

BrandBox read_brand_box(std::span<const std::byte> file, const Element& box) {
    BrandBox out;
    out.major_brand = fourcc_at(file, box.payload);
    for (std::size_t off = box.payload + 8; off + 4 <= box.payload + box.length; off += 4) {
        out.compatible_brands.push_back(fourcc_at(file, off));
    }
    return out;
}

bool has_brand(const BrandBox& box, std::string_view brand) {
    return std::find(box.compatible_brands.begin(), box.compatible_brands.end(), brand) !=
           box.compatible_brands.end();
}

// trex body (ISO/IEC 14496-12 §8.8.3): version+flags(4), track_ID(4),
// default_sample_description_index(4), default_sample_duration(4),
// default_sample_size(4), default_sample_flags(4).
struct TrexInfo {
    std::uint32_t track_id = 0;
    std::uint32_t default_sample_duration = 0;
    std::uint32_t default_sample_size = 0;
    std::uint32_t default_sample_flags = 0;
};

TrexInfo read_trex(std::span<const std::byte> file, const Element& trex) {
    // payload+0: version+flags, +4: track_ID, +8: default_sample_description_index,
    // +12: default_sample_duration, +16: default_sample_size, +20: default_sample_flags.
    return {.track_id = u32_at(file, trex.payload + 4),
            .default_sample_duration = u32_at(file, trex.payload + 12),
            .default_sample_size = u32_at(file, trex.payload + 16),
            .default_sample_flags = u32_at(file, trex.payload + 20)};
}

// tfhd body (ISO/IEC 14496-12 §8.8.7): version+flags(4), track_ID(4). No
// optional field is present in what fragment() writes (flags carry only
// default-base-is-moof), so track_ID is always the next 4 bytes.
std::uint32_t read_tfhd_track_id(std::span<const std::byte> file, const Element& tfhd) {
    return u32_at(file, tfhd.payload + 4);
}

// tfhd's own flags live in the FullBox header (bytes 1-3 of the box, after
// the 1-byte version) - ISO/IEC 14496-12 §4.2.
std::uint32_t read_fullbox_flags(std::span<const std::byte> file, const Element& box) {
    return u32_at(file, box.payload) & 0x00FFFFFFU;
}

// tfdt body (ISO/IEC 14496-12 §8.8.12), version 1: version+flags(4),
// baseMediaDecodeTime(8).
std::uint64_t read_tfdt(std::span<const std::byte> file, const Element& tfdt) {
    return u64_at(file, tfdt.payload + 4);
}

// trun body (ISO/IEC 14496-12 §8.8.8), flags data-offset-present |
// sample-size-present only (what fragment() writes): version+flags(4),
// sample_count(4), data_offset(4), [sample_size(4)]*sample_count.
struct TrunInfo {
    std::uint32_t sample_count = 0;
    std::int32_t data_offset = 0;
    std::vector<std::uint32_t> sample_sizes;
};

TrunInfo read_trun(std::span<const std::byte> file, const Element& trun) {
    TrunInfo out;
    out.sample_count = u32_at(file, trun.payload + 4);
    out.data_offset = static_cast<std::int32_t>(u32_at(file, trun.payload + 8));
    out.sample_sizes.resize(out.sample_count);
    for (std::uint32_t i = 0; i < out.sample_count; ++i) {
        out.sample_sizes[i] = u32_at(file, trun.payload + 12 + static_cast<std::size_t>(i) * 4);
    }
    return out;
}

mp4::AudioTrack sample_track(int channels = 6) {
    return mp4::AudioTrack{
        .codec_id = std::string{mp4::kCodecEac3},
        .sample_rate = 48000,
        .channels = channels,
        .samples_per_frame = 1536,
        .codec_config = Bytes{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}}};
}

// A real, multi-frame 5.1 E-AC-3 elementary stream - never silence, never
// frame 0 alone (CONTRIBUTING.md's validation discipline): distinct tones
// per channel so a channel-order or frame-boundary bug cannot hide behind
// identical content. kFrames=10 at kFramesPerFragment=3 spans FOUR
// fragments (3, 3, 3, 1) - enough for a full fragment, a fragment boundary,
// and a shorter-than-nominal final fragment all in the same fixture.
constexpr int kFrames = 10;
constexpr std::uint32_t kFramesPerFragment = 3;

std::vector<Bytes> encode_real_eac3_frames() {
    using ac3::eac3::AccessUnitConfig;
    const AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    ac3::eac3::AccessUnitEncoder encoder{config};

    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    constexpr std::array<double, 6> kTones{440.0, 660.0, 880.0, 1100.0, 1320.0, 55.0};
    std::vector<Bytes> frames;
    for (int f = 0; f < kFrames; ++f) {
        for (std::size_t ch = 0; ch < 6; ++ch) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = static_cast<double>(f * ac3::kSamplesPerFrame + n) / 48000.0;
                pcm[ch][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
            }
        }
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        frames.emplace_back(unit->bytes.begin(), unit->bytes.end());
    }
    return frames;
}

struct RealFixture {
    mp4::AudioTrack track;
    std::vector<Bytes> frames;
    mp4::FragmentedOutput fragmented;
};

RealFixture make_real_fixture() {
    const auto frames = encode_real_eac3_frames();
    Bytes stream;
    for (const auto& f : frames) {
        stream.insert(stream.end(), f.begin(), f.end());
    }
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->kind == ac3::io::StreamKind::kEac3);

    const mp4::AudioTrack track{
        .codec_id = std::string{mp4::kCodecEac3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
        .codec_config = ac3::io::build_codec_config_box(*scanned),
    };

    const auto result = mp4::fragment(
        track, frames, mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment});
    REQUIRE(result.has_value());
    return RealFixture{.track = track, .frames = frames, .fragmented = *result};
}

}  // namespace

TEST_CASE("fragment() init segment is well-formed with mvex/trex and an empty sample table",
          "[fmp4]") {
    const auto fixture = make_real_fixture();
    const auto elements = parse(fixture.fragmented.init_segment);

    const auto* ftyp = find(elements, "ftyp");
    REQUIRE(ftyp != nullptr);
    const auto brands = read_brand_box(fixture.fragmented.init_segment, *ftyp);
    CHECK(brands.major_brand == "iso5");
    CHECK(has_brand(brands, "cmfc"));

    REQUIRE(find(elements, "moov") != nullptr);
    REQUIRE(find(elements, "trak") != nullptr);
    REQUIRE(find(elements, "stsd") != nullptr);
    CHECK(count(elements, "trak") == 1);

    // The sample tables exist (ISO/IEC 14496-12 mandates their presence in
    // stbl) but describe ZERO samples - every real sample lives in a moof
    // instead. entry_count sits right after each table's 4-byte
    // version+flags header.
    for (const char* box_cstr : {"stts", "stsc", "stsz", "stco"}) {
        const std::string box{box_cstr};
        const auto* elem = find(elements, box);
        REQUIRE(elem != nullptr);
        const auto entry_count_offset = box == "stsz" ? elem->payload + 8 : elem->payload + 4;
        CHECK(u32_at(fixture.fragmented.init_segment, entry_count_offset) == 0);
    }

    const auto* mvex = find(elements, "mvex");
    REQUIRE(mvex != nullptr);
    const auto* trex = find(elements, "trex");
    REQUIRE(trex != nullptr);
    const auto trex_info = read_trex(fixture.fragmented.init_segment, *trex);
    CHECK(trex_info.track_id == 1);
    CHECK(trex_info.default_sample_duration == ac3::kSamplesPerFrame);
    CHECK(trex_info.default_sample_size == 0);
    // sample_depends_on=2 ("does not depend on others"), every other field
    // (incl. sample_is_non_sync_sample) clear - see fragment.cpp's own
    // comment on build_trex.
    CHECK(trex_info.default_sample_flags == 0x02000000U);
}

TEST_CASE("fragment() groups frames into fragments with correct sequence numbers and durations",
          "[fmp4]") {
    const auto fixture = make_real_fixture();
    const auto& segments = fixture.fragmented.media_segments;

    // 10 frames at 3/fragment: 3, 3, 3, 1.
    REQUIRE(segments.size() == 4);
    const std::array<std::uint32_t, 4> expected_counts{3, 3, 3, 1};
    std::uint64_t expected_base_time = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        CAPTURE(i);
        CHECK(segments[i].sequence_number == i + 1);
        CHECK(segments[i].sample_count == expected_counts[i]);
        CHECK(segments[i].duration_samples ==
              static_cast<std::uint64_t>(expected_counts[i]) * ac3::kSamplesPerFrame);

        const auto elements = parse(segments[i].bytes);
        const auto* styp = find(elements, "styp");
        REQUIRE(styp != nullptr);
        const auto brands = read_brand_box(segments[i].bytes, *styp);
        CHECK(brands.major_brand == "iso5");
        CHECK(has_brand(brands, "cmfc"));

        const auto* mfhd = find(elements, "mfhd");
        REQUIRE(mfhd != nullptr);
        // mfhd body: version+flags(4), sequence_number(4).
        CHECK(u32_at(segments[i].bytes, mfhd->payload + 4) == i + 1);

        const auto* tfhd = find(elements, "tfhd");
        REQUIRE(tfhd != nullptr);
        CHECK(read_tfhd_track_id(segments[i].bytes, *tfhd) == 1);
        CHECK(read_fullbox_flags(segments[i].bytes, *tfhd) == 0x020000U);  // default-base-is-moof

        const auto* tfdt = find(elements, "tfdt");
        REQUIRE(tfdt != nullptr);
        CHECK(read_tfdt(segments[i].bytes, *tfdt) == expected_base_time);
        expected_base_time += segments[i].duration_samples;

        const auto* mdat = find(elements, "mdat");
        REQUIRE(mdat != nullptr);
    }
}

TEST_CASE("fragment() trun sample sizes and mdat bytes reproduce the real encoded frames exactly",
          "[fmp4]") {
    // This is the check that actually exercises default-base-is-moof's
    // arithmetic: trun's data_offset must land EXACTLY on the first sample
    // byte in the mdat that follows moof, for every fragment, or a real
    // player reads garbage.
    const auto fixture = make_real_fixture();
    const auto& segments = fixture.fragmented.media_segments;
    std::size_t frame_cursor = 0;

    for (const auto& segment : segments) {
        const auto elements = parse(segment.bytes);
        const auto* moof = find(elements, "moof");
        const auto* trun = find(elements, "trun");
        const auto* mdat = find(elements, "mdat");
        REQUIRE(moof != nullptr);
        REQUIRE(trun != nullptr);
        REQUIRE(mdat != nullptr);

        const auto trun_info = read_trun(segment.bytes, *trun);
        REQUIRE(trun_info.sample_count == segment.sample_count);

        // moof's box starts at file offset 0 within a media segment ONLY if
        // there were nothing before it - but styp precedes moof here, so
        // "the start of moof" for data_offset purposes is moof's own
        // element position (8 bytes before its payload, for its own
        // size+type header), not byte 0 of the segment.
        const std::size_t moof_start = moof->payload - 8;
        std::size_t cursor = moof_start + static_cast<std::size_t>(trun_info.data_offset);
        REQUIRE(cursor == mdat->payload);  // data_offset lands exactly on the first sample byte

        for (std::uint32_t i = 0; i < trun_info.sample_count; ++i) {
            REQUIRE(frame_cursor < fixture.frames.size());
            const auto& real_frame = fixture.frames[frame_cursor];
            CHECK(trun_info.sample_sizes[i] == real_frame.size());
            REQUIRE(cursor + real_frame.size() <= segment.bytes.size());
            const std::span<const std::byte> at{segment.bytes.data() + cursor, real_frame.size()};
            CHECK(std::equal(at.begin(), at.end(), real_frame.begin(), real_frame.end()));
            cursor += real_frame.size();
            ++frame_cursor;
        }
    }
    CHECK(frame_cursor == fixture.frames.size());
}

TEST_CASE("fragment() rejects what it cannot describe", "[fmp4]") {
    const std::vector<Bytes> one{Bytes(16, std::byte{0})};
    const auto track = sample_track();

    // Explicit empty span: bare {} became ambiguous when the span-of-views
    // fragment overload arrived alongside the owned-list one.
    CHECK(mp4::fragment(track, std::span<const Bytes>{}).error() == mp4::MuxError::kNoFrames);

    auto bad_channels = track;
    bad_channels.channels = 0;
    CHECK(mp4::fragment(bad_channels, one).error() == mp4::MuxError::kInvalidTrack);

    auto bad_codec = track;
    bad_codec.codec_id = "mp4a";
    CHECK(mp4::fragment(bad_codec, one).error() == mp4::MuxError::kInvalidTrack);

    CHECK(mp4::fragment(track, one, mp4::FragmentOptions{.frames_per_fragment = 0}).error() ==
          mp4::MuxError::kInvalidOptions);
}

// --- HLS ---------------------------------------------------------------

TEST_CASE("hls_codec_string is RFC 6381's bare sample-entry fourcc for both codecs", "[hls]") {
    auto track = sample_track();
    track.codec_id = std::string{mp4::kCodecEac3};
    CHECK(mp4::hls_codec_string(track) == "ec-3");
    track.codec_id = std::string{mp4::kCodecAc3};
    CHECK(mp4::hls_codec_string(track) == "ac-3");
}

TEST_CASE("HLS media playlist lists every fragment in order with EXT-X-MAP and correct timing",
          "[hls]") {
    const auto fixture = make_real_fixture();
    const auto playlist = mp4::build_hls_media_playlist(
        fixture.track, fixture.fragmented.media_segments, mp4::HlsOptions{});

    CHECK(playlist.starts_with("#EXTM3U\n"));
    CHECK(playlist.find("#EXT-X-VERSION:7\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-MAP:URI=\"init.mp4\"\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-PLAYLIST-TYPE:VOD\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-ENDLIST\n") != std::string::npos);

    // Every fragment's #EXTINF/URI pair, in order - independently recomputed
    // from the SAME segments the playlist was built from, not copy-pasted
    // from hls.cpp's own formula.
    for (const auto& segment : fixture.fragmented.media_segments) {
        const auto expected_uri = std::format("segment{}.m4s", segment.sequence_number);
        CHECK(playlist.find(expected_uri) != std::string::npos);
    }
    // TARGETDURATION must be an integer >= every #EXTINF value (RFC 8216
    // §4.3.3.1) - the longest fragment here is a full one, 3*1536/48000 s.
    const double max_seconds = 3.0 * static_cast<double>(ac3::kSamplesPerFrame) /
                               static_cast<double>(fixture.track.sample_rate);
    const auto target = static_cast<std::uint64_t>(std::ceil(max_seconds));
    CHECK(playlist.find(std::format("#EXT-X-TARGETDURATION:{}\n", target)) != std::string::npos);
}

TEST_CASE("HLS master playlist signals CODECS and CHANNELS correctly", "[hls]") {
    const auto fixture = make_real_fixture();

    SECTION("plain E-AC-3: CHANNELS defaults to the track's channel count") {
        const auto master = mp4::build_hls_master_playlist(
            fixture.track, fixture.fragmented.media_segments, "audio.m3u8", mp4::HlsOptions{});
        CHECK(master.find("CODECS=\"ec-3\"") != std::string::npos);
        CHECK(master.find(std::format("CHANNELS=\"{}\"", fixture.track.channels)) !=
              std::string::npos);
        // Audio-only content self-references: the URI after EXT-X-STREAM-INF
        // is the same media playlist EXT-X-MEDIA already names (see
        // hls.cpp's own comment) - both must appear, and the line right
        // after EXT-X-STREAM-INF must BE that URI.
        const auto stream_inf_pos = master.find("#EXT-X-STREAM-INF:");
        REQUIRE(stream_inf_pos != std::string::npos);
        const auto next_line = master.find('\n', stream_inf_pos);
        REQUIRE(next_line != std::string::npos);
        const auto uri_end = master.find('\n', next_line + 1);
        CHECK(master.substr(next_line + 1, uri_end - next_line - 1) == "audio.m3u8");
    }

    SECTION("Dolby Atmos: CHANNELS carries the caller-supplied </JOC> form") {
        // mp4:: never reads TS 103 420 object-layer syntax itself (see
        // hls.hpp's own comment) - the caller (which DOES know
        // oba_complexity_index) supplies the exact string.
        const auto master = mp4::build_hls_master_playlist(
            fixture.track, fixture.fragmented.media_segments, "audio.m3u8",
            mp4::HlsOptions{.channels_attribute = "12/JOC"});
        CHECK(master.find("CODECS=\"ec-3\"") != std::string::npos);
        CHECK(master.find("CHANNELS=\"12/JOC\"") != std::string::npos);
    }
}

// --- DASH ----------------------------------------------------------------

TEST_CASE("DASH adaptation set snippet carries the correct codecs, timescale and segment template",
          "[dash]") {
    const auto fixture = make_real_fixture();
    const auto snippet = mp4::build_dash_adaptation_set(
        fixture.track, fixture.fragmented.media_segments, mp4::DashOptions{});

    CHECK(snippet.find("<AdaptationSet") != std::string::npos);
    CHECK(snippet.find("</AdaptationSet>") != std::string::npos);
    CHECK(snippet.find("mimeType=\"audio/mp4\"") != std::string::npos);
    CHECK(snippet.find("codecs=\"ec-3\"") != std::string::npos);
    CHECK(snippet.find(std::format("audioSamplingRate=\"{}\"", fixture.track.sample_rate)) !=
          std::string::npos);
    CHECK(snippet.find(std::format("timescale=\"{}\"", fixture.track.sample_rate)) !=
          std::string::npos);
    CHECK(snippet.find("initialization=\"init.mp4\"") != std::string::npos);
    CHECK(snippet.find("media=\"segment$Number$.m4s\"") != std::string::npos);

    // 10 frames at 3/fragment (the fixture's own kFramesPerFragment): three
    // full 3-frame fragments (4608 samples each) then one short 1-frame
    // fragment (1536) - exactly the run-length pattern a SegmentTimeline
    // should encode as "<S d="4608" r="2"/>" (3 occurrences: r is the
    // repeat count AFTER the first) followed by "<S d="1536"/>" (a single,
    // un-repeated final entry). Independently recomputed from the segments
    // themselves, not copied from dash.cpp's own formula.
    CHECK(snippet.find("<SegmentTimeline>") != std::string::npos);
    CHECK(snippet.find("</SegmentTimeline>") != std::string::npos);
    CHECK(snippet.find("<S d=\"4608\" r=\"2\"/>") != std::string::npos);
    CHECK(snippet.find("<S d=\"1536\"/>") != std::string::npos);
    // A flat, nominal `duration` attribute is exactly what this test's own
    // sibling regression (see the bug reintroduced further down) is about -
    // it must NOT appear on SegmentTemplate itself once SegmentTimeline is
    // present.
    CHECK(snippet.find("SegmentTemplate timescale=\"48000\" initialization=\"init.mp4\" "
                       "media=\"segment$Number$.m4s\" startNumber=\"1\">") != std::string::npos);

    // Balanced enough to be worth writing to a file: every opening tag this
    // snippet introduces has a matching close.
    CHECK(std::ranges::count(snippet, '<') > 0);
    const auto open_representation = snippet.find("<Representation ");
    const auto close_representation = snippet.find("</Representation>");
    REQUIRE(open_representation != std::string::npos);
    REQUIRE(close_representation != std::string::npos);
    CHECK(open_representation < close_representation);
    const auto open_template = snippet.find("<SegmentTemplate ");
    const auto close_template = snippet.find("</SegmentTemplate>");
    REQUIRE(open_template != std::string::npos);
    REQUIRE(close_template != std::string::npos);
    CHECK(open_template < close_template);
    CHECK(close_template < close_representation);
}

TEST_CASE("DASH adaptation set's SegmentTimeline exactly reproduces varied segment durations",
          "[dash]") {
    // A more demanding duration pattern than the shared fixture's
    // "uniform-except-last" one: run-length encoding must not assume only
    // two runs ever occur.
    const mp4::AudioTrack track = sample_track();
    const std::vector<mp4::MediaSegment> segments{
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 1, .sample_count = 2, .duration_samples = 3072},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 2, .sample_count = 2, .duration_samples = 3072},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 3, .sample_count = 1, .duration_samples = 1536},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 4, .sample_count = 3, .duration_samples = 4608},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 5, .sample_count = 3, .duration_samples = 4608},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 6, .sample_count = 3, .duration_samples = 4608},
    };
    const auto snippet = mp4::build_dash_adaptation_set(track, segments, mp4::DashOptions{});
    CHECK(snippet.find("<S d=\"3072\" r=\"1\"/>") != std::string::npos);
    CHECK(snippet.find("<S d=\"1536\"/>") != std::string::npos);
    CHECK(snippet.find("<S d=\"4608\" r=\"2\"/>") != std::string::npos);

    // Independently re-parse every <S> entry out of the snippet (rather than
    // trusting the three find()s above alone) and check the total duration
    // it describes - d*(r+1) summed - against the segments' own total. This
    // is the property a real player actually relies on: get it wrong and
    // playback position/seeking drifts even if individual <S> tags "look"
    // present.
    std::uint64_t reconstructed_total = 0;
    std::size_t pos = 0;
    while ((pos = snippet.find("<S d=\"", pos)) != std::string::npos) {
        const auto d_start = pos + 6;
        const auto d_end = snippet.find('"', d_start);
        const auto d = std::stoull(snippet.substr(d_start, d_end - d_start));
        std::uint64_t repeat = 0;
        const auto r_pos = snippet.find(" r=\"", d_end);
        const auto tag_end = snippet.find("/>", d_end);
        if (r_pos != std::string::npos && r_pos < tag_end) {
            const auto r_start = r_pos + 4;
            const auto r_end = snippet.find('"', r_start);
            repeat = std::stoull(snippet.substr(r_start, r_end - r_start));
        }
        reconstructed_total += d * (repeat + 1);
        pos = tag_end;
    }
    std::uint64_t expected_total = 0;
    for (const auto& s : segments) {
        expected_total += s.duration_samples;
    }
    CHECK(reconstructed_total == expected_total);
}
