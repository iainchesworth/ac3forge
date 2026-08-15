#include "mp4/mp4.hpp"

#include <array>
#include <cassert>
#include <limits>

namespace mp4 {

namespace {

using Bytes = std::vector<std::byte>;

void put_u8(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_u16(Bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value & 0xFF));
}

void put_u32(Bytes& out, std::uint32_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 24));
    put_u8(out, static_cast<std::uint8_t>((value >> 16) & 0xFF));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xFF));
    put_u8(out, static_cast<std::uint8_t>(value & 0xFF));
}

// ISO/IEC 14496-12 §4.2: a box type is 4 printable-ASCII bytes.
void put_fourcc(Bytes& out, std::string_view fourcc) {
    assert(fourcc.size() == 4);
    for (const char c : fourcc) {
        put_u8(out, static_cast<std::uint8_t>(c));
    }
}

void put_bytes(Bytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// ISO/IEC 14496-12 §4.2's Box: a 32-bit size (the WHOLE box, header
// included), then the 4-byte type, then the body. Every box this module
// builds stays well under 4 GiB (mux() itself refuses anything that would
// not), so the 64-bit largesize escape (size field == 1) is never needed.
void put_box(Bytes& out, std::string_view fourcc, const Bytes& body) {
    put_u32(out, static_cast<std::uint32_t>(8 + body.size()));
    put_fourcc(out, fourcc);
    put_bytes(out, body);
}

// ISO/IEC 14496-12 §4.2's FullBox: a Box with a 1-byte version and 3-byte
// flags prepended to the body.
void put_fullbox(Bytes& out, std::string_view fourcc, std::uint8_t version, std::uint32_t flags,
                 const Bytes& body) {
    Bytes full;
    put_u8(full, version);
    put_u8(full, static_cast<std::uint8_t>(flags >> 16));
    put_u8(full, static_cast<std::uint8_t>((flags >> 8) & 0xFF));
    put_u8(full, static_cast<std::uint8_t>(flags & 0xFF));
    put_bytes(full, body);
    put_box(out, fourcc, full);
}

// ISO/IEC 14496-12 §8.4.2.2: a track/media language is packed as three 5-bit
// (letter - 0x60) codes with the top bit always 0 - "und" (undetermined)
// packs to 0x55C4, the conventional value a track with no real language
// metadata carries. Falls back to "und" for anything not exactly 3 letters
// rather than packing garbage.
[[nodiscard]] std::uint16_t pack_language(std::string_view lang) {
    if (lang.size() != 3) {
        lang = "und";
    }
    std::uint16_t packed = 0;
    for (const char c : lang) {
        const auto ch = static_cast<unsigned char>(c);
        std::uint16_t letter = 0;
        if (ch >= 'a' && ch <= 'z') {
            letter = static_cast<std::uint16_t>(ch - 'a' + 1);
        } else if (ch >= 'A' && ch <= 'Z') {
            letter = static_cast<std::uint16_t>(ch - 'A' + 1);
        }
        packed = static_cast<std::uint16_t>((packed << 5) | letter);
    }
    return packed;
}

// ISO/IEC 14496-12 §8.2.2.2/§8.3.2.2: mvhd's and tkhd's identity transform,
// a 3x3 matrix in 16.16/2.30 fixed point stored row-major with an implicit
// third column.
constexpr std::array<std::uint32_t, 9> kUnityMatrix{
    0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000,
};

Bytes build_ftyp() {
    Bytes body;
    put_fourcc(body, "isom");  // major_brand
    put_u32(body, 0);          // minor_version
    put_fourcc(body, "isom");  // compatible_brands[]
    put_fourcc(body, "iso2");
    put_fourcc(body, "mp41");
    Bytes out;
    put_box(out, "ftyp", body);
    return out;
}

Bytes build_mvhd(std::uint32_t timescale, std::uint64_t duration) {
    Bytes body;
    put_u32(body, 0);  // creation_time
    put_u32(body, 0);  // modification_time
    put_u32(body, timescale);
    put_u32(body, static_cast<std::uint32_t>(duration));
    put_u32(body, 0x00010000);  // rate: 1.0x
    put_u16(body, 0x0100);      // volume: 1.0
    put_u16(body, 0);           // reserved
    put_u32(body, 0);
    put_u32(body, 0);  // reserved[2]
    for (const auto v : kUnityMatrix) {
        put_u32(body, v);
    }
    for (int i = 0; i < 6; ++i) {
        put_u32(body, 0);  // pre_defined[6]
    }
    put_u32(body, 2);  // next_track_ID: this module writes exactly one track (ID 1)
    Bytes out;
    put_fullbox(out, "mvhd", 0, 0, body);
    return out;
}

Bytes build_tkhd(std::uint64_t duration) {
    Bytes body;
    put_u32(body, 0);  // creation_time
    put_u32(body, 0);  // modification_time
    put_u32(body, 1);  // track_ID
    put_u32(body, 0);  // reserved
    put_u32(body, static_cast<std::uint32_t>(duration));
    put_u32(body, 0);
    put_u32(body, 0);  // reserved[2]
    put_u16(body, 0);       // layer
    put_u16(body, 0);       // alternate_group
    put_u16(body, 0x0100);  // volume: an audio track plays at full volume
    put_u16(body, 0);       // reserved
    for (const auto v : kUnityMatrix) {
        put_u32(body, v);
    }
    put_u32(body, 0);  // width: 0, this is an audio track
    put_u32(body, 0);  // height
    Bytes out;
    // flags 0x000007: Track_enabled | Track_in_movie | Track_in_preview.
    put_fullbox(out, "tkhd", 0, 0x000007, body);
    return out;
}

Bytes build_mdhd(std::uint32_t timescale, std::uint64_t duration, std::string_view language) {
    Bytes body;
    put_u32(body, 0);  // creation_time
    put_u32(body, 0);  // modification_time
    put_u32(body, timescale);
    put_u32(body, static_cast<std::uint32_t>(duration));
    put_u16(body, pack_language(language));
    put_u16(body, 0);  // pre_defined
    Bytes out;
    put_fullbox(out, "mdhd", 0, 0, body);
    return out;
}

Bytes build_hdlr(std::string_view name) {
    Bytes body;
    put_u32(body, 0);          // pre_defined
    put_fourcc(body, "soun");  // handler_type: sound media
    put_u32(body, 0);
    put_u32(body, 0);
    put_u32(body, 0);  // reserved[3]
    for (const char c : name) {
        put_u8(body, static_cast<std::uint8_t>(c));
    }
    put_u8(body, 0);  // NUL-terminated string
    Bytes out;
    put_fullbox(out, "hdlr", 0, 0, body);
    return out;
}

Bytes build_smhd() {
    Bytes body;
    put_u16(body, 0);  // balance: centred
    put_u16(body, 0);  // reserved
    Bytes out;
    put_fullbox(out, "smhd", 0, 0, body);
    return out;
}

Bytes build_dinf() {
    // 'url ', flags bit 0 ("media data is in the same file as the Movie
    // Box") set, so no location string is needed in the body.
    Bytes url;
    put_fullbox(url, "url ", 0, 0x000001, {});
    Bytes dref_body;
    put_u32(dref_body, 1);  // entry_count
    put_bytes(dref_body, url);
    Bytes dref;
    put_fullbox(dref, "dref", 0, 0, dref_body);
    Bytes out;
    put_box(out, "dinf", dref);
    return out;
}

// The audio sample entry (ISO/IEC 14496-12 §12.2.3's AudioSampleEntry,
// itself extending §8.5.2's SampleEntry) plus its one child configuration
// box - 'dac3' for an "ac-3" track, 'dec3' for "ec-3" (ETSI TS 102 366
// Annex F). track.codec_id has already been validated to be one of exactly
// those two strings by the time mux() calls this.
Bytes build_sample_entry(const AudioTrack& track) {
    Bytes body;
    put_u32(body, 0);
    put_u16(body, 0);  // SampleEntry::reserved[6]
    put_u16(body, 1);  // SampleEntry::data_reference_index
    put_u32(body, 0);
    put_u32(body, 0);  // AudioSampleEntry::reserved[2]
    put_u16(body, static_cast<std::uint16_t>(track.channels));
    put_u16(body, 16);  // samplesize: this project's decoder output is always 16-bit PCM-shaped
    put_u16(body, 0);   // pre_defined
    put_u16(body, 0);   // reserved
    // samplerate: 16.16 fixed point, integer part only - mux()'s own
    // kInvalidTrack check keeps sample_rate under 2^16 so this never
    // overflows the field.
    put_u32(body, track.sample_rate << 16);

    const std::string_view config_fourcc = track.codec_id == kCodecAc3 ? "dac3" : "dec3";
    put_box(body, config_fourcc, track.codec_config);

    Bytes out;
    put_box(out, track.codec_id, body);
    return out;
}

Bytes build_stsd(const AudioTrack& track) {
    Bytes body;
    put_u32(body, 1);  // entry_count: exactly one sample description
    put_bytes(body, build_sample_entry(track));
    Bytes out;
    put_fullbox(out, "stsd", 0, 0, body);
    return out;
}

Bytes build_stts(std::uint32_t sample_count, std::uint32_t sample_delta) {
    Bytes body;
    put_u32(body, 1);  // entry_count: every access unit is the same length
    put_u32(body, sample_count);
    put_u32(body, sample_delta);
    Bytes out;
    put_fullbox(out, "stts", 0, 0, body);
    return out;
}

// One sample per chunk - the simplest legal stsc/stco pairing, and the same
// minimalism matroska::mux() applies to its own one-SimpleBlock-per-frame
// layout. Coarser chunking would help a player's seek performance on a very
// long file; nothing about correctness needs it for the streams this project
// produces.
Bytes build_stsc(std::uint32_t chunk_count) {
    Bytes body;
    put_u32(body, chunk_count > 0 ? 1U : 0U);  // entry_count
    if (chunk_count > 0) {
        put_u32(body, 1);  // first_chunk
        put_u32(body, 1);  // samples_per_chunk
        put_u32(body, 1);  // sample_description_index
    }
    Bytes out;
    put_fullbox(out, "stsc", 0, 0, body);
    return out;
}

Bytes build_stsz(std::span<const std::vector<std::byte>> frames) {
    Bytes body;
    put_u32(body, 0);  // sample_size: 0 means "read each size from the table below"
    put_u32(body, static_cast<std::uint32_t>(frames.size()));
    for (const auto& frame : frames) {
        put_u32(body, static_cast<std::uint32_t>(frame.size()));
    }
    Bytes out;
    put_fullbox(out, "stsz", 0, 0, body);
    return out;
}

Bytes build_stco(std::span<const std::uint32_t> chunk_offsets) {
    Bytes body;
    put_u32(body, static_cast<std::uint32_t>(chunk_offsets.size()));
    for (const auto offset : chunk_offsets) {
        put_u32(body, offset);
    }
    Bytes out;
    put_fullbox(out, "stco", 0, 0, body);
    return out;
}

Bytes build_moov(const AudioTrack& track, const MuxOptions& options,
                 std::span<const std::vector<std::byte>> frames,
                 std::span<const std::uint32_t> chunk_offsets, std::uint64_t total_samples) {
    Bytes stbl_body;
    put_bytes(stbl_body, build_stsd(track));
    put_bytes(stbl_body, build_stts(static_cast<std::uint32_t>(frames.size()),
                                    track.samples_per_frame));
    put_bytes(stbl_body, build_stsc(static_cast<std::uint32_t>(frames.size())));
    put_bytes(stbl_body, build_stsz(frames));
    put_bytes(stbl_body, build_stco(chunk_offsets));
    Bytes stbl;
    put_box(stbl, "stbl", stbl_body);

    Bytes minf_body;
    put_bytes(minf_body, build_smhd());
    put_bytes(minf_body, build_dinf());
    put_bytes(minf_body, stbl);
    Bytes minf;
    put_box(minf, "minf", minf_body);

    Bytes mdia_body;
    put_bytes(mdia_body, build_mdhd(track.sample_rate, total_samples, track.language));
    put_bytes(mdia_body, build_hdlr(options.writing_app));
    put_bytes(mdia_body, minf);
    Bytes mdia;
    put_box(mdia, "mdia", mdia_body);

    Bytes trak_body;
    put_bytes(trak_body, build_tkhd(total_samples));
    put_bytes(trak_body, mdia);
    Bytes trak;
    put_box(trak, "trak", trak_body);

    Bytes moov_body;
    put_bytes(moov_body, build_mvhd(track.sample_rate, total_samples));
    put_bytes(moov_body, trak);
    Bytes out;
    put_box(out, "moov", moov_body);
    return out;
}

}  // namespace

std::string_view describe(MuxError error) {
    switch (error) {
        case MuxError::kNoFrames:
            return "no frames to mux";
        case MuxError::kInvalidTrack:
            return "invalid track: channels, sample rate, a recognised codec id and its "
                   "codec_config payload are required";
        case MuxError::kFileTooLarge:
            return "file too large: needs a 64-bit chunk offset (co64), unsupported in this cut";
    }
    return "unknown error";
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (track.channels <= 0 || track.sample_rate == 0 ||
        track.sample_rate > std::numeric_limits<std::uint16_t>::max() ||
        track.samples_per_frame == 0 || track.codec_config.empty() ||
        (track.codec_id != kCodecAc3 && track.codec_id != kCodecEac3)) {
        return std::unexpected(MuxError::kInvalidTrack);
    }

    const std::uint64_t total_samples =
        static_cast<std::uint64_t>(frames.size()) * track.samples_per_frame;
    if (total_samples > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(MuxError::kFileTooLarge);
    }

    const Bytes ftyp = build_ftyp();

    // ISOBMFF's usual chicken-and-egg: stco's chunk offsets are absolute
    // FILE positions, which depend on moov's size, which - since box sizes
    // are self-describing - is already fixed regardless of what those offset
    // VALUES turn out to be. So this builds moov once with placeholder
    // (zero) offsets purely to measure it, then builds it again with the
    // real ones now that mdat's start is known. Simpler than patching
    // already-serialized bytes in place (matroska::mux() has no equivalent
    // problem: EBML elements are self-contained, nothing in one needs to
    // know another's absolute file offset).
    const std::vector<std::uint32_t> placeholder_offsets(frames.size(), 0);
    const Bytes moov_measured =
        build_moov(track, options, frames, placeholder_offsets, total_samples);

    constexpr std::uint64_t kMdatHeaderBytes = 8;
    const std::uint64_t mdat_start =
        static_cast<std::uint64_t>(ftyp.size()) + moov_measured.size() + kMdatHeaderBytes;

    std::vector<std::uint32_t> offsets(frames.size());
    std::uint64_t cursor = mdat_start;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (cursor > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(MuxError::kFileTooLarge);
        }
        offsets[i] = static_cast<std::uint32_t>(cursor);
        cursor += frames[i].size();
    }
    const std::uint64_t mdat_body_bytes = cursor - mdat_start;
    if (cursor > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(MuxError::kFileTooLarge);
    }

    const Bytes moov = build_moov(track, options, frames, offsets, total_samples);
    // Same box structure as moov_measured above, offset VALUES aside - see
    // the comment on the two-pass build above.
    assert(moov.size() == moov_measured.size());

    Bytes file;
    file.reserve(ftyp.size() + moov.size() + static_cast<std::size_t>(kMdatHeaderBytes) +
                static_cast<std::size_t>(mdat_body_bytes));
    put_bytes(file, ftyp);
    put_bytes(file, moov);
    put_u32(file, static_cast<std::uint32_t>(kMdatHeaderBytes + mdat_body_bytes));
    put_fourcc(file, "mdat");
    for (const auto& frame : frames) {
        put_bytes(file, frame);
    }
    return file;
}

}  // namespace mp4
