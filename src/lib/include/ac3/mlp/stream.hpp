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
// V1 SHAPE, deliberately narrow: ONE substream, one block per access unit
// (an access unit is 40 samples at 48 kHz - within block()'s 8..frame-length
// bounds) and no EXTRA_DATA() (legal: "if substream_end_ptr[substreams-1]
// equals unit_end - start there is no EXTRA_DATA"). Multiple substreams
// layer on from here. Fields whose shipping semantics are still provisional
// (lossless_check, max_bits) are written as documented placeholders - see
// restart_header.hpp and docs/concepts/truehd-mlp.md.
//
// FIFO timing (§2.6-2.7) is real here, not trivial: output_timing is each
// frame's own first-sample number (§4.7.2), and input_timing is scheduled so
// the effective delivery rate size[n] / (input_timing[n+1] - input_timing[n])
// never exceeds the 18 Mbit/s FBA ceiling (§2.7), while the FIFO delay
// (output_timing - input_timing) stays within kFifoDelayFrames frames -
// which caps decoder-buffer occupancy at (kFifoDelayFrames + 1) largest
// possible access units, 106,470 bytes, inside §2.7's 120,000-byte minimum
// buffer. The schedule is causal (each AU's spacing depends only on the
// previous AU's size) and returns to the constant maximum delay whenever
// the audio compresses well enough. End-of-stream terminators
// (§4.6.2-4.6.5) come from the EndOfStream overload, whose zero_samples
// count lets a decoder restore the source's exact length after the encoder
// pads the final access unit.
//
// Noise shaping is deliberately ABSENT, not missing: in WO 96/37048 the
// noise shaper acts on the quantizer inside the prediction loop, and that
// quantizer only quantizes in the LOSSY operating mode. This encoder is
// lossless-only, the in-loop quantizer is the identity, and the shaper
// state would be identically zero - dead machinery. Revisit only if a
// lossy/rate-capped mode is ever added.

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
    // When set, `matrix` and `coefficients` are ignored and the encoder
    // chooses both per access unit (select::choose_block_config) - the
    // WO's own suggested encoder practice of trying preselected filters
    // per block and keeping whichever is best.
    bool automatic = false;
    // §4.2.6's peak_data_rate, in 1/16 bit per sample period. Zero means
    // "write the enforced FBA channel ceiling" - the truthful upper bound a
    // single-pass encoder can state. A caller that encodes twice (the CLI
    // does; the field is fixed-width, so pass two's sizes are identical)
    // sets the first pass's measured_peak_data_rate_16ths() here to write
    // the exact whole-stream maximum §4.2.6 defines.
    std::uint32_t peak_data_rate_16ths = 0;
};

// §2.7's delay ceiling, in access units. 12 frames holds the worst case
// with margin: a maximum-size access unit (8,190 bytes) needs
// ceil(65,520 bits / (18e6 / rate)) samples of delivery spacing = 4.375
// frames at every sample rate, and 13 in-flight maximum-size units are
// 106,470 bytes against the 120,000-byte minimum FIFO.
inline constexpr int kFifoDelayFrames = 12;

// Marks the final access unit of the stream (§4.6.2-4.6.5): zero_samples
// is how many padding sample periods the caller appended to fill the last
// access unit (0 writes terminatorB instead of the §4.6.4 count).
struct EndOfStream {
    int zero_samples = 0;  // §4.6.4, u(13): 0..8191
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

    // The final access unit: same as above plus §4.6's terminator fields.
    // No further access units may be encoded after this.
    [[nodiscard]] std::vector<std::byte> encode_access_unit(
        std::span<const std::span<const std::int32_t>> channels, EndOfStream end);

    // Single-channel convenience; requires config.channels == 1.
    [[nodiscard]] std::vector<std::byte> encode_access_unit(
        std::span<const std::int32_t> samples);

    // The largest effective delivery rate scheduled so far (§2.7's
    // size / input-timing spacing), in §4.2.6's 1/16-bit-per-sample units.
    // The definitive whole-stream value once the final access unit is
    // encoded (the last unit is exempt from the rate condition and never
    // contributes a spacing).
    [[nodiscard]] std::uint32_t measured_peak_data_rate_16ths() const {
        return measured_peak_16ths_;
    }

    // Access units whose §2.7 peak-rate spacing could not be honoured
    // because the FIFO delay had already reached zero - audio that
    // genuinely exceeds the 18 Mbit/s FBA channel. Zero for any stream a
    // compliant decoder is guaranteed to accept.
    [[nodiscard]] std::uint64_t rate_violations() const { return rate_violations_; }

   private:
    [[nodiscard]] std::vector<std::byte> encode_impl(
        std::span<const std::span<const std::int32_t>> channels, const EndOfStream* end);

    StreamConfig config_;
    std::uint64_t access_unit_index_ = 0;
    std::uint64_t sample_count_ = 0;
    // Unwrapped FIFO schedule state: the previous unit's input time and
    // size decide the earliest legal next input time. input_timing_ starts
    // one full delay before sample zero, so the wrapped field value simply
    // starts at 65536 - the delay.
    std::int64_t input_timing_ = 0;
    std::int64_t previous_size_bits_ = 0;
    std::uint32_t measured_peak_16ths_ = 0;
    std::uint64_t rate_violations_ = 0;
    bool finished_ = false;
};

class AC3FORGE_EXPORT StreamDecoder {
   public:
    // `wordlength` is only a starting default: every restart header carries
    // the stream's actual sample width in its max_bits fields (the encoder
    // writes StreamConfig::wordlength there), and the first major sync's
    // restart overrides this value before any samples are decoded.
    explicit StreamDecoder(int wordlength = 24);

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
    [[nodiscard]] int wordlength() const { return wordlength_; }

    // True once an access unit carrying §4.6's terminators has decoded -
    // the stream's final unit. zero_samples_appended() is its §4.6.4 count:
    // sample periods the encoder added to fill that unit, which a caller
    // trims to recover the source's exact length (0 when the stream ended
    // with terminatorB instead).
    [[nodiscard]] bool end_of_stream() const { return end_of_stream_; }
    [[nodiscard]] int zero_samples_appended() const { return zero_samples_; }

    // The FIFO delay (§2.7: output_timing - input_timing, mod 2^16) at the
    // most recent restart header - diagnostic visibility into the
    // encoder's delivery schedule.
    [[nodiscard]] int fifo_delay_samples() const { return fifo_delay_; }

   private:
    int wordlength_;
    bool have_context_ = false;
    int channels_ = 0;
    bool end_of_stream_ = false;
    int zero_samples_ = 0;
    int fifo_delay_ = 0;
    MajorSyncInfo context_{};
};

}  // namespace ac3::mlp
