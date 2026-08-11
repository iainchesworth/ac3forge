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

    // Append the first `bits` bits of `data` (MSB first, matching put()'s own
    // packing), for splicing in bit-exact content assembled elsewhere - e.g.
    // a sub-structure built in its own BitWriter, whose own trailing CRC was
    // computed over its exact (possibly non-byte-aligned) bit count via
    // bit_count(), and now needs joining into an ongoing bitstream without
    // introducing the padding take()/byte_align() would add. `data` must
    // hold at least ceil(bits/8) bytes; bits in its final byte beyond `bits`
    // are ignored, not required to be zero.
    void put_bits(std::span<const std::byte> data, std::size_t bits) {
        std::size_t remaining = bits;
        std::size_t byte_index = 0;
        while (remaining >= 8) {
            put(std::to_integer<std::uint32_t>(data[byte_index]), 8);
            ++byte_index;
            remaining -= 8;
        }
        if (remaining != 0) {
            const auto last = std::to_integer<std::uint32_t>(data[byte_index]);
            put(last >> (8 - remaining), static_cast<int>(remaining));
        }
    }

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
