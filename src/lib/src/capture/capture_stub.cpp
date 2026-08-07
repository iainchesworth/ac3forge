#include "ac3/capture/capture.hpp"

// The no-backend build. CMake selects this translation unit on platforms
// without a capture implementation; every entry point fails cleanly with
// kNoBackend rather than the API disappearing from the library.

namespace ac3::capture {

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure: return "a platform audio call failed";
        case CaptureError::kDeviceNotFound: return "the requested capture device was not found";
        case CaptureError::kFormatUnsupported: return "the device sample format is unsupported";
        case CaptureError::kAlreadyRunning: return "capture is already running";
    }
    return "unknown capture error";
}

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    return std::unexpected(CaptureError::kNoBackend);
}

struct Capture::Impl {};

Capture::Capture() : impl_(nullptr) {}
Capture::~Capture() = default;

std::expected<void, CaptureError> Capture::start(const std::string&, DeviceKind, std::size_t) {
    return std::unexpected(CaptureError::kNoBackend);
}

void Capture::stop() {}
bool Capture::running() const { return false; }
std::uint32_t Capture::sample_rate() const { return 0; }
std::uint16_t Capture::channels() const { return 0; }
CaptureStats Capture::stats() const { return {}; }
RingBuffer* Capture::buffer() { return nullptr; }

}  // namespace ac3::capture
