#pragma once

#include <chrono>
#include <cstddef>

// Detects a capture device that has stopped delivering audio. A read loop
// polling a vanished device (unplugged, disabled, the endpoint torn down
// under it) sees nothing but zero-byte reads forever - `while (got == 0)
// sleep 2ms` alone never learns the difference between "briefly starved" and
// "gone", so a live session just sits there reading "Running" with nothing
// coming in. This tracks time since the last non-empty read and answers
// whether that gap has crossed a threshold; the read loop decides what
// "stop the session" means, this only decides when.
//
// Header-only and Qt-free (matches BasicRingBuffer's own shape in
// ring_buffer.hpp) so it is usable from a plain Catch2 test without pulling
// in anything platform-specific, and the caller drives the clock explicitly
// rather than this reading it itself - a test can then advance time in exact
// steps instead of sleeping for real.

namespace ac3::capture {

class SilenceWatchdog {
public:
    explicit SilenceWatchdog(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000})
        : timeout_(timeout) {}

    // Call once, when the read loop starts - without a real starting point
    // `last_success_` defaults to the clock's epoch, which would read as
    // already-expired the instant the first zero-byte read is seen instead
    // of only after a genuine `timeout` of silence.
    void reset(std::chrono::steady_clock::time_point now) { last_success_ = now; }

    // Call after every read attempt, empty or not. `got` is however many
    // samples (or frames, or bytes - whatever unit the caller's read()
    // returns) that attempt produced; only zero-vs-not matters here.
    void on_read(std::size_t got, std::chrono::steady_clock::time_point now) {
        if (got > 0) {
            last_success_ = now;
        }
    }

    // True once `timeout` has passed since the last non-empty read (or since
    // reset(), if there has never been one). The read loop's job is to check
    // this after every empty read and stop the session the first time it
    // comes back true - it does not fire again on its own, so a caller that
    // keeps looping past a positive result would see it stay true, not pulse
    // once; that is the read loop's call to make; this is only a clock.
    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now) const {
        return now - last_success_ >= timeout_;
    }

    [[nodiscard]] std::chrono::milliseconds timeout() const { return timeout_; }

private:
    std::chrono::milliseconds timeout_;
    std::chrono::steady_clock::time_point last_success_{};
};

}  // namespace ac3::capture
