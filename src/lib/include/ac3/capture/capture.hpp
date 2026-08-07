#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/capture/ring_buffer.hpp"

// Live audio capture. On Windows this is WASAPI in shared mode: either a
// real input endpoint (microphone, line in) or a render endpoint opened in
// loopback mode, which captures whatever the machine is playing.
//
// The capture thread only ever writes interleaved float samples into a
// RingBuffer; callers pull from that buffer at their own pace. Nothing on the
// capture side allocates, locks or blocks.

namespace ac3::capture {

enum class CaptureError {
    kNoBackend,          // built without a platform capture backend
    kComFailure,         // COM/WASAPI call failed
    kDeviceNotFound,
    kFormatUnsupported,  // endpoint delivers a format we cannot convert
    kAlreadyRunning,
};

[[nodiscard]] std::string_view describe(CaptureError error);

enum class DeviceKind {
    kInput,     // microphone, line in
    kLoopback,  // what the machine is playing (a render endpoint)
};

struct DeviceInfo {
    std::string id;    // endpoint id; stable across sessions
    std::string name;  // friendly name for a UI
    DeviceKind kind = DeviceKind::kInput;
    std::uint32_t sample_rate = 0;  // the endpoint's mixer rate
    std::uint16_t channels = 0;
    bool is_default = false;
};

// Every active input endpoint, plus every active render endpoint offered as
// a loopback source.
[[nodiscard]] std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices();

struct CaptureStats {
    std::uint64_t frames_captured = 0;
    // Frames of silence synthesised to cover a loopback gap. A render
    // endpoint in loopback mode delivers nothing at all while the machine is
    // silent, so a continuous timeline has to be filled in.
    std::uint64_t frames_silence_filled = 0;
    std::uint64_t frames_dropped = 0;  // ring buffer overruns (consumer too slow)
};

class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Opens `device_id` (empty selects the default endpoint of `kind`) and
    // starts the capture thread. Samples land in buffer(), interleaved, at
    // sample_rate() x channels().
    [[nodiscard]] std::expected<void, CaptureError> start(const std::string& device_id,
                                                          DeviceKind kind,
                                                          std::size_t ring_capacity_samples = 1u
                                                                                              << 18);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint32_t sample_rate() const;
    [[nodiscard]] std::uint16_t channels() const;
    [[nodiscard]] CaptureStats stats() const;

    // Valid while running; the consumer reads from here.
    [[nodiscard]] RingBuffer* buffer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::capture
