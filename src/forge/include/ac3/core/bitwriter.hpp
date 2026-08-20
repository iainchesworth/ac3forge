#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ac3 {

// MSB-first bit packer for AC-3 syntax elements. A/52 §5.1: fields are packed
// into the bit stream most-significant-bit first, in syntax order.
class BitWriter {
public:
    // Append the low `bits` bits of `value`, MSB first. 0 <= bits <= 32.
    void put(std::uint32_t value, int bits) {
        assert(bits >= 0 && bits <= 32);
        assert(bits == 32 || value < (std::uint64_t{1} << bits));
        acc_ = (acc_ << bits) | value;
        pending_ += bits;
        while (pending_ >= 8) {
            pending_ -= 8;
            bytes_.push_back(static_cast<std::byte>((acc_ >> pending_) & 0xFF));
        }
    }

    void put_bit(bool bit) { put(bit ? 1u : 0u, 1); }

    // Pre-size the output buffer. Every CBR pack site knows its frame's
    // exact byte count before emitting a single field, so put()'s
    // one-byte-at-a-time growth (~11 geometric reallocations for a full
    // syncframe) is pure waste there.
    void reserve(std::size_t bytes) { bytes_.reserve(bytes); }

    // Zero-pad to the next byte boundary. Syncframes are an integral number of
    // 16-bit words, so a fully packed frame always ends byte-aligned; this is
    // for tests and partial assemblies.
    void byte_align() {
        if (pending_ != 0) {
            put(0, 8 - pending_);
        }
    }

    [[nodiscard]] std::size_t bit_count() const {
        return bytes_.size() * 8 + static_cast<std::size_t>(pending_);
    }

    // Overwrite a 16-bit big-endian field at a byte offset in already-emitted
    // output. Used to patch the two CRC words after the frame body is packed
    // (crc1 sits at byte offset 2, immediately after the sync word).
    void patch_u16(std::size_t byte_offset, std::uint16_t value) {
        assert(byte_offset + 2 <= bytes_.size());
        bytes_[byte_offset] = static_cast<std::byte>(value >> 8);
        bytes_[byte_offset + 1] = static_cast<std::byte>(value & 0xFF);
    }

    // View of the fully emitted bytes; only valid when byte-aligned.
    [[nodiscard]] std::span<const std::byte> bytes() const {
        assert(pending_ == 0);
        return bytes_;
    }

    // Zero-pad to a byte boundary and take the buffer, leaving the writer empty.
    [[nodiscard]] std::vector<std::byte> take() {
        byte_align();
        return std::exchange(bytes_, {});
    }

private:
    std::vector<std::byte> bytes_;
    std::uint64_t acc_ = 0;  // bits above `pending_` are stale and ignored
    int pending_ = 0;
};

}  // namespace ac3
