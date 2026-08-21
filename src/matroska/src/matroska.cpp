#include "matroska/matroska.hpp"

#include <bit>
#include <limits>

namespace matroska {

namespace {

// --- EBML element ids ------------------------------------------------------
// Ids are written exactly as listed: an id already carries its own length
// marker in the leading bits, so unlike a size it is never re-encoded.
constexpr std::uint32_t kEbmlHeader = 0x1A45DFA3;
constexpr std::uint32_t kEbmlVersion = 0x4286;
constexpr std::uint32_t kEbmlReadVersion = 0x42F7;
constexpr std::uint32_t kEbmlMaxIdLength = 0x42F2;
constexpr std::uint32_t kEbmlMaxSizeLength = 0x42F3;
constexpr std::uint32_t kDocType = 0x4282;
constexpr std::uint32_t kDocTypeVersion = 0x4287;
constexpr std::uint32_t kDocTypeReadVersion = 0x4285;

constexpr std::uint32_t kSegment = 0x18538067;
constexpr std::uint32_t kInfo = 0x1549A966;
constexpr std::uint32_t kTimestampScale = 0x2AD7B1;
constexpr std::uint32_t kDuration = 0x4489;
constexpr std::uint32_t kMuxingApp = 0x4D80;
constexpr std::uint32_t kWritingApp = 0x5741;

constexpr std::uint32_t kTracks = 0x1654AE6B;
constexpr std::uint32_t kTrackEntry = 0xAE;
constexpr std::uint32_t kTrackNumber = 0xD7;
constexpr std::uint32_t kTrackUid = 0x73C5;
constexpr std::uint32_t kTrackType = 0x83;
constexpr std::uint32_t kFlagLacing = 0x9C;
constexpr std::uint32_t kCodecId = 0x86;
constexpr std::uint32_t kLanguage = 0x22B59C;
constexpr std::uint32_t kAudio = 0xE1;
constexpr std::uint32_t kSamplingFrequency = 0xB5;
constexpr std::uint32_t kChannels = 0x9F;

constexpr std::uint32_t kCluster = 0x1F43B675;
constexpr std::uint32_t kClusterTimestamp = 0xE7;
constexpr std::uint32_t kSimpleBlock = 0xA3;

constexpr std::uint8_t kTrackTypeAudio = 0x02;
constexpr std::uint64_t kTimestampScaleNs = 1'000'000;  // one tick == 1 ms

using Bytes = std::vector<std::byte>;

void put_byte(Bytes& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

// An id's width is implied by its most significant set bit, so write only the
// bytes it actually occupies.
void put_id(Bytes& out, std::uint32_t id) {
    const int width = id <= 0xFF ? 1 : id <= 0xFFFF ? 2 : id <= 0xFF'FFFF ? 3 : 4;
    for (int i = width - 1; i >= 0; --i) {
        put_byte(out, static_cast<std::uint8_t>(id >> (8 * i)));
    }
}

// EBML variable-length integer: a leading-zero run picks the width, then a
// marker bit, then the value. Written at the narrowest width that fits, except
// where a caller pins the width to reserve space.
void put_vint(Bytes& out, std::uint64_t value, int width = 0) {
    if (width == 0) {
        width = 1;
        // A value of all-ones at a given width is reserved for "unknown", so
        // it cannot be used as a length - step up a byte when we hit it.
        while (width < 8 && value >= (1ULL << (7 * width)) - 1) {
            ++width;
        }
    }
    const auto marker = static_cast<std::uint64_t>(1) << (7 * width);
    const std::uint64_t encoded = value | marker;
    for (int i = width - 1; i >= 0; --i) {
        put_byte(out, static_cast<std::uint8_t>(encoded >> (8 * i)));
    }
}

// Big-endian unsigned, trimmed to its significant bytes (EBML's "uint").
void put_uint_element(Bytes& out, std::uint32_t id, std::uint64_t value) {
    int width = 1;
    while (width < 8 && value >> (8 * width)) {
        ++width;
    }
    put_id(out, id);
    put_vint(out, static_cast<std::uint64_t>(width));
    for (int i = width - 1; i >= 0; --i) {
        put_byte(out, static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void put_float_element(Bytes& out, std::uint32_t id, double value) {
    put_id(out, id);
    put_vint(out, 8);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    for (int i = 7; i >= 0; --i) {
        put_byte(out, static_cast<std::uint8_t>(bits >> (8 * i)));
    }
}

void put_string_element(Bytes& out, std::uint32_t id, std::string_view value) {
    put_id(out, id);
    put_vint(out, value.size());
    for (const char c : value) {
        put_byte(out, static_cast<std::uint8_t>(c));
    }
}

// A parent element whose size is only known once its children are written.
// Building children into their own buffer keeps every size a known length
// rather than the "unknown size" form, which not every player tolerates.
void put_master(Bytes& out, std::uint32_t id, const Bytes& children) {
    put_id(out, id);
    put_vint(out, children.size());
    out.insert(out.end(), children.begin(), children.end());
}

Bytes build_ebml_header() {
    Bytes h;
    put_uint_element(h, kEbmlVersion, 1);
    put_uint_element(h, kEbmlReadVersion, 1);
    put_uint_element(h, kEbmlMaxIdLength, 4);
    put_uint_element(h, kEbmlMaxSizeLength, 8);
    put_string_element(h, kDocType, "matroska");
    put_uint_element(h, kDocTypeVersion, 4);
    put_uint_element(h, kDocTypeReadVersion, 2);
    return h;
}

Bytes build_info(const MuxOptions& options, std::uint64_t duration_ms) {
    Bytes info;
    put_uint_element(info, kTimestampScale, kTimestampScaleNs);
    // Duration is in TimestampScale units and is a float, not an integer.
    put_float_element(info, kDuration, static_cast<double>(duration_ms));
    put_string_element(info, kMuxingApp, options.writing_app);
    put_string_element(info, kWritingApp, options.writing_app);
    return info;
}

// EBML's reserved "unknown size": a size vint whose value bits are ALL set,
// at the widest width (8 bytes) this file ever writes a vint at - the
// standard way a streamed element declares a length it cannot know yet (used
// by Writer's Segment below). Reserving this exact pattern is what stops
// put_vint's own automatic width-stepping from ever emitting it by accident
// for a real size: the loop in put_vint steps to a wider width the moment a
// real value would collide with it.
constexpr std::uint64_t kUnknownSize = (std::uint64_t{1} << 56) - 1;

void put_master_unknown_size(Bytes& out, std::uint32_t id) {
    put_id(out, id);
    put_vint(out, kUnknownSize, 8);
}

// Same as build_info, minus Duration: a caller writing incrementally (see
// matroska::Writer) does not know the session's length until it decides to
// stop, so there is nothing honest to put there yet. Kept as its own
// function rather than a bool on build_info - every existing caller of that
// one DOES know its duration, and a parameter only one of two callers would
// ever pass true is worse than two small functions that each say what they
// write.
Bytes build_info_streaming(const MuxOptions& options) {
    Bytes info;
    put_uint_element(info, kTimestampScale, kTimestampScaleNs);
    put_string_element(info, kMuxingApp, options.writing_app);
    put_string_element(info, kWritingApp, options.writing_app);
    return info;
}

Bytes build_tracks(const AudioTrack& track) {
    Bytes audio;
    put_float_element(audio, kSamplingFrequency, static_cast<double>(track.sample_rate));
    put_uint_element(audio, kChannels, static_cast<std::uint64_t>(track.channels));

    Bytes entry;
    put_uint_element(entry, kTrackNumber, 1);
    put_uint_element(entry, kTrackUid, 1);
    put_uint_element(entry, kTrackType, kTrackTypeAudio);
    // Every frame is its own SimpleBlock, so lacing is off. Saying so keeps a
    // demuxer from looking for lacing headers that are not there.
    put_uint_element(entry, kFlagLacing, 0);
    put_string_element(entry, kCodecId, track.codec_id);
    put_string_element(entry, kLanguage, track.language);
    put_master(entry, kAudio, audio);

    Bytes tracks;
    put_master(tracks, kTrackEntry, entry);
    return tracks;
}

}  // namespace

std::string_view describe(MuxError error) {
    switch (error) {
        case MuxError::kNoFrames:
            return "no frames to mux";
        case MuxError::kInvalidTrack:
            return "invalid track: channels, sample rate and codec id are required";
        case MuxError::kFrameTooLarge:
            return "frame too large for one SimpleBlock";
    }
    return "unknown error";
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (track.channels <= 0 || track.sample_rate == 0 || track.codec_id.empty() ||
        track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }

    // Timestamps run off the CUMULATIVE sample count rather than a per-frame
    // increment, so a frame duration that is not a whole number of
    // milliseconds - 1536 samples at 44.1 kHz is 34.83 - rounds without the
    // error ever accumulating.
    const auto stamp_ms = [&](std::size_t index) {
        return static_cast<std::uint64_t>(index) * track.samples_per_frame * 1000 /
               track.sample_rate;
    };
    const std::uint64_t duration_ms = stamp_ms(frames.size());

    Bytes clusters;
    std::size_t index = 0;
    while (index < frames.size()) {
        const std::uint64_t base_ms = stamp_ms(index);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, base_ms);
        // Close the cluster on the time budget, but never let a block's
        // relative timestamp leave int16 range whatever the caller asked for.
        while (index < frames.size()) {
            const std::uint64_t delta = stamp_ms(index) - base_ms;
            const bool first = cluster.empty() || stamp_ms(index) == base_ms;
            if (!first && (delta >= options.cluster_ms ||
                           delta > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int16_t>::max()))) {
                break;
            }
            const auto& frame = frames[index];
            if (frame.size() > (1ULL << 40)) {
                return std::unexpected(MuxError::kFrameTooLarge);
            }
            // SimpleBlock: track number as a vint, a signed 16-bit timestamp
            // relative to the cluster, one flags byte, then the frame. Bit 7
            // of the flags marks a keyframe, which every audio frame is.
            Bytes block;
            put_vint(block, 1);
            const auto relative = static_cast<std::int16_t>(delta);
            put_byte(block, static_cast<std::uint8_t>(relative >> 8));
            put_byte(block, static_cast<std::uint8_t>(relative & 0xFF));
            put_byte(block, 0x80);
            block.insert(block.end(), frame.begin(), frame.end());

            put_id(cluster, kSimpleBlock);
            put_vint(cluster, block.size());
            cluster.insert(cluster.end(), block.begin(), block.end());
            ++index;
        }
        put_master(clusters, kCluster, cluster);
    }

    Bytes segment;
    put_master(segment, kInfo, build_info(options, duration_ms));
    put_master(segment, kTracks, build_tracks(track));
    segment.insert(segment.end(), clusters.begin(), clusters.end());

    Bytes file;
    put_master(file, kEbmlHeader, build_ebml_header());
    put_master(file, kSegment, segment);
    return file;
}

Writer::Writer(AudioTrack track, MuxOptions options, std::vector<std::byte> header)
    : track_(std::move(track)), options_(std::move(options)), header_(std::move(header)) {}

std::expected<Writer, MuxError> Writer::create(const AudioTrack& track,
                                               const MuxOptions& options) {
    if (track.channels <= 0 || track.sample_rate == 0 || track.codec_id.empty() ||
        track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    Bytes header;
    put_master(header, kEbmlHeader, build_ebml_header());
    // Segment is the one element in this whole file with an unknown size -
    // see the class comment for why. Its children (Info, Tracks, every
    // Cluster pushed below) all keep normal, known sizes; only this outer
    // wrapper is open-ended, exactly like a live WebM/Matroska recording.
    put_master_unknown_size(header, kSegment);
    put_master(header, kInfo, build_info_streaming(options));
    put_master(header, kTracks, build_tracks(track));
    return Writer{track, options, std::move(header)};
}

std::uint64_t Writer::stamp_ms(std::size_t index) const {
    // Identical to mux()'s own stamp_ms lambda, and for the same reason: the
    // cumulative sample count is what keeps a frame duration that is not a
    // whole number of milliseconds from ever accumulating rounding error.
    return static_cast<std::uint64_t>(index) * track_.samples_per_frame * 1000 /
          track_.sample_rate;
}

std::vector<std::byte> Writer::close_cluster() {
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, cluster_base_ms_);
    cluster.insert(cluster.end(), cluster_body_.begin(), cluster_body_.end());
    Bytes out;
    put_master(out, kCluster, cluster);
    cluster_body_.clear();
    cluster_open_ = false;
    return out;
}

std::expected<std::vector<std::byte>, MuxError> Writer::push(std::span<const std::byte> frame) {
    if (frame.size() > (1ULL << 40)) {
        return std::unexpected(MuxError::kFrameTooLarge);
    }
    const auto abs_ms = stamp_ms(index_);
    std::vector<std::byte> closed;
    if (!cluster_open_) {
        // The very first frame of a cluster always starts one, the same
        // "first" exemption mux()'s own loop gives - there is nowhere else
        // for it to go even if its own duration already exceeds the budget.
        cluster_base_ms_ = abs_ms;
        cluster_open_ = true;
    } else {
        const std::uint64_t delta = abs_ms - cluster_base_ms_;
        // Close on the time budget, but never let a block's relative
        // timestamp leave int16 range whatever options_.cluster_ms asks for
        // - identical rule to mux()'s own loop, see there for why.
        if (delta >= options_.cluster_ms ||
            delta > static_cast<std::uint64_t>(std::numeric_limits<std::int16_t>::max())) {
            closed = close_cluster();
            cluster_base_ms_ = abs_ms;
            cluster_open_ = true;
        }
    }

    // SimpleBlock: track number as a vint, a signed 16-bit timestamp
    // relative to the cluster, one flags byte, then the frame - identical
    // construction to mux()'s own loop.
    const std::uint64_t delta = abs_ms - cluster_base_ms_;
    Bytes block;
    put_vint(block, 1);
    const auto relative = static_cast<std::int16_t>(delta);
    put_byte(block, static_cast<std::uint8_t>(relative >> 8));
    put_byte(block, static_cast<std::uint8_t>(relative & 0xFF));
    put_byte(block, 0x80);
    block.insert(block.end(), frame.begin(), frame.end());

    put_id(cluster_body_, kSimpleBlock);
    put_vint(cluster_body_, block.size());
    cluster_body_.insert(cluster_body_.end(), block.begin(), block.end());
    ++index_;
    return closed;
}

std::vector<std::byte> Writer::finalize() {
    if (!cluster_open_) {
        return {};
    }
    return close_cluster();
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options) {
    const std::vector<std::span<const std::byte>> views(frames.begin(), frames.end());
    return mux(track, views, options);
}

}  // namespace matroska
