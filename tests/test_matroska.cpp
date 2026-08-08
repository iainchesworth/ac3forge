#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "matroska/matroska.hpp"

// These tests read the muxer's output back with an independent EBML walker
// rather than comparing against bytes this same code produced. A muxer
// checked only against itself proves nothing about whether a player can
// open the file.

namespace {

using Bytes = std::vector<std::byte>;

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

struct Reader {
    std::span<const std::byte> data;
    std::size_t pos = 0;

    [[nodiscard]] std::uint8_t byte() { return std::to_integer<std::uint8_t>(data[pos++]); }

    // An EBML id keeps its length marker, so read it whole.
    [[nodiscard]] std::uint32_t id() {
        const std::uint8_t first = byte();
        int extra = 0;
        for (int i = 0; i < 4; ++i) {
            if (first & (0x80 >> i)) {
                extra = i;
                break;
            }
        }
        std::uint32_t value = first;
        for (int i = 0; i < extra; ++i) {
            value = (value << 8) | byte();
        }
        return value;
    }

    // A size drops its marker bit and keeps only the value beneath it.
    [[nodiscard]] std::uint64_t size() {
        const std::uint8_t first = byte();
        int width = 1;
        for (int i = 0; i < 8; ++i) {
            if (first & (0x80 >> i)) {
                width = i + 1;
                break;
            }
        }
        std::uint64_t value = first & (0xFFu >> width);
        for (int i = 1; i < width; ++i) {
            value = (value << 8) | byte();
        }
        return value;
    }

    [[nodiscard]] std::uint64_t uint_value(std::uint64_t length) {
        std::uint64_t value = 0;
        for (std::uint64_t i = 0; i < length; ++i) {
            value = (value << 8) | byte();
        }
        return value;
    }

    [[nodiscard]] double float_value(std::uint64_t length) {
        return std::bit_cast<double>(uint_value(length));
    }

    [[nodiscard]] std::string string_value(std::uint64_t length) {
        std::string out;
        for (std::uint64_t i = 0; i < length; ++i) {
            out.push_back(static_cast<char>(byte()));
        }
        return out;
    }
};

// Every element found anywhere in the tree, flattened. Masters are recursed
// into; leaves record their payload offset and length.
struct Element {
    std::uint32_t id = 0;
    std::size_t payload = 0;
    std::uint64_t length = 0;
};

constexpr std::uint32_t kSegment = 0x18538067;
constexpr std::uint32_t kInfo = 0x1549A966;
constexpr std::uint32_t kTracks = 0x1654AE6B;
constexpr std::uint32_t kTrackEntry = 0xAE;
constexpr std::uint32_t kAudio = 0xE1;
constexpr std::uint32_t kCluster = 0x1F43B675;
constexpr std::uint32_t kEbmlHeader = 0x1A45DFA3;

bool is_master(std::uint32_t id) {
    return id == kSegment || id == kInfo || id == kTracks || id == kTrackEntry ||
           id == kAudio || id == kCluster || id == kEbmlHeader;
}

void walk(Reader& r, std::size_t end, std::vector<Element>& out) {
    while (r.pos < end) {
        const auto id = r.id();
        const auto length = r.size();
        const auto payload = r.pos;
        // A size that overruns its parent means the muxer wrote a bad length -
        // exactly the failure this walker exists to catch.
        REQUIRE(payload + length <= end);
        out.push_back({id, payload, length});
        if (is_master(id)) {
            walk(r, payload + static_cast<std::size_t>(length), out);
        } else {
            r.pos = payload + static_cast<std::size_t>(length);
        }
    }
    REQUIRE(r.pos == end);
}

std::vector<Element> parse(std::span<const std::byte> file) {
    Reader r{file};
    std::vector<Element> out;
    walk(r, file.size(), out);
    return out;
}

const Element* find(const std::vector<Element>& elements, std::uint32_t id) {
    for (const auto& e : elements) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

std::size_t count(const std::vector<Element>& elements, std::uint32_t id) {
    std::size_t n = 0;
    for (const auto& e : elements) {
        n += e.id == id ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("Matroska file parses as well-formed EBML", "[matroska]") {
    // Sizes nesting exactly is the whole correctness question for a muxer:
    // one wrong length and a player sees garbage from that point on. walk()
    // REQUIREs every child to fit its parent and every master to end exactly
    // where its size said.
    const std::vector<Bytes> frames{frame_of(1792, 0x11), frame_of(1792, 0x22),
                                    frame_of(1792, 0x33)};
    const auto file = matroska::mux({.channels = 6}, frames);
    REQUIRE(file.has_value());

    const auto elements = parse(*file);
    REQUIRE(find(elements, kEbmlHeader) != nullptr);
    REQUIRE(find(elements, kSegment) != nullptr);
    REQUIRE(find(elements, kInfo) != nullptr);
    REQUIRE(find(elements, kTracks) != nullptr);
    CHECK(count(elements, kTrackEntry) == 1);

    // DocType must say matroska or nothing will open it.
    const auto* doctype = find(elements, 0x4282);
    REQUIRE(doctype != nullptr);
    Reader r{*file, doctype->payload};
    CHECK(r.string_value(doctype->length) == "matroska");
}

TEST_CASE("Matroska track and timing describe the audio", "[matroska]") {
    const std::vector<Bytes> frames(63, frame_of(896, 0xAB));
    const auto file = matroska::mux(
        {.codec_id = std::string{matroska::kCodecEac3}, .sample_rate = 48000,
         .channels = 10, .samples_per_frame = 1536},
        frames);
    REQUIRE(file.has_value());
    const auto elements = parse(*file);

    const auto* codec = find(elements, 0x86);
    REQUIRE(codec != nullptr);
    Reader cr{*file, codec->payload};
    CHECK(cr.string_value(codec->length) == "A_EAC3");

    const auto* channels = find(elements, 0x9F);
    REQUIRE(channels != nullptr);
    Reader chr{*file, channels->payload};
    CHECK(chr.uint_value(channels->length) == 10);

    const auto* rate = find(elements, 0xB5);
    REQUIRE(rate != nullptr);
    Reader rr{*file, rate->payload};
    CHECK(rr.float_value(rate->length) == 48000.0);

    // TimestampScale is nanoseconds per tick; Duration is in ticks and is a
    // float, not an integer. 63 frames of 1536 at 48 kHz is 2016 ms.
    const auto* scale = find(elements, 0x2AD7B1);
    REQUIRE(scale != nullptr);
    Reader sr{*file, scale->payload};
    CHECK(sr.uint_value(scale->length) == 1'000'000);

    const auto* duration = find(elements, 0x4489);
    REQUIRE(duration != nullptr);
    Reader dr{*file, duration->payload};
    CHECK(dr.float_value(duration->length) == 2016.0);
}

TEST_CASE("Matroska clusters keep block timestamps inside int16", "[matroska]") {
    // A SimpleBlock's timestamp is signed 16-bit and relative to its cluster,
    // so a cluster can never span more than 32767 ms no matter what budget the
    // caller asks for. Demand a wildly oversized one and check it is ignored.
    const std::vector<Bytes> frames(2000, frame_of(64, 0x5A));  // 64 seconds
    const auto file =
        matroska::mux({.channels = 2}, frames, {.cluster_ms = 10'000'000});
    REQUIRE(file.has_value());
    const auto elements = parse(*file);

    // Every frame is carried, one SimpleBlock each.
    CHECK(count(elements, 0xA3) == frames.size());
    CHECK(count(elements, kCluster) > 1);

    for (const auto& e : elements) {
        if (e.id != 0xA3) {
            continue;
        }
        // track number vint (1 byte here), then the int16 relative timestamp.
        Reader br{*file, e.payload + 1};
        const auto hi = static_cast<std::int16_t>(br.byte());
        const auto lo = static_cast<std::int16_t>(br.byte());
        const auto relative = static_cast<std::int16_t>((hi << 8) | lo);
        CHECK(relative >= 0);
    }
}

TEST_CASE("Matroska muxer rejects what it cannot describe", "[matroska]") {
    const std::vector<Bytes> one{frame_of(16, 0)};
    CHECK(matroska::mux({.channels = 2}, {}).error() == matroska::MuxError::kNoFrames);
    CHECK(matroska::mux({.channels = 0}, one).error() ==
          matroska::MuxError::kInvalidTrack);
    CHECK(matroska::mux({.sample_rate = 0, .channels = 2}, one).error() ==
          matroska::MuxError::kInvalidTrack);
    CHECK(matroska::mux({.codec_id = "", .channels = 2}, one).error() ==
          matroska::MuxError::kInvalidTrack);
}
