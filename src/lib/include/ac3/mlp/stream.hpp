#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/mlp/matrix.hpp"
#include "ac3/mlp/mlp_tables.hpp"
#include "ac3/mlp/predictor.hpp"
#include "ac3/mlp/sync.hpp"

// The access-unit assembler - "Dolby TrueHD (MLP) high-level bitstream
// description" §3.3.1's access_unit(), assembled from the pieces this
// module tree already implements: mlp_sync (check_nibble/length/timing),
// periodic major_sync_info() (§2.5: "Major syncs occur at intervals of no
// fewer than eight access units and no more than 128"), the
// substream_directory, substream_segment() with its §4.6.6/§4.6.7
// parity+CRC, block()'s wrapper flags around the block codec, and
// restart_header() exactly on major-sync access units ("A substream
// segment starts with a block containing a restart header if and only if
// it is contained in an access unit that begins with a major sync").
//
// V1 SHAPE, deliberately narrow: ONE substream carrying ONE channel, one
// block per access unit (an access unit is 40 samples at 48 kHz - within
// block()'s 8..frame-length bounds), no EXTRA_DATA() (legal: "if
// substream_end_ptr[substreams-1] equals unit_end - start there is no
// EXTRA_DATA"), no end-of-stream terminators yet, and trivial FIFO timing
// (input_timing == output_timing == the running sample count; the FIFO
// smoothing of §2.6-2.7 is an encoder quality feature for rate-limited
// carriers, not a correctness requirement for framing round trips).
// Multichannel blocks, the matrix stage, and multiple substreams layer on
// from here. Fields whose shipping semantics are still provisional
// (lossless_check, max_bits, peak_data_rate under VBR) are written as
// documented placeholders - see restart_header.hpp and
// docs/concepts/truehd-mlp.md.

namespace ac3::mlp {

struct StreamConfig {
    SampleRate sample_rate = SampleRate::k48000;
    int wordlength = 24;          // stream-level sample width (v1: a stream
                                  // parameter, not yet carried in-band)
    int major_sync_interval = 8;  // 8..128 per §2.5
    int channels = 1;             // 1..16; carried in-band via the restart
                                  // header's max_chan, the way real MLP
                                  // scopes a substream's channels
    std::vector<matrix::Step> matrix{};  // PMQ cascade; may be empty
    // One per channel; leave empty for passthrough predictors everywhere.
    std::vector<PredictorCoefficients> coefficients{};
};

class AC3FORGE_EXPORT StreamEncoder {
   public:
    explicit StreamEncoder(const StreamConfig& config);

    // One access unit's worth of samples per channel
    // (samples_per_access_unit(rate)) in, one complete byte-aligned access
    // unit out. The first call emits a major sync (a stream must begin with
    // one, §5.1), then every major_sync_interval-th call after that.
    [[nodiscard]] std::vector<std::byte> encode_access_unit(
        std::span<const std::span<const std::int32_t>> channels);

    // Single-channel convenience; requires config.channels == 1.
    [[nodiscard]] std::vector<std::byte> encode_access_unit(
        std::span<const std::int32_t> samples);

   private:
    StreamConfig config_;
    std::uint64_t access_unit_index_ = 0;
    std::uint64_t sample_count_ = 0;
};

class AC3FORGE_EXPORT StreamDecoder {
   public:
    // `wordlength` matches the encoder's StreamConfig - a stream-level
    // parameter in the v1 shape.
    explicit StreamDecoder(int wordlength);

    // Decodes exactly one access unit (the caller frames the stream into
    // access units via the length field, as a demuxer would). The first
    // access unit fed in must carry a major sync - everything else about
    // the stream is learned in-band: sample rate (hence block length) from
    // major_sync_info, channel count from the restart header's max_chan.
    // Returns false on any framing, checksum, or consistency failure.
    [[nodiscard]] bool decode_access_unit(std::span<const std::byte> data,
                                          std::vector<std::vector<std::int32_t>>& channels);

    // Single-channel convenience; fails unless the stream carries exactly
    // one channel.
    [[nodiscard]] bool decode_access_unit(std::span<const std::byte> data,
                                          std::vector<std::int32_t>& samples);

    [[nodiscard]] bool has_stream_context() const { return have_context_; }
    [[nodiscard]] SampleRate sample_rate() const { return context_.sample_rate; }
    [[nodiscard]] int channels() const { return channels_; }

   private:
    int wordlength_;
    bool have_context_ = false;
    int channels_ = 0;
    MajorSyncInfo context_{};
};

}  // namespace ac3::mlp
