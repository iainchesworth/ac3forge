#include "ac3/admbridge/bridge.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

#include "ac3/admbridge/coordinates.hpp"

namespace ac3::admbridge {

std::string_view describe(BridgeError error) {
    switch (error) {
        case BridgeError::kNoProgramme: return "the ADM model has no audioProgramme";
        case BridgeError::kProgrammeNotFound: return "no audioProgramme with the requested ID";
        case BridgeError::kUnresolvedReference: return "an ID reference did not resolve";
        case BridgeError::kObjectReferenceCycle: return "nested audioObject references form a loop";
        case BridgeError::kUnsupportedType:
            return "a resolved audioPackFormat is not DirectSpeakers or Objects, its type "
                   "disagrees with a sibling pack, or it nests further audioPackFormats";
        case BridgeError::kChannelTrackMismatch:
            return "audioTrackUIDRef count does not match the resolved channel count";
        case BridgeError::kNoAudioForTrack: return "a resolved chna track has no PCM audio";
        case BridgeError::kEmptyBlockSequence: return "an audioChannelFormat has no audioBlockFormat";
        case BridgeError::kTooManyChannels: return "more channels than AtmosEncoder supports";
    }
    return "unknown ac3::admbridge::BridgeError";
}

namespace {

// A real Dirac/instantaneous jump has no representation in KeyframePath's piecewise-linear model
// (two keyframes cannot share one time_s - see ac3::oba::PathError::kDuplicateTimestamp). This is
// the same resolution test_atmos_motion.cpp's own make_holds() helper relies on implicitly: every
// caller in this codebase samples ObjectPath::evaluate() once per encoded frame
// (ac3::kSamplesPerFrame = 1536 samples, 32 ms at 48 kHz - see ac3::oba::AtmosEncoder::
// encode_frame's own doc comment, "one placement per frame"), so any transition faster than one
// frame period is already indistinguishable from instantaneous at the resolution that actually
// reaches the bitstream. 1 microsecond is roughly 1/20 of one 48 kHz sample and about six orders
// of magnitude below a 32 ms frame - far too small for any real frame boundary to land inside it,
// and far smaller than any interpolationLength BS.2076-2 §10.3 itself expects a producer to
// author ("It is recommended that audioBlockFormat sizes are chosen to be small enough to avoid
// the use of the interpolationLength parameter for smoothly moving objects" - i.e. this parameter
// is meant for short but audible crossfades, not zero).
constexpr double kInstantJumpEpsilon = 1.0e-6;

}  // namespace

std::expected<ac3::oba::ObjectPath, BridgeError> build_channel_path(
    const ac3adm::AudioChannelFormat& channel, double object_start_s, bool force_lfe) {
    if (channel.block_formats.empty()) {
        return std::unexpected(BridgeError::kEmptyBlockSequence);
    }

    // §10.3's interpolatable parameters bundled into one Keyframe per breakpoint - position and
    // gain move together, governed by the same block's own jumpPosition/interpolationLength (see
    // build_channel_path's own header comment). force_lfe discards the block's real data
    // entirely: an LFE bed channel has no direction to pin (ac3/oba/atmos.hpp: "Objects never
    // reach the LFE by panning"), so it reaches the bed only via lfe_send.
    const auto placement_of = [&](const ac3adm::AudioBlockFormat& block)
        -> std::pair<ac3::oba::Position, double> {
        if (force_lfe) {
            return {ac3::oba::Position{}, 0.0};
        }
        return {adm_position_to_room(block.position), block.gain};
    };
    const double lfe_send = force_lfe ? 1.0 : 0.0;

    std::vector<ac3::oba::Keyframe> keyframes;
    keyframes.reserve(channel.block_formats.size() * 2);

    // Monotonically-increasing insertion with a minimum spacing of kInstantJumpEpsilon - the one
    // mechanism that turns every §10.3 case (an instant jump, a jump immediately following a
    // zero-length "make a position jump" block per Fig. 10, or simply two blocks abutting at the
    // same nominal time) into a valid, strictly-increasing keyframe sequence without needing a
    // separate branch for each. See kInstantJumpEpsilon's own comment for why this is inaudible
    // at the resolution that reaches the bitstream.
    const auto push_keyframe = [&](double time_s, ac3::oba::Position position, double gain) {
        if (!keyframes.empty() && time_s <= keyframes.back().time_s) {
            time_s = keyframes.back().time_s + kInstantJumpEpsilon;
        }
        keyframes.push_back(
            {.time_s = time_s, .position = position, .gain = gain, .lfe_send = lfe_send});
    };

    if (channel.block_formats.size() == 1) {
        // §5.4.1: "If there is only one audioBlockFormat within an audioChannelFormat, the
        // characteristics of the parent audioChannelFormat are considered to be static over
        // time" - one keyframe, which ac3::oba::KeyframePath already holds everywhere.
        const auto& block = channel.block_formats.front();
        const auto [position, gain] = placement_of(block);
        push_keyframe(object_start_s + block.rtime_s, position, gain);
    } else {
        for (std::size_t i = 0; i < channel.block_formats.size(); ++i) {
            const auto& block = channel.block_formats[i];
            const double t_start = object_start_s + block.rtime_s;
            const bool has_end = block.has_duration;
            const double t_end = t_start + block.duration_s;
            const auto [position, gain] = placement_of(block);

            if (i == 0) {
                // §10.3: "the position specified in the first block covers the entire length of
                // the block (regardless of the jumpPosition and interpolationLength properties)".
                push_keyframe(t_start, position, gain);
                // A non-final block omitting duration is legal (§5.4.1 only "should" - not
                // "must" - pair rtime with duration once a channel has more than one block) but
                // discouraged, and its true end is the NEXT block's own start, not "forever":
                // channel.block_formats[1] always exists here (this whole branch only runs when
                // size() > 1). Without this, the hold-keyframe below would be skipped entirely
                // and block 1's own ramp/jump would be computed relative to this single
                // keyframe, silently stretching its interpolation span back to object_start_s
                // instead of block 1's own duration.
                const double effective_end =
                    has_end ? t_end : object_start_s + channel.block_formats[1].rtime_s;
                if (effective_end > t_start) {
                    push_keyframe(effective_end, position, gain);
                }
                continue;
            }

            if (!block.jump_position) {
                // Ramp across the FULL block duration, continuous from whatever the previous
                // block's own last keyframe already pinned at this same t_start.
                push_keyframe(has_end ? t_end : t_start, position, gain);
                continue;
            }

            // jumpPosition = 1: a ramp of interpolationLength (default/absent = as close to
            // instant as this representation allows) at the block's start, then a hold.
            const double length =
                block.has_interpolation_length ? std::max(block.interpolation_length_s, 0.0) : 0.0;
            const double ramp_end = has_end ? std::min(t_start + length, t_end) : t_start + length;
            push_keyframe(ramp_end, position, gain);
            if (has_end && t_end > ramp_end) {
                push_keyframe(t_end, position, gain);
            }
        }
    }

    auto created = ac3::oba::KeyframePath::create(std::move(keyframes));
    if (!created) {
        // Unreachable in practice - push_keyframe's own monotonic nudge guarantees a strictly
        // increasing sequence, and it is never called with an empty channel.block_formats (the
        // kEmptyBlockSequence check above already rejected that) - kept as a real, checked error
        // rather than an assert because it is cheap to check and this boundary parses untrusted
        // file-derived data.
        return std::unexpected(BridgeError::kEmptyBlockSequence);
    }
    return ac3::oba::ObjectPath(std::move(*created));
}

namespace {

template <typename T>
const T* find_by_id(const std::vector<T>& elements, std::string_view id) {
    const auto it = std::ranges::find_if(elements, [&](const T& e) { return e.id == id; });
    return it == elements.end() ? nullptr : &*it;
}

// BS.2076-2 Table 12's own speakerLabel values for a low-frequency-effects channel - "LFE" for a
// single-LFE bed, "LFE1"/"LFE2" for a dual-LFE one (the standard's own 9.1/7.1.2 worked examples
// use both forms).
bool is_lfe_label(const std::string& label) {
    return label == "LFE" || label == "LFE1" || label == "LFE2";
}

bool channel_is_lfe(const ac3adm::AudioChannelFormat& channel) {
    for (const auto& block : channel.block_formats) {
        if (std::ranges::any_of(block.speaker_labels, is_lfe_label)) {
            return true;
        }
    }
    return false;
}

struct ClassifiedObject {
    bool is_bed = false;
    std::vector<const ac3adm::AudioChannelFormat*> channels;
};

// Resolves one audioObject's pack_format_refs down to an ordered list of audioChannelFormats and
// classifies it as a bed (every resolved pack is DirectSpeakers) or a dynamic object (every
// resolved pack is Objects) - see bridge.hpp's own top comment for what happens to every other
// TypeDefinition and to nested audioPackFormats.
std::expected<ClassifiedObject, BridgeError> classify_object(const ac3adm::AdmModel& model,
                                                              const ac3adm::AudioObject& object) {
    ClassifiedObject result;
    std::optional<ac3adm::TypeDefinition> agreed_type;
    for (const auto& pack_ref : object.pack_format_refs) {
        const auto* pack = find_by_id(model.pack_formats, pack_ref);
        if (pack == nullptr) {
            return std::unexpected(BridgeError::kUnresolvedReference);
        }
        if (!pack->pack_format_refs.empty()) {
            // Nested audioPackFormat (§5.5's own pack_format_refs sub-reference - Matrix decode/
            // HOA layouts nest packs this way) - out of this phase's scope, see bridge.hpp's own
            // top comment.
            return std::unexpected(BridgeError::kUnsupportedType);
        }
        if (pack->type != ac3adm::TypeDefinition::kDirectSpeakers &&
            pack->type != ac3adm::TypeDefinition::kObjects) {
            return std::unexpected(BridgeError::kUnsupportedType);
        }
        if (agreed_type.has_value() && *agreed_type != pack->type) {
            return std::unexpected(BridgeError::kUnsupportedType);
        }
        agreed_type = pack->type;
        for (const auto& channel_ref : pack->channel_format_refs) {
            const auto* channel = find_by_id(model.channel_formats, channel_ref);
            if (channel == nullptr) {
                return std::unexpected(BridgeError::kUnresolvedReference);
            }
            result.channels.push_back(channel);
        }
    }
    result.is_bed = (agreed_type == ac3adm::TypeDefinition::kDirectSpeakers);
    return result;
}

// Depth-first walk from one audioProgramme's own audioContents down through every audioObject
// they reference, following nested audioObject::object_refs (§5.6: "AudioObjects can be nested
// and so they can refer to other audioObjects") and collecting every object that carries its own
// pack_format_refs (a "leaf" with actual audio to place - a pure container object that only
// nests further objects contributes nothing of its own). `visiting` guards against a reference
// cycle (§5.6.7: "An audioObject element should not reference itself, nor can a loop of
// references be used").
std::expected<std::vector<const ac3adm::AudioObject*>, BridgeError> collect_leaf_objects(
    const ac3adm::AdmModel& model, const ac3adm::AudioProgramme& programme) {
    std::vector<const ac3adm::AudioObject*> objects;
    std::vector<std::string> visiting;

    const std::function<std::expected<void, BridgeError>(const std::string&)> visit =
        [&](const std::string& object_id) -> std::expected<void, BridgeError> {
        if (std::ranges::find(visiting, object_id) != visiting.end()) {
            return std::unexpected(BridgeError::kObjectReferenceCycle);
        }
        const auto* object = find_by_id(model.objects, object_id);
        if (object == nullptr) {
            return std::unexpected(BridgeError::kUnresolvedReference);
        }
        if (!object->pack_format_refs.empty()) {
            objects.push_back(object);
        }
        visiting.push_back(object_id);
        for (const auto& child_id : object->object_refs) {
            if (const auto child_result = visit(child_id); !child_result) {
                return child_result;
            }
        }
        visiting.pop_back();
        return {};
    };

    for (const auto& content_ref : programme.content_refs) {
        const auto* content = find_by_id(model.contents, content_ref);
        if (content == nullptr) {
            return std::unexpected(BridgeError::kUnresolvedReference);
        }
        for (const auto& object_ref : content->object_refs) {
            if (const auto result = visit(object_ref); !result) {
                return std::unexpected(result.error());
            }
        }
    }
    return objects;
}

// TS 103 420 §8.3.2.2 caps complexity_index_type_a (== object_count(program), see ac3/oba/
// oamd.hpp) at 16 total, with AtmosEncoder's own implicit LFE bookkeeping (see atmos.cpp's
// AtmosEncoder::AtmosEncoder: `Program{.dynamic_only = true, .lfe = true, ...}`) always taking
// one of those 16 regardless of what this bridge produces - the same cap and the same reasoning
// apps/cli/main.cpp's run_atmos_encode/run_atmos_path already apply to the `objects` constructor
// parameter, reused here rather than re-derived.
constexpr std::size_t kMaxChannels = 15;

}  // namespace

std::expected<BridgeResult, BridgeError> build(const ac3adm::AdmDocument& document,
                                                std::string_view programme_id) {
    const auto& model = document.model;
    if (model.programmes.empty()) {
        return std::unexpected(BridgeError::kNoProgramme);
    }

    const ac3adm::AudioProgramme* programme = nullptr;
    if (programme_id.empty()) {
        // §5.8: IDs are formatted strings (APR_wwww, fixed-width hex), so a lexicographic
        // compare over them is a numeric-ID compare too.
        programme = &*std::ranges::min_element(model.programmes, std::ranges::less{},
                                               &ac3adm::AudioProgramme::id);
    } else {
        programme = find_by_id(model.programmes, programme_id);
        if (programme == nullptr) {
            return std::unexpected(BridgeError::kProgrammeNotFound);
        }
    }

    const auto leaf_objects = collect_leaf_objects(model, *programme);
    if (!leaf_objects) {
        return std::unexpected(leaf_objects.error());
    }

    BridgeResult out;
    out.sample_rate = document.audio.sample_rate;

    for (const auto* object : *leaf_objects) {
        const auto classified = classify_object(model, *object);
        if (!classified) {
            return std::unexpected(classified.error());
        }
        if (classified->channels.size() != object->track_uid_refs.size()) {
            return std::unexpected(BridgeError::kChannelTrackMismatch);
        }

        for (std::size_t i = 0; i < classified->channels.size(); ++i) {
            const auto* channel = classified->channels[i];
            const auto& track_uid_ref = object->track_uid_refs[i];

            const auto chna_it = std::ranges::find(document.chna, track_uid_ref,
                                                    &ac3adm::ChnaEntry::uid);
            if (chna_it == document.chna.end()) {
                return std::unexpected(BridgeError::kUnresolvedReference);
            }
            if (chna_it->track_index == 0 ||
                static_cast<std::size_t>(chna_it->track_index) > document.audio.channels.size()) {
                return std::unexpected(BridgeError::kNoAudioForTrack);
            }

            const bool is_lfe = classified->is_bed && channel_is_lfe(*channel);
            auto path = build_channel_path(*channel, object->start_s, is_lfe);
            if (!path) {
                return std::unexpected(path.error());
            }

            out.channel_ids.push_back(channel->id);
            out.is_bed.push_back(classified->is_bed);
            out.is_lfe.push_back(is_lfe);
            out.paths.push_back(std::move(*path));
            out.pcm.emplace_back(
                document.audio.channels[static_cast<std::size_t>(chna_it->track_index) - 1]);
        }
    }

    if (out.paths.size() > kMaxChannels) {
        return std::unexpected(BridgeError::kTooManyChannels);
    }
    return out;
}

}  // namespace ac3::admbridge
