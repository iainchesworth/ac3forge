#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "ac3/capture/capture.hpp"
#include "ac3/platform/audio_backend.hpp"
#include "ac3/sinks/monitor.hpp"
#include "ac3/sinks/passthrough.hpp"

// The platform tree, tested from outside it.
//
// One directory of src/audio/src/platform/ is compiled into the library and the
// others are not, so this file cannot name a backend - and does not want to.
// What it checks is the contract every backend has to keep, which is that
// ac3::platform::audio_backend() and the implementations beside it agree:
//
//   * a capability reported unavailable must actually refuse, with kNoBackend
//     and nothing else, because the CLI prints that report instead of calling
//     and would otherwise be lying to the user;
//   * a capability reported available must not answer kNoBackend, because the
//     report said the code exists;
//   * every error code must describe itself, because those strings are
//     printed verbatim.
//
// None of it touches audio hardware. Enumeration on a real backend walks the
// machine's sound cards, and a machine with none - a container, a CI runner,
// WSL - enumerates an empty list, which is a success. Nothing here opens a
// device, starts a thread or makes a sound, so the suite stays runnable
// headless on every platform it is built for.

TEST_CASE("audio_backend reports a reason exactly when a capability is missing") {
    const auto& backend = ac3::platform::audio_backend();

    for (const auto& capability : {backend.capture, backend.passthrough, backend.monitor}) {
        if (capability.available) {
            // Nothing to excuse, so nothing to say.
            CHECK(capability.reason.empty());
        } else {
            // The CLI prints this instead of running the command; an empty
            // string would print as "UNAVAILABLE HERE — ." and tell nobody
            // anything.
            CHECK_FALSE(capability.reason.empty());
        }
    }
}

TEST_CASE("capture enumeration agrees with the reported capability") {
    const auto& capture = ac3::platform::audio_backend().capture;
    const auto devices = ac3::capture::enumerate_devices();

    if (capture.available) {
        // A machine with no capture hardware is entitled to an empty list;
        // what it is not entitled to is claiming there is no backend when the
        // report next door says there is one.
        if (!devices.has_value()) {
            CHECK(devices.error() != ac3::capture::CaptureError::kNoBackend);
        }
    } else {
        REQUIRE_FALSE(devices.has_value());
        CHECK(devices.error() == ac3::capture::CaptureError::kNoBackend);
    }
}

TEST_CASE("passthrough enumeration agrees with the reported capability") {
    const auto& passthrough = ac3::platform::audio_backend().passthrough;
    const auto devices = ac3::sinks::enumerate_render_devices();

    if (passthrough.available) {
        if (!devices.has_value()) {
            CHECK(devices.error() != ac3::sinks::PassthroughError::kNoBackend);
        }
    } else {
        REQUIRE_FALSE(devices.has_value());
        CHECK(devices.error() == ac3::sinks::PassthroughError::kNoBackend);
    }
}

TEST_CASE("a device list is well formed whatever the machine has in it") {
    const auto devices = ac3::capture::enumerate_devices();
    if (!devices) {
        SUCCEED("no capture backend in this build");
        return;
    }
    for (const auto& device : *devices) {
        // An id is what start() is handed back; an entry without one names a
        // device nobody can open.
        CHECK_FALSE(device.id.empty());
        CHECK_FALSE(device.name.empty());
    }
}

TEST_CASE("a render device list is well formed whatever the machine has in it") {
    const auto devices = ac3::sinks::enumerate_render_devices();
    if (!devices) {
        SUCCEED("no passthrough backend in this build");
        return;
    }
    for (const auto& device : *devices) {
        CHECK_FALSE(device.id.empty());
        CHECK_FALSE(device.name.empty());
    }
}

TEST_CASE("every capture error describes itself") {
    using ac3::capture::CaptureError;
    for (const auto error : {CaptureError::kNoBackend, CaptureError::kComFailure,
                             CaptureError::kDeviceNotFound, CaptureError::kFormatUnsupported,
                             CaptureError::kAlreadyRunning}) {
        const std::string_view text = ac3::capture::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown capture error");
    }
}

TEST_CASE("every passthrough error describes itself") {
    using ac3::sinks::PassthroughError;
    for (const auto error :
         {PassthroughError::kNoBackend, PassthroughError::kComFailure,
          PassthroughError::kDeviceNotFound, PassthroughError::kFormatRejected,
          PassthroughError::kExclusiveUnavailable, PassthroughError::kAlreadyRunning,
          PassthroughError::kNotRunning}) {
        const std::string_view text = ac3::sinks::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown passthrough error");
    }
}

TEST_CASE("every monitor error describes itself") {
    using ac3::sinks::MonitorError;
    for (const auto error : {MonitorError::kNoBackend, MonitorError::kComFailure,
                             MonitorError::kDeviceNotFound, MonitorError::kAlreadyRunning,
                             MonitorError::kNotRunning}) {
        const std::string_view text = ac3::sinks::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown monitor error");
    }
}

TEST_CASE("a monitor sink refuses a channel count it cannot interpret") {
    // Zero channels is nonsense on every backend and is rejected before any
    // device is touched, so this reaches no hardware even where a backend
    // exists. A real start() is not attempted anywhere in this file - that
    // would open an output and make a noise on a developer's machine.
    ac3::sinks::MonitorSink sink;
    CHECK_FALSE(sink.start("", 48000, 0).has_value());
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit({}));
}

TEST_CASE("a sink that was never started refuses work") {
    // True of every backend including the stub, and the reason submit() checks
    // running() first: a caller that ignores start()'s result must not be able
    // to write into a queue that does not exist.
    ac3::sinks::PassthroughSink sink;
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit({}));

    const auto stats = sink.stats();
    CHECK(stats.bursts_submitted == 0);
    CHECK(stats.bursts_rendered == 0);
    CHECK(stats.underruns == 0);
}

TEST_CASE("a capture that was never started reports nothing") {
    ac3::capture::Capture capture;
    CHECK_FALSE(capture.running());
    CHECK(capture.sample_rate() == 0);
    CHECK(capture.channels() == 0);
    CHECK(capture.stats().frames_captured == 0);
}
