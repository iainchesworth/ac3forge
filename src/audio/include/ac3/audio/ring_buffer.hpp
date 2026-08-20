#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <span>
#include <vector>

// Lock-free single-producer / single-consumer ring buffer: one thread writes,
// one reads, with no locks, no allocation and no system calls once
// constructed, so an audio thread never blocks.
//
// Used in both directions: the WASAPI capture thread writes float samples for
// the encoder to drain, and the encoder writes IEC 61937 burst bytes for the
// exclusive-mode render thread to drain.
//
// Exactly one thread may write and one may read. Capacity is rounded up to a
// power of two so the index wrap is a mask rather than a modulo.

namespace ac3::audio {

template <typename T>
class BasicRingBuffer {
public:
    explicit BasicRingBuffer(std::size_t capacity)
        : buffer_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
          mask_(buffer_.size() - 1) {}

    [[nodiscard]] std::size_t capacity() const { return buffer_.size(); }

    // Producer side. Returns the number of items actually written; a short
    // return means the consumer is behind and the remainder was refused.
    std::size_t write(std::span<const T> items) {
        const auto write_at = write_.load(std::memory_order_relaxed);
        const auto read_at = read_.load(std::memory_order_acquire);
        const std::size_t free_space = buffer_.size() - (write_at - read_at) - 1;
        const std::size_t count = std::min(items.size(), free_space);
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[(write_at + i) & mask_] = items[i];
        }
        write_.store(write_at + count, std::memory_order_release);
        if (count < items.size()) {
            dropped_.fetch_add(items.size() - count, std::memory_order_relaxed);
        }
        return count;
    }

    // Consumer side. Returns the number of items actually read.
    std::size_t read(std::span<T> out) {
        const auto read_at = read_.load(std::memory_order_relaxed);
        const auto write_at = write_.load(std::memory_order_acquire);
        const std::size_t count = std::min(out.size(), write_at - read_at);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = buffer_[(read_at + i) & mask_];
        }
        read_.store(read_at + count, std::memory_order_release);
        return count;
    }

    [[nodiscard]] std::size_t available() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

    // Items refused because the buffer was full when write() was called.
    // The capture thread cannot retry - it has to return to the device loop -
    // so for the real producer a refusal is a permanent loss, which is what
    // makes this a useful overrun signal. A producer that DOES retry (tests,
    // offline feeds) will see this climb without losing anything.
    [[nodiscard]] std::size_t dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

    void reset() {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<T> buffer_;
    std::size_t mask_;
    // Monotonic counters; the mask turns them into indices, so a full buffer
    // is distinguishable from an empty one without a spare flag.
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> dropped_{0};
};

using RingBuffer = BasicRingBuffer<float>;
using ByteRingBuffer = BasicRingBuffer<std::byte>;

}  // namespace ac3::audio
