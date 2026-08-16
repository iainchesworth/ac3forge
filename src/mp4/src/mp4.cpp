#include "mp4/mp4.hpp"

#include <array>
#include <cassert>
#include <limits>

#include "isobmff_detail.hpp"

namespace mp4 {

namespace {

using detail::Bytes;
using detail::put_box;
using detail::put_bytes;
using detail::put_fourcc;
using detail::put_fullbox;
using detail::put_u32;

Bytes build_ftyp() {
    constexpr std::array<std::string_view, 3> kCompatibleBrands{"isom", "iso2", "mp41"};
    return detail::build_brand_box("ftyp", "isom", 0, kCompatibleBrands);
}

Bytes build_moov(const AudioTrack& track, const MuxOptions& options,
                 std::span<const std::vector<std::byte>> frames,
                 std::span<const std::uint32_t> chunk_offsets, std::uint64_t total_samples) {
    Bytes stbl_body;
    put_bytes(stbl_body, detail::build_stsd(track));
    put_bytes(stbl_body, detail::build_stts(static_cast<std::uint32_t>(frames.size()),
                                            track.samples_per_frame));
    put_bytes(stbl_body, detail::build_stsc(static_cast<std::uint32_t>(frames.size())));
    put_bytes(stbl_body, detail::build_stsz(frames));
    put_bytes(stbl_body, detail::build_stco(chunk_offsets));
    Bytes stbl;
    put_box(stbl, "stbl", stbl_body);

    Bytes minf_body;
    put_bytes(minf_body, detail::build_smhd());
    put_bytes(minf_body, detail::build_dinf());
    put_bytes(minf_body, stbl);
    Bytes minf;
    put_box(minf, "minf", minf_body);

    Bytes mdia_body;
    put_bytes(mdia_body, detail::build_mdhd(track.sample_rate, total_samples, track.language));
    put_bytes(mdia_body, detail::build_hdlr(options.writing_app));
    put_bytes(mdia_body, minf);
    Bytes mdia;
    put_box(mdia, "mdia", mdia_body);

    Bytes trak_body;
    put_bytes(trak_body, detail::build_tkhd(total_samples));
    put_bytes(trak_body, mdia);
    Bytes trak;
    put_box(trak, "trak", trak_body);

    Bytes moov_body;
    put_bytes(moov_body, detail::build_mvhd(track.sample_rate, total_samples));
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
        case MuxError::kInvalidOptions:
            return "invalid options";
    }
    return "unknown error";
}

std::expected<std::vector<std::byte>, MuxError> mux(const AudioTrack& track,
                                                    std::span<const std::vector<std::byte>> frames,
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
