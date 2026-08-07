#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

#include "ac3/capture/ring_buffer.hpp"

TEST_CASE("ring buffer rounds capacity up to a power of two", "[ring]") {
    CHECK(ac3::capture::RingBuffer(1000).capacity() == 1024);
    CHECK(ac3::capture::RingBuffer(1024).capacity() == 1024);
    CHECK(ac3::capture::RingBuffer(1).capacity() == 2);
}

TEST_CASE("write then read returns the same samples in order", "[ring]") {
    ac3::capture::RingBuffer ring(64);
    std::vector<float> in(40);
    std::iota(in.begin(), in.end(), 1.0f);
    CHECK(ring.write(in) == in.size());
    CHECK(ring.available() == in.size());

    std::vector<float> out(40);
    CHECK(ring.read(out) == out.size());
    CHECK(out == in);
    CHECK(ring.available() == 0);
    CHECK(ring.dropped() == 0);
}

TEST_CASE("writes wrap around the buffer end", "[ring]") {
    ac3::capture::RingBuffer ring(16);  // capacity 16, usable 15
    std::vector<float> chunk(10);
    std::vector<float> out(10);
    // Three passes push the write index past the wrap point twice.
    for (int pass = 0; pass < 3; ++pass) {
        std::iota(chunk.begin(), chunk.end(), static_cast<float>(pass * 100));
        REQUIRE(ring.write(chunk) == chunk.size());
        REQUIRE(ring.read(out) == out.size());
        CHECK(out == chunk);
    }
}

TEST_CASE("a full buffer drops the overflow and counts it", "[ring]") {
    ac3::capture::RingBuffer ring(8);  // capacity 8, one slot reserved
    const std::vector<float> in(20, 0.5f);
    const auto written = ring.write(in);
    CHECK(written == 7);
    CHECK(ring.dropped() == 13);
    CHECK(ring.available() == 7);

    // Draining makes room again.
    std::vector<float> out(7);
    CHECK(ring.read(out) == 7);
    CHECK(ring.write(std::vector<float>(4, 1.0f)) == 4);
}

TEST_CASE("reads never exceed what was written", "[ring]") {
    ac3::capture::RingBuffer ring(32);
    std::vector<float> out(10);
    CHECK(ring.read(out) == 0);  // empty
    const std::vector<float> in(3, 2.0f);
    ring.write(in);
    CHECK(ring.read(out) == 3);
}

TEST_CASE("concurrent producer and consumer preserve the sample sequence", "[ring]") {
    // The real usage: the WASAPI thread writes while the encoder reads. Every
    // sample that survives must appear exactly once, in order.
    constexpr std::size_t kTotal = 200'000;
    ac3::capture::RingBuffer ring(1024);
    std::atomic<std::size_t> produced{0};

    std::jthread producer([&] {
        std::vector<float> chunk(64);
        std::size_t next = 0;
        while (next < kTotal) {
            const std::size_t count = std::min<std::size_t>(chunk.size(), kTotal - next);
            for (std::size_t i = 0; i < count; ++i) {
                chunk[i] = static_cast<float>(next + i);
            }
            std::size_t offset = 0;
            while (offset < count) {
                const auto wrote = ring.write(std::span{chunk}.subspan(offset, count - offset));
                offset += wrote;
                if (wrote == 0) {
                    std::this_thread::yield();
                }
            }
            next += count;
            produced.store(next, std::memory_order_release);
        }
    });

    std::vector<float> out(128);
    std::size_t expected = 0;
    bool ordered = true;
    while (expected < kTotal) {
        const auto got = ring.read(out);
        for (std::size_t i = 0; i < got; ++i) {
            if (out[i] != static_cast<float>(expected + i)) {
                ordered = false;
            }
        }
        expected += got;
        if (got == 0) {
            std::this_thread::yield();
        }
    }
    producer.join();

    // What matters across the thread boundary: every sample arrived exactly
    // once, in order. dropped() is deliberately NOT asserted to be zero here
    // - this producer retries whatever a full buffer refuses, so the counter
    // records back-pressure rather than lost audio (see its comment).
    CHECK(ordered);
    CHECK(expected == kTotal);
}
