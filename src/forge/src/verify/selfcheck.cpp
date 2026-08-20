#include "ac3/verify/selfcheck.hpp"

namespace ac3::verify {

namespace {

EncoderConfig with_trace(EncoderConfig config, FrameTrace* trace) {
    config.trace = trace;
    return config;
}

DecoderConfig decoder_config(FrameTrace* trace) {
    DecoderConfig config;
    // drc_scale stays 0: this decoder exists to check what the encoder wrote,
    // and none of the compared state is affected by the §7.7 gain anyway (it
    // is applied to reconstructed coefficients, downstream of every field the
    // trace records). Left explicit rather than defaulted-by-omission so a
    // future change to DecoderConfig's default cannot quietly alter what the
    // self-check decodes.
    config.drc_scale = 0.0;
    config.trace = trace;
    return config;
}

}  // namespace

MirrorEncoder::MirrorEncoder(EncoderConfig config)
    : encoder_(with_trace(config, &encoder_trace_)),
      decoder_(decoder_config(&decoder_trace_)) {}

std::expected<CheckedFrame, FrameError> MirrorEncoder::encode_frame(
    std::span<const std::span<const float>> channels) {
    auto encoded = encoder_.encode_frame(channels);
    if (!encoded) {
        last_mismatches_.clear();
        return std::unexpected(encoded.error());
    }

    CheckedFrame result;
    result.frame = std::move(*encoded);
    // The decoder writes into decoder_trace_ as it goes, so a refusal
    // part-way through still leaves everything it managed to read - which is
    // exactly the case where naming the block matters most.
    auto decoded = decoder_.decode_frame(result.frame);
    if (!decoded) {
        result.decode_error = decoded.error();
    }
    result.mismatches = compare(encoder_trace_, decoder_trace_, frame_index_);
    last_mismatches_ = result.mismatches;
    ++frame_index_;
    return result;
}

std::string MirrorEncoder::last_report() const {
    return report(last_mismatches_, encoder_trace_.fbw_channels, encoder_trace_.coded_channels);
}

}  // namespace ac3::verify
