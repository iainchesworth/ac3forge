#include "ac3/mlp/stream.hpp"

#include <cassert>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/mlp/block.hpp"
#include "ac3/mlp/crc.hpp"
#include "ac3/mlp/restart_header.hpp"

namespace ac3::mlp {

namespace {

// §4.2.9's rules for the v1 single-substream shape: the 2-channel
// presentation is always in substream 0; bits 3-2 = 01 ("6-ch presentation
// is a copy of the 2-ch presentation carried in substream 0"); bits 6-4 =
// 001 (8-ch likewise); bit 7 = 0 (no 16-channel presentation).
constexpr std::uint8_t kSingleSubstreamInfo = 0x14;

[[nodiscard]] MajorSyncInfo make_major_sync(const StreamConfig& config) {
    MajorSyncInfo info;
    info.sample_rate = config.sample_rate;
    info.variable_rate = true;
    // §4.2.6 defines peak_data_rate over the whole stream, which a
    // streaming encoder cannot know up front - left zero as a documented
    // placeholder until rate control exists.
    info.peak_data_rate = 0;
    info.substreams = 1;
    info.extended_substream_info = 0;
    info.substream_info = kSingleSubstreamInfo;
    return info;
}

[[nodiscard]] RestartHeader make_restart_header(const StreamConfig& config,
                                                std::uint16_t output_timing) {
    RestartHeader header;
    header.substream_index = 0;
    header.restart_sync_word = kRestartSyncWordSubstream0;
    header.output_timing = output_timing;
    header.min_chan = 0;  // §4.7.2: one less than the channel number - one channel
    header.max_chan = 0;
    header.max_matrix_chan = 0;
    // Provisional-semantics fields (see restart_header.hpp): plausible
    // structural values, to be reconciled against layer-3/4 sources.
    header.max_bits_a = static_cast<std::uint8_t>(config.wordlength & 0x1F);
    header.max_bits_b = static_cast<std::uint8_t>(config.wordlength & 0x1F);
    header.lossless_check = 0;
    header.channel_assignment = {0};
    return header;
}

}  // namespace

StreamEncoder::StreamEncoder(const StreamConfig& config) : config_(config) {
    assert(config.major_sync_interval >= 8 && config.major_sync_interval <= 128);
    assert(config.wordlength >= 2 && config.wordlength <= 24);
}

std::vector<std::byte> StreamEncoder::encode_access_unit(std::span<const std::int32_t> samples) {
    const auto frame = static_cast<std::size_t>(samples_per_access_unit(config_.sample_rate));
    assert(samples.size() == frame);

    const bool major =
        access_unit_index_ % static_cast<std::uint64_t>(config_.major_sync_interval) == 0;
    const auto timing = static_cast<std::uint16_t>(sample_count_ & 0xFFFF);

    // substream_segment(): block() wrapper flags, restart header on major
    // sync only (§2.5), the block itself, the last-block flag, then §2.5's
    // "padding to a 16-bit boundary".
    BitWriter segment;
    segment.put(1, 1);             // block_header_exists
    segment.put(major ? 1u : 0u, 1);  // restart_header_exists
    if (major) {
        (void)build_restart_header(segment, make_restart_header(config_, timing));
    }
    encode_block(segment, samples, config_.wordlength, config_.coefficients);
    segment.put(1, 1);  // last_block_in_segment
    const auto segment_bits = segment.bit_count();
    const auto pad = (16 - segment_bits % 16) % 16;
    if (pad != 0) {
        segment.put(0, static_cast<int>(pad));
    }
    const std::vector<std::byte> segment_bytes = segment.take();
    const auto parity = substream_parity(segment_bytes);
    const auto crc = substream_crc(segment_bytes);

    std::vector<std::byte> major_bytes;
    if (major) {
        major_bytes = build_major_sync_info(make_major_sync(config_));
    }

    const std::size_t total = 4 + major_bytes.size() + 2 + segment_bytes.size() + 2;
    assert(total % 2 == 0);
    const auto length_words = static_cast<std::uint16_t>(total / 2);
    assert(length_words < 4096);

    BitWriter unit;
    unit.put(compute_check_nibble(length_words, timing), 4);
    unit.put(length_words, 12);
    unit.put(timing, 16);
    for (const auto b : major_bytes) {
        unit.put(std::to_integer<std::uint32_t>(b), 8);
    }
    // substream_directory, one entry: no extra 16-bit DRC word;
    // restart_nonexistent is the INVERSE of "this is a major sync" (§4.5.2);
    // crc_present on, since the segment carries parity+CRC.
    unit.put(0, 1);                   // extra_substream_word
    unit.put(major ? 0u : 1u, 1);     // restart_nonexistent
    unit.put(1, 1);                   // crc_present
    unit.put(0, 1);                   // reserved
    // §4.5.4: substream_end_ptr is the offset of substream_end relative to
    // /* start */ (just after the directory), in 16-bit words - and per
    // §3.3.1's label placement, substream_end sits after the parity/CRC.
    unit.put(static_cast<std::uint32_t>((segment_bytes.size() + 2) / 2), 12);
    for (const auto b : segment_bytes) {
        unit.put(std::to_integer<std::uint32_t>(b), 8);
    }
    unit.put(parity, 8);
    unit.put(crc, 8);

    ++access_unit_index_;
    sample_count_ += frame;
    return unit.take();
}

StreamDecoder::StreamDecoder(int wordlength) : wordlength_(wordlength) {
    assert(wordlength >= 2 && wordlength <= 24);
}

bool StreamDecoder::decode_access_unit(std::span<const std::byte> data,
                                       std::vector<std::int32_t>& samples) {
    if (data.size() < 8 || data.size() % 2 != 0) {
        return false;
    }

    BitReader r(data);
    const auto nibble = static_cast<std::uint8_t>(r.read(4));
    const auto length_words = static_cast<std::uint16_t>(r.read(12));
    const auto timing = static_cast<std::uint16_t>(r.read(16));
    if (static_cast<std::size_t>(length_words) * 2 != data.size()) {
        return false;
    }
    if (nibble != compute_check_nibble(length_words, timing)) {
        return false;
    }

    // §3.1: a major sync is identified by the 32 bits at bit offset 32
    // equalling format_sync.
    std::size_t offset = 4;
    const bool major = data.size() >= 8 && r.read(32) == kFormatSync;
    if (major) {
        // parse_major_sync_info wants the whole 28-byte structure.
        if (data.size() < offset + 28 + 2) {
            return false;
        }
        MajorSyncInfo info;
        if (!parse_major_sync_info(data.subspan(offset, 28), info)) {
            return false;
        }
        if (info.substreams != 1) {
            return false;  // v1 shape: single substream only
        }
        context_ = info;
        have_context_ = true;
        offset += 28;
    }
    if (!have_context_) {
        return false;  // §5.1: decoding starts at a major sync
    }

    // substream_directory (one entry).
    BitReader directory(data.subspan(offset, 2));
    const auto extra_substream_word = directory.read(1);
    const auto restart_nonexistent = directory.read(1);
    const auto crc_present = directory.read(1);
    directory.skip(1);  // reserved
    const auto end_ptr = directory.read(12);
    if (extra_substream_word != 0) {
        return false;  // v1: no inline DRC word
    }
    if ((restart_nonexistent == 0) != major) {
        return false;  // §4.5.2's restart-iff-major rule
    }
    offset += 2;

    const std::size_t end_bytes = static_cast<std::size_t>(end_ptr) * 2;
    if (offset + end_bytes != data.size()) {
        return false;
    }
    const std::size_t segment_len = crc_present != 0 ? end_bytes - 2 : end_bytes;
    const auto segment = data.subspan(offset, segment_len);
    if (crc_present != 0) {
        if (substream_parity(segment) != std::to_integer<std::uint8_t>(data[offset + segment_len]) ||
            substream_crc(segment) != std::to_integer<std::uint8_t>(data[offset + segment_len + 1])) {
            return false;
        }
    }

    BitReader sr(segment);
    if (sr.read(1) != 1) {
        return false;  // block_header_exists: always set by this encoder
    }
    const bool restart = sr.read(1) != 0;
    if (restart != major) {
        return false;
    }
    if (restart) {
        RestartHeader header;
        if (!parse_restart_header(sr, 0, header)) {
            return false;
        }
    }

    samples.resize(static_cast<std::size_t>(samples_per_access_unit(context_.sample_rate)));
    if (!decode_block(sr, wordlength_, samples)) {
        return false;
    }
    if (sr.read(1) != 1) {
        return false;  // last_block_in_segment
    }
    while (sr.bit_position() % 16 != 0) {
        if (sr.read_bit() != 0) {
            return false;  // padding must be zeros
        }
    }
    return !sr.overflowed() && sr.bit_position() == segment_len * 8;
}

}  // namespace ac3::mlp
