#include <memory>
#include <span>

#include "internal.hpp"

using ac3forge_c::guard;

static_assert(static_cast<int>(ac3::eac3::chanmap::Location::kLeft) == AC3FORGE_LOCATION_L);
static_assert(static_cast<int>(ac3::eac3::chanmap::Location::kLfe) == AC3FORGE_LOCATION_LFE);
static_assert(static_cast<int>(ac3::eac3::chanmap::kMaxChannels) == 22);

static_assert(ac3::oba::bed::kLR == AC3FORGE_BED_LR);
static_assert(ac3::oba::bed::kLfe2 == AC3FORGE_BED_LFE2);

extern "C" {

ac3forge_status_t ac3forge_eac3_decoder_create(const ac3forge_decoder_config_t* config,
                                                ac3forge_eac3_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        *out_decoder = new ac3forge_eac3_decoder(
            ac3::DecoderConfig{.drc_scale = config->drc_scale,
                               .heavy_compression = config->heavy_compression != 0});
        return AC3FORGE_OK;
    });
}

void ac3forge_eac3_decoder_destroy(ac3forge_eac3_decoder_t* decoder) { delete decoder; }

ac3forge_status_t ac3forge_eac3_decoder_decode_substream(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* frame, size_t frame_size,
    ac3forge_decoded_substream_t** out_substream) {
    if (decoder == nullptr || frame == nullptr || out_substream == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        auto result = decoder->impl.decode_substream(
            std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_substream = nullptr;
            return AC3FORGE_OK;
        }
        auto owned = std::make_unique<ac3forge_decoded_substream>();
        owned->data = std::move(**result);
        *out_substream = owned.release();
        return AC3FORGE_OK;
    });
}

ac3forge_status_t ac3forge_eac3_decoder_decode_access_unit(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    ac3forge_decoded_access_unit_t** out_unit) {
    if (decoder == nullptr || unit == nullptr || out_unit == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        auto result = decoder->impl.decode_access_unit(
            std::as_bytes(std::span<const uint8_t>(unit, unit_size)));
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_unit = nullptr;
            return AC3FORGE_OK;
        }
        auto owned = std::make_unique<ac3forge_decoded_access_unit>();
        owned->data = std::move(**result);
        *out_unit = owned.release();
        return AC3FORGE_OK;
    });
}

ac3forge_status_t ac3forge_eac3_decoder_flush(ac3forge_eac3_decoder_t* decoder,
                                               ac3forge_decoded_substream_t*** out_substreams,
                                               size_t* out_count) {
    if (decoder == nullptr || out_substreams == nullptr || out_count == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        std::vector<ac3::DecodedSubstream> flushed = decoder->impl.flush();
        if (flushed.empty()) {
            *out_substreams = nullptr;
            *out_count = 0;
            return AC3FORGE_OK;
        }
        auto array = std::make_unique<ac3forge_decoded_substream*[]>(flushed.size());
        for (size_t i = 0; i < flushed.size(); ++i) {
            auto owned = std::make_unique<ac3forge_decoded_substream>();
            owned->data = std::move(flushed[i]);
            array[i] = owned.release();
        }
        *out_count = flushed.size();
        *out_substreams = array.release();
        return AC3FORGE_OK;
    });
}

void ac3forge_decoded_substream_array_destroy(ac3forge_decoded_substream_t** substreams,
                                               size_t count) {
    if (substreams == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        delete substreams[i];
    }
    delete[] substreams;
}

// --- DecodedSubstream accessors -----------------------------------------

int ac3forge_decoded_substream_is_independent(const ac3forge_decoded_substream_t* substream) {
    // kIndependent and kConvertible both begin an access unit and decode
    // alone; only kDependent extends a preceding one (see StreamType's own
    // comment, ac3/core/eac3_tables.hpp).
    return substream != nullptr && substream->data.strmtyp != ac3::eac3::StreamType::kDependent
               ? 1
               : 0;
}

int ac3forge_decoded_substream_id(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.substreamid;
}

ac3forge_sample_rate_t ac3forge_decoded_substream_sample_rate(
    const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? AC3FORGE_SAMPLE_RATE_48000
                                 : ac3forge_c::from_cpp(substream->data.sample_rate);
}

ac3forge_acmod_t ac3forge_decoded_substream_acmod(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? AC3FORGE_ACMOD_2_0 : ac3forge_c::from_cpp(substream->data.acmod);
}

int ac3forge_decoded_substream_lfe(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.lfe ? 1 : 0;
}

int ac3forge_decoded_substream_dialnorm(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.dialnorm;
}

int ac3forge_decoded_substream_has_compr(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr.has_value() ? 1 : 0;
}

uint8_t ac3forge_decoded_substream_compr(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr.has_value() ? *substream->data.compr : 0;
}

uint8_t ac3forge_decoded_substream_dynrng(const ac3forge_decoded_substream_t* substream,
                                           int block_index) {
    if (substream == nullptr || block_index < 0 || block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return substream->data.dynrng[static_cast<size_t>(block_index)];
}

int ac3forge_decoded_substream_has_dialnorm2(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.dialnorm2.has_value() ? 1 : 0;
}

int ac3forge_decoded_substream_dialnorm2(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.dialnorm2.has_value() ? *substream->data.dialnorm2
                                                                          : 0;
}

int ac3forge_decoded_substream_has_compr2(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr2.has_value() ? 1 : 0;
}

uint8_t ac3forge_decoded_substream_compr2(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr2.has_value() ? *substream->data.compr2 : 0;
}

uint8_t ac3forge_decoded_substream_dynrng2(const ac3forge_decoded_substream_t* substream,
                                            int block_index) {
    if (substream == nullptr || block_index < 0 || block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return substream->data.dynrng2[static_cast<size_t>(block_index)];
}

int ac3forge_decoded_substream_numblkscod(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.numblkscod;
}

int ac3forge_decoded_substream_has_chanmap(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.chanmap.has_value() ? 1 : 0;
}

uint16_t ac3forge_decoded_substream_chanmap(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.chanmap.has_value() ? *substream->data.chanmap
                                                                        : 0;
}

int ac3forge_decoded_substream_last_dependent(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.last_dependent ? 1 : 0;
}

uint16_t ac3forge_decoded_substream_location_map(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.location_map();
}

size_t ac3forge_decoded_substream_channel_count(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.channels.size();
}

size_t ac3forge_decoded_substream_samples_per_channel(const ac3forge_decoded_substream_t*) {
    return ac3::kSamplesPerFrame;
}

const float* ac3forge_decoded_substream_channel_samples(
    const ac3forge_decoded_substream_t* substream, size_t channel_index) {
    if (substream == nullptr || channel_index >= substream->data.channels.size()) {
        return nullptr;
    }
    return substream->data.channels[channel_index].data();
}

int ac3forge_decoded_substream_block_switched(const ac3forge_decoded_substream_t* substream,
                                               size_t channel_index, int block_index) {
    if (substream == nullptr || channel_index >= substream->data.blksw.size() || block_index < 0 ||
        block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return substream->data.blksw[channel_index][static_cast<size_t>(block_index)] ? 1 : 0;
}

int ac3forge_decoded_substream_has_object_metadata(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value() ? 1 : 0;
}

int ac3forge_decoded_substream_program_dynamic_only(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value() &&
                   substream->data.object_metadata->program.dynamic_only
               ? 1
               : 0;
}

int ac3forge_decoded_substream_program_lfe(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value() &&
                   substream->data.object_metadata->program.lfe
               ? 1
               : 0;
}

uint16_t ac3forge_decoded_substream_program_bed(const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value()
               ? substream->data.object_metadata->program.bed
               : 0;
}

int ac3forge_decoded_substream_program_dynamic_object_count(
    const ac3forge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value()
               ? substream->data.object_metadata->program.dynamic_objects
               : 0;
}

void ac3forge_decoded_substream_dynamic_object(const ac3forge_decoded_substream_t* substream,
                                                int object_index, double* out_x, double* out_y,
                                                double* out_z, double* out_gain_db) {
    if (out_x != nullptr) *out_x = 0.5;
    if (out_y != nullptr) *out_y = 0.5;
    if (out_z != nullptr) *out_z = 0.0;
    if (out_gain_db != nullptr) *out_gain_db = 0.0;
    if (substream == nullptr || !substream->data.object_metadata.has_value() || object_index < 0) {
        return;
    }
    const auto& objects = substream->data.object_metadata->objects;
    if (static_cast<size_t>(object_index) >= objects.size()) {
        return;
    }
    const auto& object = objects[static_cast<size_t>(object_index)];
    if (out_x != nullptr) *out_x = object.position.x;
    if (out_y != nullptr) *out_y = object.position.y;
    if (out_z != nullptr) *out_z = object.position.z;
    if (out_gain_db != nullptr) *out_gain_db = object.gain_db;
}

size_t ac3forge_decoded_substream_object_audio_count(const ac3forge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.object_audio.size();
}

const float* ac3forge_decoded_substream_object_audio(const ac3forge_decoded_substream_t* substream,
                                                       size_t object_index) {
    if (substream == nullptr || object_index >= substream->data.object_audio.size()) {
        return nullptr;
    }
    return substream->data.object_audio[object_index].data();
}

void ac3forge_decoded_substream_destroy(ac3forge_decoded_substream_t* substream) {
    delete substream;
}

// --- DecodedAccessUnit accessors ----------------------------------------

ac3forge_sample_rate_t ac3forge_decoded_access_unit_sample_rate(
    const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? AC3FORGE_SAMPLE_RATE_48000 : ac3forge_c::from_cpp(unit->data.sample_rate);
}

ac3forge_acmod_t ac3forge_decoded_access_unit_acmod(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? AC3FORGE_ACMOD_2_0 : ac3forge_c::from_cpp(unit->data.acmod);
}

int ac3forge_decoded_access_unit_dialnorm(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.dialnorm;
}

int ac3forge_decoded_access_unit_has_compr(const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.compr.has_value() ? 1 : 0;
}

uint8_t ac3forge_decoded_access_unit_compr(const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.compr.has_value() ? *unit->data.compr : 0;
}

uint8_t ac3forge_decoded_access_unit_dynrng(const ac3forge_decoded_access_unit_t* unit,
                                             int block_index) {
    if (unit == nullptr || block_index < 0 || block_index >= ac3::kBlocksPerFrame) {
        return 0;
    }
    return unit->data.dynrng[static_cast<size_t>(block_index)];
}

int ac3forge_decoded_access_unit_numblkscod(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.numblkscod;
}

int ac3forge_decoded_access_unit_substream_count(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.substream_count;
}

size_t ac3forge_decoded_access_unit_channel_count(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.channels.size();
}

size_t ac3forge_decoded_access_unit_samples_per_channel(const ac3forge_decoded_access_unit_t*) {
    return ac3::kSamplesPerFrame;
}

const float* ac3forge_decoded_access_unit_channel_samples(const ac3forge_decoded_access_unit_t* unit,
                                                            size_t channel_index) {
    if (unit == nullptr || channel_index >= unit->data.channels.size()) {
        return nullptr;
    }
    return unit->data.channels[channel_index].data();
}

size_t ac3forge_decoded_access_unit_layout_count(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : static_cast<size_t>(unit->data.layout.count);
}

ac3forge_location_t ac3forge_decoded_access_unit_layout_location(
    const ac3forge_decoded_access_unit_t* unit, size_t index) {
    if (unit == nullptr || index >= static_cast<size_t>(unit->data.layout.count)) {
        return AC3FORGE_LOCATION_L;
    }
    return static_cast<ac3forge_location_t>(unit->data.layout[static_cast<int>(index)]);
}

int ac3forge_decoded_access_unit_has_object_metadata(const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value() ? 1 : 0;
}

int ac3forge_decoded_access_unit_program_dynamic_only(const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value() &&
                   unit->data.object_metadata->program.dynamic_only
               ? 1
               : 0;
}

int ac3forge_decoded_access_unit_program_lfe(const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value() &&
                   unit->data.object_metadata->program.lfe
               ? 1
               : 0;
}

uint16_t ac3forge_decoded_access_unit_program_bed(const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value()
               ? unit->data.object_metadata->program.bed
               : 0;
}

int ac3forge_decoded_access_unit_program_dynamic_object_count(
    const ac3forge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value()
               ? unit->data.object_metadata->program.dynamic_objects
               : 0;
}

void ac3forge_decoded_access_unit_dynamic_object(const ac3forge_decoded_access_unit_t* unit,
                                                  int object_index, double* out_x, double* out_y,
                                                  double* out_z, double* out_gain_db) {
    if (out_x != nullptr) *out_x = 0.5;
    if (out_y != nullptr) *out_y = 0.5;
    if (out_z != nullptr) *out_z = 0.0;
    if (out_gain_db != nullptr) *out_gain_db = 0.0;
    if (unit == nullptr || !unit->data.object_metadata.has_value() || object_index < 0) {
        return;
    }
    const auto& objects = unit->data.object_metadata->objects;
    if (static_cast<size_t>(object_index) >= objects.size()) {
        return;
    }
    const auto& object = objects[static_cast<size_t>(object_index)];
    if (out_x != nullptr) *out_x = object.position.x;
    if (out_y != nullptr) *out_y = object.position.y;
    if (out_z != nullptr) *out_z = object.position.z;
    if (out_gain_db != nullptr) *out_gain_db = object.gain_db;
}

size_t ac3forge_decoded_access_unit_object_audio_count(const ac3forge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.object_audio.size();
}

const float* ac3forge_decoded_access_unit_object_audio(const ac3forge_decoded_access_unit_t* unit,
                                                         size_t object_index) {
    if (unit == nullptr || object_index >= unit->data.object_audio.size()) {
        return nullptr;
    }
    return unit->data.object_audio[object_index].data();
}

void ac3forge_decoded_access_unit_destroy(ac3forge_decoded_access_unit_t* unit) { delete unit; }

// --- stream framing helpers ----------------------------------------------

size_t ac3forge_spans_count(const ac3forge_spans_t* spans) {
    return spans == nullptr ? 0 : spans->items.size();
}

ac3forge_span_t ac3forge_spans_get(const ac3forge_spans_t* spans, size_t index) {
    if (spans == nullptr || index >= spans->items.size()) {
        return ac3forge_span_t{0, 0};
    }
    return spans->items[index];
}

void ac3forge_spans_destroy(ac3forge_spans_t* spans) { delete spans; }

namespace {

ac3forge_status_t split_into_spans(const uint8_t* stream, size_t stream_size,
                                    ac3forge_spans_t** out_spans, bool access_units) {
    if (stream == nullptr || out_spans == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        const auto bytes = std::as_bytes(std::span<const uint8_t>(stream, stream_size));
        auto result = access_units ? ac3::split_access_units(bytes) : ac3::split_frames(bytes);
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<ac3forge_spans>();
        owned->items.reserve(result->size());
        for (const auto& span : *result) {
            const auto offset = static_cast<size_t>(span.data() - reinterpret_cast<const std::byte*>(
                                                                        static_cast<const void*>(stream)));
            owned->items.push_back(ac3forge_span_t{offset, span.size()});
        }
        *out_spans = owned.release();
        return AC3FORGE_OK;
    });
}

}  // namespace

ac3forge_status_t ac3forge_split_frames(const uint8_t* stream, size_t stream_size,
                                         ac3forge_spans_t** out_spans) {
    return split_into_spans(stream, stream_size, out_spans, /*access_units=*/false);
}

ac3forge_status_t ac3forge_split_access_units(const uint8_t* stream, size_t stream_size,
                                               ac3forge_spans_t** out_spans) {
    return split_into_spans(stream, stream_size, out_spans, /*access_units=*/true);
}

ac3forge_status_t ac3forge_stream_bsid(const uint8_t* frame, size_t frame_size, int* out_bsid) {
    if (frame == nullptr || out_bsid == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        auto result = ac3::stream_bsid(std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        *out_bsid = *result;
        return AC3FORGE_OK;
    });
}

}  // extern "C"
