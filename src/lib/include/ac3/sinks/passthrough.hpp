#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Exclusive-mode IEC 61937 passthrough: hand already-packed AC-3 bursts to an
// S/PDIF or HDMI endpoint so the AV receiver on the other end decodes them
// itself and lights its Dolby Digital indicator.
//
// Exclusive mode is mandatory. In shared mode the Windows audio engine would
// treat the bursts as ordinary PCM and mix, resample or volume-scale them;
// any of those corrupts the bit pattern and the receiver hears static or
// loses lock. Exclusive mode hands the endpoint our bytes untouched.
//
// The burst packing itself lives in ac3::iec61937 (byte-exact against
// FFmpeg's spdif muxer); this is only delivery.

namespace ac3::sinks {

enum class PassthroughError {
    kNoBackend,             // built without a platform passthrough backend
    kComFailure,
    kDeviceNotFound,
    kFormatRejected,        // endpoint will not accept AC-3 over IEC 61937
    kExclusiveUnavailable,  // device busy, or exclusive access disabled for it
    kAlreadyRunning,
    kNotRunning,
};

[[nodiscard]] std::string_view describe(PassthroughError error);

struct RenderDeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
    // IsFormatSupported() said yes to AC-3 over IEC 61937 in exclusive mode.
    // A GUI should grey out everything else rather than let the user pick a
    // device that can only fail.
    bool supports_ac3_passthrough = false;
    // Whether plain 16-bit stereo PCM is accepted in exclusive mode. This
    // separates the two reasons passthrough can be unavailable: a device that
    // refuses even PCM has exclusive mode switched off (or is in use), while
    // one that takes PCM but not IEC 61937 simply cannot bitstream - an
    // analog output, say, rather than S/PDIF or HDMI.
    bool supports_exclusive_pcm = false;
};

// Every active render endpoint, each probed for AC-3 passthrough support at
// the given carrier rate (the AC-3 stream's own sample rate).
[[nodiscard]] std::expected<std::vector<RenderDeviceInfo>, PassthroughError>
enumerate_render_devices(std::uint32_t sample_rate = 48000);

struct PassthroughStats {
    std::uint64_t bursts_submitted = 0;
    std::uint64_t bursts_rendered = 0;
    // Render periods that found the queue empty. Any non-zero value means the
    // receiver heard a gap, which usually drops its lock.
    std::uint64_t underruns = 0;
};

class PassthroughSink {
public:
    PassthroughSink();
    ~PassthroughSink();
    PassthroughSink(const PassthroughSink&) = delete;
    PassthroughSink& operator=(const PassthroughSink&) = delete;

    // Opens `device_id` (empty selects the default render endpoint) in
    // exclusive mode with an IEC 61937 / AC-3 format at `sample_rate`, and
    // starts the render thread.
    [[nodiscard]] std::expected<void, PassthroughError> start(const std::string& device_id,
                                                              std::uint32_t sample_rate = 48000);

    // Queues one complete 6144-byte burst (see ac3::iec61937::wrap_frame).
    // Returns false if the queue is full - the caller is running ahead of
    // real time and should wait rather than spin.
    bool submit(std::span<const std::byte> burst);

    // Room for at least one more burst without blocking.
    [[nodiscard]] bool can_submit() const;

    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] PassthroughStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::sinks
