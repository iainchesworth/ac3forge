#include "ac3/mlp/rice.hpp"

#include <cassert>

namespace ac3::mlp::rice {

void encode(BitWriter& w, std::uint32_t value, int k) {
    assert(k >= 0 && k <= 30);
    const std::uint32_t quotient = value >> k;
    for (std::uint32_t i = 0; i < quotient; ++i) {
        w.put_bit(true);
    }
    w.put_bit(false);
    if (k > 0) {
        w.put(value & ((std::uint32_t{1} << k) - 1), k);
    }
}

std::uint32_t decode(BitReader& r, int k) {
    assert(k >= 0 && k <= 30);
    std::uint32_t quotient = 0;
    while (r.read_bit() != 0) {
        ++quotient;
    }
    const std::uint32_t remainder = k > 0 ? r.read(k) : 0;
    return (quotient << k) | remainder;
}

}  // namespace ac3::mlp::rice
