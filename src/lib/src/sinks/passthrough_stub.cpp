#include "ac3/sinks/passthrough.hpp"

// The no-backend build. CMake selects this translation unit on platforms
// without an exclusive-mode passthrough implementation; every entry point
// fails cleanly with kNoBackend rather than the API disappearing.

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
