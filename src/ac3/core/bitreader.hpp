#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ac3 {

// MSB-first bit reader, the mirror of BitWriter (A/52 §5.1). Reading past
// the end sets a sticky overflow flag and yields zeros — callers check
// overflowed() once at a suitable boundary instead of guarding every read.
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint32_t read(int bits) {
        assert(bits >= 0 && bits <= 32);
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            value = (value << 1) | read_bit();
        }
        return value;
    }

    [[nodiscard]] std::uint32_t read_bit() {
        const std::size_t byte_index = position_ >> 3;
        if (byte_index >= data_.size()) {
            overflowed_ = true;
            return 0;
        }
        const auto bit =
            (std::to_integer<std::uint32_t>(data_[byte_index]) >> (7 - (position_ & 7))) & 1;
        ++position_;
        return bit;
    }

    void skip(std::size_t bits) {
        position_ += bits;
        if (position_ > data_.size() * 8) {
            overflowed_ = true;
        }
    }

    [[nodiscard]] std::size_t bit_position() const { return position_; }
    [[nodiscard]] bool overflowed() const { return overflowed_; }

private:
    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    bool overflowed_ = false;
};

}  // namespace ac3
