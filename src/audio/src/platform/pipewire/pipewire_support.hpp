#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "ac3/sinks/passthrough.hpp"

// The handful of things capture.cpp, monitor.cpp and passthrough.cpp all need
// from libpipewire, kept in one place so the three cannot drift - the same
// role alsa_support.hpp plays for the ALSA backend.
//
// PipeWire's object graph has no single ownership convention the way ALSA's
// malloc/free-pair structs do: pw_core_disconnect() returns an int,
// pw_proxy_destroy() is void and doubles as pw_registry's own destructor
// (a registry IS a proxy), and pw_stream/pw_thread_loop/pw_context/
// pw_main_loop each have their own void …_destroy(). The RAII aliases below
// say that once, here, rather than at every call site in the three files
// that use them.

namespace ac3::pipewire {

struct ThreadLoopDeleter {
    void operator()(pw_thread_loop* loop) const { pw_thread_loop_destroy(loop); }
};
using ThreadLoop = std::unique_ptr<pw_thread_loop, ThreadLoopDeleter>;

struct MainLoopDeleter {
    void operator()(pw_main_loop* loop) const { pw_main_loop_destroy(loop); }
};
using MainLoop = std::unique_ptr<pw_main_loop, MainLoopDeleter>;

struct ContextDeleter {
    void operator()(pw_context* context) const { pw_context_destroy(context); }
};
using Context = std::unique_ptr<pw_context, ContextDeleter>;

struct CoreDeleter {
    void operator()(pw_core* core) const { pw_core_disconnect(core); }
};
using Core = std::unique_ptr<pw_core, CoreDeleter>;

struct RegistryDeleter {
    void operator()(pw_registry* registry) const {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    }
};
using Registry = std::unique_ptr<pw_registry, RegistryDeleter>;

struct StreamDeleter {
    void operator()(pw_stream* stream) const { pw_stream_destroy(stream); }
};
using Stream = std::unique_ptr<pw_stream, StreamDeleter>;

// pw_init()/pw_deinit() are process-wide, not per-object: repeated init/
// deinit cycles within one process are not a documented, relied-upon PipeWire
// usage pattern the way opening and closing an ALSA PCM repeatedly is, so
// this calls pw_init() exactly once, ever, and never pairs it with
// pw_deinit() - the same "leak it for the life of the process" choice every
// real PipeWire client (pw-cat, pw-mon, and the session manager itself)
// makes, since the alternative is repeated init/deinit cycles nothing
// upstream actually exercises.
inline void ensure_initialized() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

// The link rate a carrier runs at to carry `content_rate` of `format` - the
// PipeWire-side twin of ac3::alsa::carrier_rate(). Not shared with it: three
// lines duplicated beside their own backend read more clearly than a cross-
// backend helper would, and the two backends have no other reason to share
// code at all.
[[nodiscard]] constexpr std::uint32_t carrier_rate(sinks::BitstreamFormat format,
                                                    std::uint32_t content_rate) {
    return format == sinks::BitstreamFormat::kEac3 ? content_rate * 4 : content_rate;
}

[[nodiscard]] constexpr spa_audio_iec958_codec iec958_codec_for(sinks::BitstreamFormat format) {
    return format == sinks::BitstreamFormat::kEac3 ? SPA_AUDIO_IEC958_CODEC_EAC3
                                                    : SPA_AUDIO_IEC958_CODEC_AC3;
}

// The carrier is a 2-channel 16-bit stream whatever rides inside it (see
// ac3::alsa's identical constant and the comment on it), so a burst's length
// in bytes and in sample-frames differ by a constant 4 here too.
inline constexpr std::size_t kCarrierFrameBytes = 4;

// Every Audio/Source or Audio/Sink node currently in the graph, visited once
// each with its global id and property dictionary.
//
// This is a full connect - list - disconnect round trip against a fresh,
// scoped pw_main_loop: attach to the session, register a registry listener,
// ask the server to confirm it has sent every global that existed at attach
// time (pw_core_sync's `done` event, matched by sequence number so a global
// added mid-walk cannot be mistaken for the boundary), and return once that
// confirmation arrives. The same shape PipeWire's own listing tools use
// (`pw-cli ls`, the `list-inputs.c` example) as a synchronous call, which is
// what ac3::capture::enumerate_devices() and ac3::sinks::enumerate_render_
// devices() both need to stay synchronous themselves.
//
// A machine with no PipeWire session running - a container, a CI runner, a
// desktop where the daemon simply is not started - fails at
// pw_context_connect() and `visit` is never called, exactly like ALSA's own
// for_each_pcm() silently visiting nothing on a machine with no sound card.
// Returns false only for that "no session to ask" case; the caller decides
// what an empty result means from context, the same way an empty ALSA
// enumeration is a success rather than an error.
template <typename Visitor>
bool for_each_audio_node(Visitor&& visit) {
    ensure_initialized();

    MainLoop loop{pw_main_loop_new(nullptr)};
    if (!loop) {
        return false;
    }
    Context context{pw_context_new(pw_main_loop_get_loop(loop.get()), nullptr, 0)};
    if (!context) {
        return false;
    }
    Core core{pw_context_connect(context.get(), nullptr, 0)};
    if (!core) {
        return false;
    }
    Registry registry{pw_core_get_registry(core.get(), PW_VERSION_REGISTRY, 0)};
    if (!registry) {
        return false;
    }

    struct State {
        pw_main_loop* loop;
        Visitor* visit;
        int pending_seq = -1;
        bool done = false;
    } state{loop.get(), &visit, -1, false};

    spa_hook registry_listener{};
    pw_registry_events registry_events{};
    registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = [](void* data, uint32_t id, uint32_t /*permissions*/,
                                 const char* type, uint32_t /*version*/,
                                 const spa_dict* props) {
        auto& s = *static_cast<State*>(data);
        if (props != nullptr && std::string_view{type} == PW_TYPE_INTERFACE_Node) {
            (*s.visit)(id, *props);
        }
    };
    pw_registry_add_listener(registry.get(), &registry_listener, &registry_events, &state);

    spa_hook core_listener{};
    pw_core_events core_events{};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = [](void* data, uint32_t id, int seq) {
        auto& s = *static_cast<State*>(data);
        if (id == PW_ID_CORE && seq == s.pending_seq) {
            s.done = true;
            pw_main_loop_quit(s.loop);
        }
    };
    pw_core_add_listener(core.get(), &core_listener, &core_events, &state);

    state.pending_seq = pw_core_sync(core.get(), PW_ID_CORE, 0);
    pw_main_loop_run(loop.get());

    return state.done;
}

// "Audio/Source" (a real input) or "Audio/Sink" (a render endpoint, whose
// monitor a capture stream can loop back - see capture.cpp) - the two
// media.class values this backend does anything with. Everything else in
// the graph (video nodes, other clients' own streams, virtual devices with
// no media.class at all) is silently not visited by for_each_audio_node's
// caller-side filtering below.
[[nodiscard]] inline bool is_audio_source(const spa_dict& props) {
    const char* class_name = spa_dict_lookup(&props, PW_KEY_MEDIA_CLASS);
    return class_name != nullptr && std::string_view{class_name} == "Audio/Source";
}

[[nodiscard]] inline bool is_audio_sink(const spa_dict& props) {
    const char* class_name = spa_dict_lookup(&props, PW_KEY_MEDIA_CLASS);
    return class_name != nullptr && std::string_view{class_name} == "Audio/Sink";
}

// What PW_KEY_TARGET_OBJECT accepts for this node - see pw_stream_connect()'s
// own documentation ("the PW_KEY_NODE_NAME value of the target node"). Used
// both as the DeviceInfo/RenderDeviceInfo id this library hands back to a
// caller and as the value that caller's id is later handed back to
// PW_KEY_TARGET_OBJECT, so the two must stay the same key.
[[nodiscard]] inline std::string node_id(const spa_dict& props) {
    const char* name = spa_dict_lookup(&props, PW_KEY_NODE_NAME);
    return name != nullptr ? name : "";
}

[[nodiscard]] inline std::string node_friendly_name(const spa_dict& props) {
    if (const char* description = spa_dict_lookup(&props, PW_KEY_NODE_DESCRIPTION);
        description != nullptr && description[0] != '\0') {
        return description;
    }
    return node_id(props);
}

}  // namespace ac3::pipewire
