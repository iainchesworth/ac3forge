#include "ac3/mlp/atmos.hpp"

#include <array>
#include <cassert>

#include "ac3/emdf/emdf.hpp"
#include "ac3/mlp/sync.hpp"

namespace ac3::mlp {

namespace {

// Table 18's bit i names the same channel(s) as OAMD Table 12's bit 9-i,
// label for label (L/R down to LFE2) - so the OAMD bed mask is this exact
// 10-bit reversal, and the implied bed-channel ORDER (most-significant OAMD
// bit first) comes out identical to the 16ch presentation's wire order too.
[[nodiscard]] std::uint16_t oamd_bed_mask(std::uint16_t bed16_mask) {
    std::uint16_t out = 0;
    for (int bit = 0; bit < 10; ++bit) {
        if ((bed16_mask >> bit) & 1) {
            out |= static_cast<std::uint16_t>(1 << (9 - bit));
        }
    }
    return out;
}

[[nodiscard]] SixteenChannelMeaning make_meaning(const AtmosConfig& config, int channel_count) {
    SixteenChannelMeaning meaning;
    meaning.dialogue_norm = config.dialogue_norm;
    meaning.mix_level = config.mix_level;
    meaning.channel_count = channel_count;
    if (config.bed == 0 || (config.bed == bed16::kLfe && config.dynamic_objects > 0)) {
        // §4.4.4/§4.4.5's efficient form: everything full-bandwidth is a
        // dynamic object, with at most a leading LFE.
        meaning.dyn_object_only = true;
        meaning.lfe_present = config.bed == bed16::kLfe;
        return meaning;
    }
    meaning.dyn_object_only = false;
    meaning.content_description =
        static_cast<std::uint8_t>(0b001 | (config.dynamic_objects > 0 ? 0b100 : 0));
    meaning.lfe_only = config.bed == bed16::kLfe;
    if (!meaning.lfe_only) {
        meaning.channel_assignment = config.bed;
    }
    meaning.dynamic_object_count = config.dynamic_objects;
    return meaning;
}

[[nodiscard]] StreamConfig make_stream_config(const AtmosConfig& config, int channel_count) {
    StreamConfig stream;
    stream.sample_rate = config.sample_rate;
    stream.wordlength = config.wordlength;
    stream.major_sync_interval = config.major_sync_interval;
    stream.channels = channel_count;
    stream.automatic = true;
    stream.peak_data_rate_16ths = config.peak_data_rate_16ths;
    stream.sixteen_channel = make_meaning(config, channel_count);
    return stream;
}

[[nodiscard]] oba::Program make_program(const AtmosConfig& config) {
    if (config.bed == 0 || config.bed == bed16::kLfe) {
        return {.dynamic_only = true,
                .lfe = config.bed == bed16::kLfe,
                .bed = 0,
                .dynamic_objects = config.dynamic_objects};
    }
    return {.dynamic_only = false,
            .lfe = false,
            .bed = oamd_bed_mask(config.bed),
            .dynamic_objects = config.dynamic_objects};
}

}  // namespace

AtmosEncoder::AtmosEncoder(const AtmosConfig& config)
    : config_(config),
      channel_count_(sixteen_channel_assignment_count(config.bed) + config.dynamic_objects),
      program_(make_program(config)),
      encoder_(make_stream_config(config, channel_count_)) {
    assert(config.dynamic_objects >= 0);
    assert(config.bed != 0 || config.dynamic_objects >= 1);
    assert(channel_count_ >= 1 && channel_count_ <= 16);
    assert(config.metadata_interval >= 1);
    // §5.5.2's payload builder needs at least one object in the programme;
    // a bed with no dynamic objects still describes its feeds there.
    assert(oba::object_count(program_) >= 1 && oba::object_count(program_) <= 31);
}

std::vector<std::byte> AtmosEncoder::metadata(std::span<const oba::DynamicObject> objects) {
    assert(objects.size() == static_cast<std::size_t>(config_.dynamic_objects));
    if (unit_index_ % static_cast<std::uint64_t>(config_.metadata_interval) != 0) {
        return {};
    }
    const auto oamd = oba::build_payload(program_, objects);
    const std::array<emdf::Payload, 1> payloads{
        emdf::Payload{emdf::kPayloadIdOamd, oamd}};
    return emdf::build_container(payloads);
}

std::vector<std::byte> AtmosEncoder::encode_access_unit(
    std::span<const std::span<const std::int32_t>> channels,
    std::span<const oba::DynamicObject> objects) {
    const auto container = metadata(objects);
    ++unit_index_;
    return encoder_.encode_access_unit(channels, container);
}

std::vector<std::byte> AtmosEncoder::encode_access_unit(
    std::span<const std::span<const std::int32_t>> channels,
    std::span<const oba::DynamicObject> objects, EndOfStream end) {
    const auto container = metadata(objects);
    ++unit_index_;
    return encoder_.encode_access_unit(channels, end, container);
}

}  // namespace ac3::mlp
