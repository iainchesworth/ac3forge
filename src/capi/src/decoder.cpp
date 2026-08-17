#include <memory>
#include <span>

#include "internal.hpp"

using ac3forge_c::guard;

extern "C" {

void ac3forge_decoder_config_init(ac3forge_decoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac3::DecoderConfig defaults{};
    *config = ac3forge_decoder_config_t{.drc_scale = defaults.drc_scale,
                                         .heavy_compression = defaults.heavy_compression ? 1 : 0};
}

namespace {
ac3::DecoderConfig decoder_config_to_cpp(const ac3forge_decoder_config_t& config) {
    return ac3::DecoderConfig{.drc_scale = config.drc_scale,
                               .heavy_compression = config.heavy_compression != 0};
}
}  // namespace

ac3forge_status_t ac3forge_decoder_create(const ac3forge_decoder_config_t* config,
                                           ac3forge_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        *out_decoder = new ac3forge_decoder(decoder_config_to_cpp(*config));
        return AC3FORGE_OK;
    });
}

void ac3forge_decoder_destroy(ac3forge_decoder_t* decoder) { delete decoder; }

ac3forge_status_t ac3forge_decoder_decode_frame(ac3forge_decoder_t* decoder, const uint8_t* frame,
                                                 size_t frame_size,
                                                 ac3forge_decoded_frame_t** out_frame) {
    if (decoder == nullptr || frame == nullptr || out_frame == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        auto result = decoder->impl.decode_frame(
            std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<ac3forge_decoded_frame>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return AC3FORGE_OK;
    });
}

ac3forge_sample_rate_t ac3forge_decoded_frame_sample_rate(const ac3forge_decoded_frame_t* frame) {
    return frame == nullptr ? AC3FORGE_SAMPLE_RATE_48000 : ac3forge_c::from_cpp(frame->data.sample_rate);
}

uint32_t ac3forge_decoded_frame_bitrate_kbps(const ac3forge_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.bitrate_kbps;
}

ac3forge_acmod_t ac3forge_decoded_frame_acmod(const ac3forge_decoded_frame_t* frame) {
    return frame == nullptr ? AC3FORGE_ACMOD_2_0 : ac3forge_c::from_cpp(frame->data.acmod);
}

int ac3forge_decoded_frame_lfe(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.lfe ? 1 : 0;
}

int ac3forge_decoded_frame_dialnorm(const ac3forge_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.dialnorm;
}

int ac3forge_decoded_frame_has_compr(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr.has_value() ? 1 : 0;
}

uint8_t ac3forge_decoded_frame_compr(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr.has_value() ? *frame->data.compr : 0;
}

uint8_t ac3forge_decoded_frame_dynrng(const ac3forge_decoded_frame_t* frame, int block_index) {
    if (frame == nullptr || block_index < 0 || block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return frame->data.dynrng[static_cast<size_t>(block_index)];
}

int ac3forge_decoded_frame_has_dialnorm2(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.dialnorm2.has_value() ? 1 : 0;
}

int ac3forge_decoded_frame_dialnorm2(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.dialnorm2.has_value() ? *frame->data.dialnorm2 : 0;
}

int ac3forge_decoded_frame_has_compr2(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr2.has_value() ? 1 : 0;
}

uint8_t ac3forge_decoded_frame_compr2(const ac3forge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr2.has_value() ? *frame->data.compr2 : 0;
}

uint8_t ac3forge_decoded_frame_dynrng2(const ac3forge_decoded_frame_t* frame, int block_index) {
    if (frame == nullptr || block_index < 0 || block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return frame->data.dynrng2[static_cast<size_t>(block_index)];
}

size_t ac3forge_decoded_frame_channel_count(const ac3forge_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.channels.size();
}

size_t ac3forge_decoded_frame_samples_per_channel(const ac3forge_decoded_frame_t*) {
    return ac3::kSamplesPerFrame;
}

const float* ac3forge_decoded_frame_channel_samples(const ac3forge_decoded_frame_t* frame,
                                                      size_t channel_index) {
    if (frame == nullptr || channel_index >= frame->data.channels.size()) {
        return nullptr;
    }
    return frame->data.channels[channel_index].data();
}

int ac3forge_decoded_frame_block_switched(const ac3forge_decoded_frame_t* frame,
                                           size_t channel_index, int block_index) {
    if (frame == nullptr || channel_index >= frame->data.blksw.size() || block_index < 0 ||
        block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return frame->data.blksw[channel_index][static_cast<size_t>(block_index)] ? 1 : 0;
}

void ac3forge_decoded_frame_destroy(ac3forge_decoded_frame_t* frame) { delete frame; }

}  // extern "C"
