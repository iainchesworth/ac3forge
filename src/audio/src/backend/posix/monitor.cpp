#include "ac3/audio/monitor.hpp"

// The Unix monitor backend: there isn't one. CMake compiles this directory's
// monitor.cpp on Linux and macOS, and every entry point fails with
// kNoBackend rather than the API disappearing - callers keep compiling, and
// get told no instead of getting nothing. See platform/posix/passthrough.cpp
// for the same convention.

namespace ac3::audio {

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "a platform audio call failed";
        case MonitorError::kDeviceNotFound: return "the requested render device was not found";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

struct MonitorSink::Impl {};

MonitorSink::MonitorSink() : impl_(nullptr) {}
MonitorSink::~MonitorSink() = default;

std::expected<void, MonitorError> MonitorSink::start(const std::string&, std::uint32_t,
                                                      std::uint16_t, std::uint32_t) {
    return std::unexpected(MonitorError::kNoBackend);
}

bool MonitorSink::submit(std::span<const float>) { return false; }
bool MonitorSink::can_submit() const { return false; }
void MonitorSink::stop() {}
bool MonitorSink::running() const { return false; }
MonitorStats MonitorSink::stats() const { return {}; }

}  // namespace ac3::audio
