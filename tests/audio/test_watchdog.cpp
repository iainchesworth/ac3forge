#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "ac3/audio/watchdog.hpp"

using namespace std::chrono_literals;
using ac3::audio::SilenceWatchdog;

TEST_CASE("a watchdog that keeps seeing data never times out", "[watchdog]") {
    SilenceWatchdog watchdog(3000ms);
    auto now = std::chrono::steady_clock::now();
    watchdog.reset(now);

    for (int i = 0; i < 50; ++i) {
        now += 100ms;
        watchdog.on_read(480, now);
        CHECK_FALSE(watchdog.timed_out(now));
    }
}

TEST_CASE("silence past the timeout trips the watchdog", "[watchdog]") {
    SilenceWatchdog watchdog(3000ms);
    auto now = std::chrono::steady_clock::now();
    watchdog.reset(now);

    // Empty reads (got == 0) never touch last_success_ - only on_read's own
    // "got > 0" branch does, so feeding zeros here is exactly what the real
    // read loop's dead-device case looks like.
    now += 2999ms;
    watchdog.on_read(0, now);
    CHECK_FALSE(watchdog.timed_out(now));

    now += 2ms;  // 3001ms of silence total
    watchdog.on_read(0, now);
    CHECK(watchdog.timed_out(now));
}

TEST_CASE("a late arrival resets the silence clock", "[watchdog]") {
    SilenceWatchdog watchdog(3000ms);
    auto now = std::chrono::steady_clock::now();
    watchdog.reset(now);

    now += 2900ms;  // right up against the edge, but still fed
    watchdog.on_read(256, now);
    CHECK_FALSE(watchdog.timed_out(now));

    // Without the reset above, this next gap alone would already exceed the
    // timeout - proving on_read(got > 0, ...) actually moved the clock
    // forward rather than the watchdog just measuring from its first reset()
    // for the whole test.
    now += 2900ms;
    watchdog.on_read(0, now);
    CHECK_FALSE(watchdog.timed_out(now));

    now += 200ms;  // now 3100ms since the last real arrival
    watchdog.on_read(0, now);
    CHECK(watchdog.timed_out(now));
}

TEST_CASE("timed_out is a pure read - it does not consume the trip", "[watchdog]") {
    SilenceWatchdog watchdog(1000ms);
    auto now = std::chrono::steady_clock::now();
    watchdog.reset(now);

    now += 1500ms;
    watchdog.on_read(0, now);
    CHECK(watchdog.timed_out(now));
    // Calling it again without any new state change reports the same thing -
    // the read loop, not the watchdog, decides to act on (or stop checking)
    // a trip.
    CHECK(watchdog.timed_out(now));
}

TEST_CASE("an untouched watchdog exposes the timeout it was built with", "[watchdog]") {
    CHECK(SilenceWatchdog(3000ms).timeout() == 3000ms);
    CHECK(SilenceWatchdog().timeout() == 3000ms);  // the default
}
