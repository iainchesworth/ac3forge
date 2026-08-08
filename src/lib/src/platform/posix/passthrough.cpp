#include "ac3/sinks/passthrough.hpp"

// The Unix passthrough backend: there isn't one. CMake compiles this
// directory's passthrough.cpp on Linux and macOS, and every entry point fails
// with kNoBackend rather than the API disappearing - callers keep compiling,
// and get told no instead of getting nothing.
//
// Ask ac3::platform::audio_backend() BEFORE calling any of this if the answer
// wants to be a sentence rather than an error code; see
// platform/posix/audio_backend.cpp for why there is no backend here, and what
// to reach for instead.

namespace ac3::sinks {

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "a platform audio call failed";
        case PassthroughError::kDeviceNotFound: return "the requested render device was not found";
        case PassthroughError::kFormatRejected:
            return "the endpoint will not accept AC-3 over IEC 61937";
        case PassthroughError::kExclusiveUnavailable: return "exclusive access was refused";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t) {
    return std::unexpected(PassthroughError::kNoBackend);
}

struct PassthroughSink::Impl {};

PassthroughSink::PassthroughSink() : impl_(nullptr) {}
PassthroughSink::~PassthroughSink() = default;

std::expected<void, PassthroughError> PassthroughSink::start(const std::string&, std::uint32_t) {
    return std::unexpected(PassthroughError::kNoBackend);
}

bool PassthroughSink::submit(std::span<const std::byte>) { return false; }
bool PassthroughSink::can_submit() const { return false; }
void PassthroughSink::stop() {}
bool PassthroughSink::running() const { return false; }
PassthroughStats PassthroughSink::stats() const { return {}; }

}  // namespace ac3::sinks
