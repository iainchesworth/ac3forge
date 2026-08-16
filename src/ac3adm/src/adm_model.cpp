#include "adm_model.hpp"

#include <chrono>

#include <adm/adm.hpp>
#include <boost/variant.hpp>

// Clause references below are to Recommendation ITU-R BS.2076-2 (10/2019)
// Annex 1 unless stated otherwise. Every `adm::` symbol in this file is
// libadm's own (github.com/ebu/libadm); every `ac3adm::` symbol - including
// the unqualified ones, since this file lives inside namespace ac3adm::detail
// - is this module's own. See adm_model.hpp's header comment for why the two
// must never mix unqualified.

namespace ac3adm::detail {

namespace {

double to_seconds(const adm::Time& time) {
    return std::chrono::duration<double>(time.asNanoseconds()).count();
}

// Table 7/10/20/53's five typeDefinition values, plus UNDEFINED and the
// 0x1000-0xFFFF "User Custom" range libadm folds into neither - see
// adm::TypeDescriptor's own doc comment ("valid values are in the range
// [0, 5]"): libadm does not model typeLabel values above 5 as a distinct
// concept, so any TypeDescriptor this project doesn't recognize (there are
// none beyond UNDEFINED/0..5) falls back to kUserCustom.
TypeDefinition to_type_definition(const adm::TypeDescriptor& type_descriptor) {
    if (type_descriptor == adm::TypeDefinition::DIRECT_SPEAKERS) return TypeDefinition::kDirectSpeakers;
    if (type_descriptor == adm::TypeDefinition::MATRIX) return TypeDefinition::kMatrix;
    if (type_descriptor == adm::TypeDefinition::OBJECTS) return TypeDefinition::kObjects;
    if (type_descriptor == adm::TypeDefinition::HOA) return TypeDefinition::kHoa;
    if (type_descriptor == adm::TypeDefinition::BINAURAL) return TypeDefinition::kBinaural;
    if (type_descriptor == adm::TypeDefinition::UNDEFINED) return TypeDefinition::kUnknown;
    return TypeDefinition::kUserCustom;
}

// One overload per ADM element type - each type's own `id_type` typedef
// names a distinct libadm class (AudioObjectId, AudioContentId, ...), so a
// single function template keyed on the element type covers every case via
// ordinary overload resolution rather than needing a trait per element.
std::string id_of(const std::shared_ptr<const adm::AudioProgramme>& e) {
    return adm::formatId(e->get<adm::AudioProgrammeId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioContent>& e) {
    return adm::formatId(e->get<adm::AudioContentId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioObject>& e) {
    return adm::formatId(e->get<adm::AudioObjectId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioPackFormat>& e) {
    return adm::formatId(e->get<adm::AudioPackFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioChannelFormat>& e) {
    return adm::formatId(e->get<adm::AudioChannelFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioStreamFormat>& e) {
    return adm::formatId(e->get<adm::AudioStreamFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioTrackFormat>& e) {
    return adm::formatId(e->get<adm::AudioTrackFormatId>());
}
std::string id_of(const std::shared_ptr<const adm::AudioTrackUid>& e) {
    return adm::formatId(e->get<adm::AudioTrackUidId>());
}

template <typename Element>
std::optional<std::string> id_of_opt(const std::shared_ptr<const Element>& e) {
    if (!e) {
        return std::nullopt;
    }
    return id_of(e);
}

template <typename Range>
std::vector<std::string> ids_of(const Range& range) {
    std::vector<std::string> ids;
    for (const auto& element : range) {
        ids.push_back(id_of(element));
    }
    return ids;
}

// §5.4.3.1/§5.4.3.3, Tables 12/15/16: DirectSpeakers and Objects blocks both
// carry a position, but as different libadm types (SpeakerPosition vs.
// Position) with different sub-getter names - this pulls the common
// azimuth/elevation/distance / X/Y/Z shape out of whichever one a given
// block actually has.
template <typename Spherical, typename Cartesian, typename BlockFormat>
void set_position(const BlockFormat& block, AudioBlockFormat& out) {
    if (block.template has<Cartesian>()) {
        const auto cartesian = block.template get<Cartesian>();
        out.cartesian = true;
        out.position = CartesianPosition{
            .x = cartesian.template get<adm::X>().get(),
            .y = cartesian.template get<adm::Y>().get(),
            .z = cartesian.template has<adm::Z>() ? cartesian.template get<adm::Z>().get() : 0.0,
        };
    } else if (block.template has<Spherical>()) {
        const auto spherical = block.template get<Spherical>();
        out.cartesian = false;
        out.position = PolarPosition{
            .azimuth_deg = spherical.template get<adm::Azimuth>().get(),
            .elevation_deg = spherical.template get<adm::Elevation>().get(),
            .distance = spherical.template has<adm::Distance>() ? spherical.template get<adm::Distance>().get() : 1.0,
        };
    }
}

// §5.4.3.3, Table 17: Objects' `position` (unlike DirectSpeakers') is
// exposed by libadm as a single `adm::Position` boost::variant, so this
// takes the more direct isCartesian()/boost::get() path instead of
// set_position()'s has<Cartesian>()/has<Spherical>() probing.
void set_objects_position(const adm::AudioBlockFormatObjects& block, AudioBlockFormat& out) {
    if (!block.has<adm::Position>()) {
        return;
    }
    const adm::Position position = block.get<adm::Position>();
    if (adm::isCartesian(position)) {
        const auto& cartesian = boost::get<adm::CartesianPosition>(position);
        out.cartesian = true;
        out.position = CartesianPosition{
            .x = cartesian.get<adm::X>().get(),
            .y = cartesian.get<adm::Y>().get(),
            .z = cartesian.has<adm::Z>() ? cartesian.get<adm::Z>().get() : 0.0,
        };
    } else {
        const auto& spherical = boost::get<adm::SphericalPosition>(position);
        out.cartesian = false;
        out.position = PolarPosition{
            .azimuth_deg = spherical.get<adm::Azimuth>().get(),
            .elevation_deg = spherical.get<adm::Elevation>().get(),
            .distance = spherical.has<adm::Distance>() ? spherical.get<adm::Distance>().get() : 1.0,
        };
    }
}

AudioBlockFormat convert_common(const std::string& id, const adm::Rtime& rtime,
                                 const boost::optional<adm::Duration>& duration, const adm::Gain& gain,
                                 const adm::Importance& importance) {
    AudioBlockFormat block;
    block.id = id;
    block.rtime_s = to_seconds(rtime.get());
    if (duration) {
        block.has_duration = true;
        block.duration_s = to_seconds(duration->get());
    }
    block.gain = gain.asLinear();
    block.has_importance = true;
    block.importance = importance.get();
    return block;
}

AudioBlockFormat convert(const adm::AudioBlockFormatDirectSpeakers& src) {
    auto block = convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                                 src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                           : boost::none,
                                 src.get<adm::Gain>(), src.get<adm::Importance>());
    set_position<adm::SphericalSpeakerPosition, adm::CartesianSpeakerPosition>(src, block);
    for (const auto& label : src.has<adm::SpeakerLabels>() ? src.get<adm::SpeakerLabels>() : adm::SpeakerLabels{}) {
        block.speaker_labels.push_back(label.get());
    }
    return block;
}

AudioBlockFormat convert(const adm::AudioBlockFormatObjects& src) {
    auto block = convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                                 src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                           : boost::none,
                                 src.get<adm::Gain>(), src.get<adm::Importance>());
    set_objects_position(src, block);
    block.width = src.get<adm::Width>().get();
    block.height = src.get<adm::Height>().get();
    block.depth = src.get<adm::Depth>().get();
    block.diffuse = src.get<adm::Diffuse>().get();
    if (src.has<adm::ChannelLock>()) {
        const auto channel_lock = src.get<adm::ChannelLock>();
        block.has_channel_lock = true;
        block.channel_lock = channel_lock.get<adm::ChannelLockFlag>().get();
        if (channel_lock.has<adm::MaxDistance>()) {
            block.has_channel_lock_max_distance = true;
            block.channel_lock_max_distance = channel_lock.get<adm::MaxDistance>().get();
        }
    }
    if (src.has<adm::JumpPosition>()) {
        const auto jump_position = src.get<adm::JumpPosition>();
        block.has_jump_position = true;
        block.jump_position = jump_position.get<adm::JumpPositionFlag>().get();
        if (jump_position.has<adm::InterpolationLength>()) {
            block.has_interpolation_length = true;
            block.interpolation_length_s =
                std::chrono::duration<double>(jump_position.get<adm::InterpolationLength>().get()).count();
        }
    }
    return block;
}

AudioBlockFormat convert(const adm::AudioBlockFormatHoa& src) {
    auto block = convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                                 src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                           : boost::none,
                                 src.get<adm::Gain>(), src.get<adm::Importance>());
    // §5.4.3.4: order/degree are Required for HOA blocks - libadm's parser
    // already enforces that, so has_hoa_order/has_hoa_degree are set
    // unconditionally rather than guarded by has<>() the way the genuinely
    // optional fields above are.
    block.has_hoa_order = true;
    block.hoa_order = src.get<adm::Order>().get();
    block.has_hoa_degree = true;
    block.hoa_degree = src.get<adm::Degree>().get();
    block.hoa_normalization = src.get<adm::Normalization>().get();
    return block;
}

// Matrix and Binaural blocks (§5.4.3.2, §5.4.3.5) contribute only the common
// id/rtime/duration/gain/importance fields set by convert_common() -
// Matrix's own coefficient-matrix content and Binaural's near-absence of
// content are both outside this phase's scope, per model.hpp's own header
// comment.
AudioBlockFormat convert(const adm::AudioBlockFormatMatrix& src) {
    return convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                           src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                     : boost::none,
                           src.get<adm::Gain>(), src.get<adm::Importance>());
}

AudioBlockFormat convert(const adm::AudioBlockFormatBinaural& src) {
    return convert_common(adm::formatId(src.get<adm::AudioBlockFormatId>()), src.get<adm::Rtime>(),
                           src.has<adm::Duration>() ? boost::optional<adm::Duration>(src.get<adm::Duration>())
                                                     : boost::none,
                           src.get<adm::Gain>(), src.get<adm::Importance>());
}

AudioChannelFormat convert(const std::shared_ptr<const adm::AudioChannelFormat>& src) {
    AudioChannelFormat channel_format;
    channel_format.id = id_of(src);
    channel_format.name = src->get<adm::AudioChannelFormatName>().get();
    const adm::TypeDescriptor type_descriptor = src->get<adm::TypeDescriptor>();
    channel_format.type = to_type_definition(type_descriptor);

    // §5.3.2: exactly one of these five ranges is non-empty, matching
    // channel_format.type - libadm stores each typeDefinition's blocks in
    // its own internal vector (see AudioChannelFormat::getElements<T>()).
    for (const auto& block : src->getElements<adm::AudioBlockFormatDirectSpeakers>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatObjects>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatHoa>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatMatrix>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    for (const auto& block : src->getElements<adm::AudioBlockFormatBinaural>()) {
        channel_format.block_formats.push_back(convert(block));
    }
    return channel_format;
}

AudioPackFormat convert(const std::shared_ptr<const adm::AudioPackFormat>& src) {
    AudioPackFormat pack_format;
    pack_format.id = id_of(src);
    pack_format.name = src->get<adm::AudioPackFormatName>().get();
    pack_format.type = to_type_definition(src->get<adm::TypeDescriptor>());
    pack_format.channel_format_refs = ids_of(src->getReferences<adm::AudioChannelFormat>());
    pack_format.pack_format_refs = ids_of(src->getReferences<adm::AudioPackFormat>());
    return pack_format;
}

AudioStreamFormat convert(const std::shared_ptr<const adm::AudioStreamFormat>& src) {
    AudioStreamFormat stream_format;
    stream_format.id = id_of(src);
    stream_format.name = src->get<adm::AudioStreamFormatName>().get();
    stream_format.channel_format_ref = id_of_opt(src->getReference<adm::AudioChannelFormat>());
    stream_format.pack_format_ref = id_of_opt(src->getReference<adm::AudioPackFormat>());
    // §5.2's audioTrackFormatIDRef is 0..* but AudioTrackFormat is held via
    // weak_ptr on this side of the (cyclic) reference - see
    // AudioStreamFormat::getAudioTrackFormatReferences()'s own doc comment.
    for (const auto& weak_track : src->getAudioTrackFormatReferences()) {
        if (const auto track = weak_track.lock()) {
            stream_format.track_format_refs.push_back(id_of(track));
        }
    }
    return stream_format;
}

AudioTrackFormat convert(const std::shared_ptr<const adm::AudioTrackFormat>& src) {
    AudioTrackFormat track_format;
    track_format.id = id_of(src);
    track_format.name = src->get<adm::AudioTrackFormatName>().get();
    track_format.stream_format_ref = id_of_opt(src->getReference<adm::AudioStreamFormat>());
    return track_format;
}

AudioTrackUid convert(const std::shared_ptr<const adm::AudioTrackUid>& src) {
    AudioTrackUid track_uid;
    track_uid.uid = id_of(src);
    if (src->has<adm::SampleRate>()) {
        track_uid.has_sample_rate = true;
        track_uid.sample_rate = src->get<adm::SampleRate>().get();
    }
    if (src->has<adm::BitDepth>()) {
        track_uid.has_bit_depth = true;
        track_uid.bit_depth = src->get<adm::BitDepth>().get();
    }
    track_uid.track_format_ref = id_of_opt(src->getReference<adm::AudioTrackFormat>());
    track_uid.channel_format_ref = id_of_opt(src->getReference<adm::AudioChannelFormat>());
    track_uid.pack_format_ref = id_of_opt(src->getReference<adm::AudioPackFormat>());
    return track_uid;
}

AudioObject convert(const std::shared_ptr<const adm::AudioObject>& src) {
    AudioObject object;
    object.id = id_of(src);
    object.name = src->get<adm::AudioObjectName>().get();
    object.start_s = to_seconds(src->get<adm::Start>().get());
    if (src->has<adm::Duration>()) {
        object.has_duration = true;
        object.duration_s = to_seconds(src->get<adm::Duration>().get());
    }
    object.pack_format_refs = ids_of(src->getReferences<adm::AudioPackFormat>());
    object.track_uid_refs = ids_of(src->getReferences<adm::AudioTrackUid>());
    object.object_refs = ids_of(src->getReferences<adm::AudioObject>());
    return object;
}

AudioContent convert(const std::shared_ptr<const adm::AudioContent>& src) {
    AudioContent content;
    content.id = id_of(src);
    content.name = src->get<adm::AudioContentName>().get();
    content.object_refs = ids_of(src->getReferences<adm::AudioObject>());
    return content;
}

AudioProgramme convert(const std::shared_ptr<const adm::AudioProgramme>& src) {
    AudioProgramme programme;
    programme.id = id_of(src);
    programme.name = src->get<adm::AudioProgrammeName>().get();
    programme.content_refs = ids_of(src->getReferences<adm::AudioContent>());
    return programme;
}

}  // namespace

AdmModel build_adm_model(const std::shared_ptr<::adm::Document>& document) {
    AdmModel model;
    if (!document) {
        return model;
    }
    for (const auto& programme : document->getElements<adm::AudioProgramme>()) {
        model.programmes.push_back(convert(programme));
    }
    for (const auto& content : document->getElements<adm::AudioContent>()) {
        model.contents.push_back(convert(content));
    }
    for (const auto& object : document->getElements<adm::AudioObject>()) {
        model.objects.push_back(convert(object));
    }
    for (const auto& pack_format : document->getElements<adm::AudioPackFormat>()) {
        model.pack_formats.push_back(convert(pack_format));
    }
    for (const auto& channel_format : document->getElements<adm::AudioChannelFormat>()) {
        model.channel_formats.push_back(convert(channel_format));
    }
    for (const auto& stream_format : document->getElements<adm::AudioStreamFormat>()) {
        model.stream_formats.push_back(convert(stream_format));
    }
    for (const auto& track_format : document->getElements<adm::AudioTrackFormat>()) {
        model.track_formats.push_back(convert(track_format));
    }
    for (const auto& track_uid : document->getElements<adm::AudioTrackUid>()) {
        model.track_uids.push_back(convert(track_uid));
    }
    return model;
}

}  // namespace ac3adm::detail
