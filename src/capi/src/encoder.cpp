#include <memory>
#include <optional>
#include <span>

#include "internal.hpp"

using ac3forge_c::guard;
using ac3forge_c::to_cpp;

extern "C" {

void ac3forge_encoder_config_init(ac3forge_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac3::EncoderConfig defaults{};
    *config = ac3forge_encoder_config_t{
        .sample_rate = ac3forge_c::from_cpp(defaults.sample_rate),
        .bitrate_kbps = defaults.bitrate_kbps,
        .dialnorm = defaults.dialnorm,
        .has_dialnorm2 = 0,
        .dialnorm2 = 0,
        .chbwcod = defaults.chbwcod,
        .acmod = ac3forge_c::from_cpp(defaults.acmod),
        .lfe = defaults.lfe ? 1 : 0,
        .coupling = defaults.coupling ? 1 : 0,
        .cplbegf = defaults.cplbegf,
        .cplendf = defaults.cplendf,
        .fast_mdct = defaults.fast_mdct ? 1 : 0,
        .has_drc = 0,
        .drc_profile = AC3FORGE_DRC_FILM_STANDARD,
        .has_heavy = 0,
        .heavy = {},
        .has_drc2 = 0,
        .drc2_profile = AC3FORGE_DRC_FILM_STANDARD,
        .has_heavy2 = 0,
        .heavy2 = {},
        .cmixlev = ac3forge_c::from_cpp(defaults.cmixlev),
        .surmixlev = ac3forge_c::from_cpp(defaults.surmixlev)};
    ac3forge_heavy_config_init(&config->heavy);
    ac3forge_heavy_config_init(&config->heavy2);
}

namespace {

ac3::EncoderConfig encoder_config_to_cpp(const ac3forge_encoder_config_t& config) {
    ac3::EncoderConfig out;
    out.sample_rate = to_cpp(config.sample_rate);
    out.bitrate_kbps = config.bitrate_kbps;
    out.dialnorm = config.dialnorm;
    out.dialnorm2 = config.has_dialnorm2 ? std::optional<int>(config.dialnorm2) : std::nullopt;
    out.chbwcod = config.chbwcod;
    out.acmod = to_cpp(config.acmod);
    out.lfe = config.lfe != 0;
    out.coupling = config.coupling != 0;
    out.cplbegf = config.cplbegf;
    out.cplendf = config.cplendf;
    out.fast_mdct = config.fast_mdct != 0;
    out.drc = config.has_drc ? std::optional<ac3::meta::Profile>(ac3::meta::profile(to_cpp(config.drc_profile)))
                              : std::nullopt;
    out.heavy = config.has_heavy ? std::optional<ac3::meta::HeavyConfig>(to_cpp(config.heavy))
                                  : std::nullopt;
    out.drc2 = config.has_drc2
                   ? std::optional<ac3::meta::Profile>(ac3::meta::profile(to_cpp(config.drc2_profile)))
                   : std::nullopt;
    out.heavy2 = config.has_heavy2 ? std::optional<ac3::meta::HeavyConfig>(to_cpp(config.heavy2))
                                    : std::nullopt;
    out.cmixlev = to_cpp(config.cmixlev);
    out.surmixlev = to_cpp(config.surmixlev);
    return out;
}

}  // namespace

ac3forge_status_t ac3forge_encoder_create(const ac3forge_encoder_config_t* config,
                                           ac3forge_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        *out_encoder = new ac3forge_encoder(encoder_config_to_cpp(*config));
        return AC3FORGE_OK;
    });
}

void ac3forge_encoder_destroy(ac3forge_encoder_t* encoder) { delete encoder; }

size_t ac3forge_encoder_channel_count(const ac3forge_encoder_t* encoder) {
    if (encoder == nullptr) {
        return 0;
    }
    return static_cast<size_t>(encoder->impl.channel_count());
}

const uint8_t* ac3forge_bytes_data(const ac3forge_bytes_t* bytes) {
    if (bytes == nullptr || bytes->data.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(bytes->data.data());
}

size_t ac3forge_bytes_size(const ac3forge_bytes_t* bytes) {
    return bytes == nullptr ? 0 : bytes->data.size();
}

void ac3forge_bytes_destroy(ac3forge_bytes_t* bytes) { delete bytes; }

ac3forge_status_t ac3forge_encoder_encode_frame(ac3forge_encoder_t* encoder,
                                                 const float* const* channels,
                                                 size_t channel_count, size_t samples_per_channel,
                                                 ac3forge_bytes_t** out_frame) {
    if (encoder == nullptr || channels == nullptr || out_frame == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != ac3forge_encoder_channel_count(encoder) ||
        samples_per_channel != ac3::kSamplesPerFrame) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return AC3FORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        auto result = encoder->impl.encode_frame(spans);
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<ac3forge_bytes>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return AC3FORGE_OK;
    });
}

}  // extern "C"
