#include <memory>
#include <span>
#include <vector>

#include "internal.hpp"

using ac3forge_c::guard;

// Kept outside extern "C" below - see encoder.cpp's identical comment on
// -Wreturn-type-c-linkage.
namespace {
ac3::oba::AtmosConfig atmos_config_to_cpp(const ac3forge_atmos_config_t& config) {
    return ac3::oba::AtmosConfig{.sample_rate = ac3forge_c::to_cpp(config.sample_rate),
                                  .bitrate_kbps = config.bitrate_kbps,
                                  .dialnorm = config.dialnorm,
                                  .num_bands_idx = config.num_bands_idx,
                                  .fine_quant = config.fine_quant != 0,
                                  .emit_object_metadata = config.emit_object_metadata != 0,
                                  .fast_mdct = config.fast_mdct != 0};
}
}  // namespace

extern "C" {

void ac3forge_atmos_config_init(ac3forge_atmos_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac3::oba::AtmosConfig defaults{};
    *config = ac3forge_atmos_config_t{.sample_rate = ac3forge_c::from_cpp(defaults.sample_rate),
                                       .bitrate_kbps = defaults.bitrate_kbps,
                                       .dialnorm = defaults.dialnorm,
                                       .num_bands_idx = defaults.num_bands_idx,
                                       .fine_quant = defaults.fine_quant ? 1 : 0,
                                       .emit_object_metadata = defaults.emit_object_metadata ? 1 : 0,
                                       .fast_mdct = defaults.fast_mdct ? 1 : 0};
}

ac3forge_status_t ac3forge_atmos_encoder_create(const ac3forge_atmos_config_t* config,
                                                 int object_count,
                                                 ac3forge_atmos_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr || object_count < 0) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        *out_encoder = new ac3forge_atmos_encoder(atmos_config_to_cpp(*config), object_count);
        return AC3FORGE_OK;
    });
}

void ac3forge_atmos_encoder_destroy(ac3forge_atmos_encoder_t* encoder) { delete encoder; }

int ac3forge_atmos_encoder_dynamic_object_count(const ac3forge_atmos_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.dynamic_object_count();
}

ac3forge_status_t ac3forge_atmos_encoder_encode_frame(
    ac3forge_atmos_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const ac3forge_object_placement_t* placements,
    size_t placement_count, ac3forge_bytes_t** out_unit) {
    if (encoder == nullptr || out_unit == nullptr ||
        (object_count > 0 && (objects == nullptr || placements == nullptr))) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    if (object_count != static_cast<size_t>(ac3forge_atmos_encoder_dynamic_object_count(encoder)) ||
        placement_count != object_count || samples_per_object != ac3::kSamplesPerFrame) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        std::vector<std::span<const float>> object_spans;
        object_spans.reserve(object_count);
        for (size_t i = 0; i < object_count; ++i) {
            if (objects[i] == nullptr) {
                return AC3FORGE_ERROR_INVALID_ARGUMENT;
            }
            object_spans.emplace_back(objects[i], samples_per_object);
        }
        std::vector<ac3::oba::ObjectPlacement> placement_values;
        placement_values.reserve(placement_count);
        for (size_t i = 0; i < placement_count; ++i) {
            const auto& p = placements[i];
            placement_values.push_back(ac3::oba::ObjectPlacement{
                .position = ac3::oba::Position{.x = p.x, .y = p.y, .z = p.z},
                .gain = p.gain,
                .lfe_send = p.lfe_send});
        }
        auto result = encoder->impl.encode_frame(object_spans, placement_values);
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<ac3forge_bytes>();
        owned->data = std::move(result->bytes);
        *out_unit = owned.release();
        return AC3FORGE_OK;
    });
}

}  // extern "C"
