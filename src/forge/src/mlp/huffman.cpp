#include "ac3/mlp/huffman.hpp"

#include <cassert>

namespace ac3::mlp::huffman {

void encode_table2(BitWriter& w, int value) {
    assert(value >= kTable2Min && value <= kTable2Max);
    if (value <= 0) {
        // A run of (1 - value) zeros, then a terminating 1 - except -7,
        // which is all eight zeros with no terminator (WO Table 2).
        if (value == kTable2Min) {
            w.put(0, 8);
        } else {
            const int zeros = 1 - value;
            w.put(1, zeros + 1);  // MSB-first: `zeros` zeros then a 1
        }
    } else {
        // A run of `value` ones, then a terminating 0 - except +8, which is
        // all eight ones with no terminator.
        if (value == kTable2Max) {
            w.put(0xFF, 8);
        } else {
            w.put((1u << (value + 1)) - 2, value + 1);  // `value` ones then a 0
        }
    }
}

int decode_table2(BitReader& r) {
    if (r.read_bit() == 0) {
        int zeros = 1;
        while (zeros < 8) {
            if (r.read_bit() != 0) {
                return 1 - zeros;
            }
            ++zeros;
        }
        return kTable2Min;
    }
    int ones = 1;
    while (ones < 8) {
        if (r.read_bit() == 0) {
            return ones;
        }
        ++ones;
    }
    return kTable2Max;
}

int select_n(std::int32_t lo, std::int32_t hi) {
    assert(lo <= hi);
    for (int n = kMinN; n <= kMaxN; ++n) {
        if (lo >= -(std::int32_t{1} << n) + 1 && hi <= (std::int32_t{1} << n)) {
            return n;
        }
    }
    assert(false && "significant word exceeds Table 3's widest range - use encode_pcm");
    return kMaxN;
}

void encode_significant(BitWriter& w, std::int32_t x, int n) {
    assert(n >= kMinN && n <= kMaxN);
    assert(x >= -(std::int32_t{1} << n) + 1 && x <= (std::int32_t{1} << n));
    // Offset-binary mapping: u spans exactly [0, 2^(n+1) - 1] over Table 3's
    // asymmetric range, so the top four digits land exactly on Table 2's
    // -7..+8 (see the header comment on transcription vs convention).
    const auto u = static_cast<std::uint32_t>(x + (std::int32_t{1} << n) - 1);
    const int m = n - 3;  // raw digits following the codeword
    encode_table2(w, static_cast<int>(u >> m) - 7);
    if (m > 0) {
        w.put(u & ((1u << m) - 1), m);
    }
}

std::int32_t decode_significant(BitReader& r, int n) {
    assert(n >= kMinN && n <= kMaxN);
    const int m = n - 3;
    const auto top = static_cast<std::uint32_t>(decode_table2(r) + 7);
    const std::uint32_t low = m > 0 ? r.read(m) : 0;
    const std::uint32_t u = (top << m) | low;
    return static_cast<std::int32_t>(u) - (std::int32_t{1} << n) + 1;
}

void encode_small(BitWriter& w, std::span<const SmallCode> table, int value) {
    for (const auto& entry : table) {
        if (entry.value == value) {
            w.put(entry.code, entry.length);
            return;
        }
    }
    assert(false && "value not representable in this small-signal table");
}

int decode_small(BitReader& r, std::span<const SmallCode> table) {
    std::uint32_t code = 0;
    for (int length = 1; length <= 8; ++length) {
        code = (code << 1) | r.read_bit();
        for (const auto& entry : table) {
            if (entry.length == length && entry.code == code) {
                return entry.value;
            }
        }
    }
    assert(false && "bit sequence matches no codeword in this table");
    return 0;
}

void encode_pcm(BitWriter& w, std::int32_t x, int n) {
    assert(n >= 1 && n <= 30);
    assert(x >= -(std::int32_t{1} << n) + 1 && x <= (std::int32_t{1} << n));
    const auto u = static_cast<std::uint32_t>(x + (std::int32_t{1} << n) - 1);
    w.put(u, n + 1);
}

std::int32_t decode_pcm(BitReader& r, int n) {
    assert(n >= 1 && n <= 30);
    const std::uint32_t u = r.read(n + 1);
    return static_cast<std::int32_t>(u) - (std::int32_t{1} << n) + 1;
}

}  // namespace ac3::mlp::huffman
