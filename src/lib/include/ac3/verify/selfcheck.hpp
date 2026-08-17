#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/export.hpp"
#include "ac3/verify/mirror.hpp"

// The encode-then-decode-then-compare driver over ac3/verify/mirror.hpp.
//
// MirrorEncoder is a drop-in for FrameEncoder that additionally decodes every
// frame it emits with this repo's own decoder and diffs the decoder's model
// against the encoder's own. It is a debugging and test facility, not a
// production encode path: it costs a full decode per frame on top of the
// encode. FrameEncoder itself is untouched by using this - the trace pointer
// EncoderConfig now carries is null unless something like this class sets it,
// and a null trace means one predictable branch per block and no allocation.

namespace ac3::verify {

// One frame, plus what the check found out about it.
struct CheckedFrame {
    std::vector<std::byte> frame;
    // Empty when the two models agree, which is the only outcome a correct
    // encoder ever produces. Otherwise the first divergent block's findings,
    // most useful first - see compare().
    std::vector<Mismatch> mismatches;
    // Set when the decoder refused the frame outright rather than merely
    // disagreeing about it. The two are independent findings and both are
    // worth having: a refusal says the stream is unusable, `mismatches` says
    // WHERE the two sides parted company, and the second is what points at
    // the bug. A desync typically produces both, with the mismatch naming an
    // earlier block than the refusal - that gap is the misdirection this
    // whole facility exists to remove.
    std::optional<DecodeError> decode_error;

    [[nodiscard]] bool ok() const { return mismatches.empty() && !decode_error.has_value(); }
};

class AC3FORGE_EXPORT MirrorEncoder {
   public:
    // `config` is taken by value and its trace pointer overwritten - a caller
    // has no use for setting one here, and letting one through would silently
    // disable the check.
    explicit MirrorEncoder(EncoderConfig config);

    // Non-copyable and non-movable: both configs hold pointers into this
    // object's own trace members, which a copy or move would leave aimed at
    // the original.
    MirrorEncoder(const MirrorEncoder&) = delete;
    MirrorEncoder& operator=(const MirrorEncoder&) = delete;
    MirrorEncoder(MirrorEncoder&&) = delete;
    MirrorEncoder& operator=(MirrorEncoder&&) = delete;

    // Same contract as FrameEncoder::encode_frame, with the check run over
    // the result. An encode failure propagates unchanged and consumes no
    // frame index.
    [[nodiscard]] std::expected<CheckedFrame, FrameError> encode_frame(
        std::span<const std::span<const float>> channels);

    // Frames encoded so far, which is the index the NEXT frame will report.
    [[nodiscard]] std::uint64_t frames_encoded() const { return frame_index_; }

    // The two views of the most recently encoded frame, for a caller wanting
    // more than compare() reports.
    [[nodiscard]] const FrameTrace& encoder_trace() const { return encoder_trace_; }
    [[nodiscard]] const FrameTrace& decoder_trace() const { return decoder_trace_; }

    // The most recent frame's findings rendered as text, with the stream
    // names resolved from that frame's own shape. Empty when it was clean.
    [[nodiscard]] std::string last_report() const;

   private:
    // Declared before the encoder and decoder below: their configs hold
    // pointers to these, so these must outlive them and be constructed first.
    FrameTrace encoder_trace_;
    FrameTrace decoder_trace_;
    FrameEncoder encoder_;
    FrameDecoder decoder_;
    std::vector<Mismatch> last_mismatches_;
    std::uint64_t frame_index_ = 0;
};

}  // namespace ac3::verify
