#include "hmac_sha256.hpp"

#include <algorithm>

#include "sha256.hpp"

namespace ac3::signing {

std::array<std::byte, 32> hmac_sha256(std::span<const std::byte> key,
                                      std::span<const std::byte> message) {
    constexpr std::size_t kBlock = 64;

    // K0: the key sized to one SHA-256 block - hashed down first if longer,
    // zero-padded up if shorter (RFC 2104 §2).
    std::array<std::byte, kBlock> k0{};
    if (key.size() > kBlock) {
        const std::array<std::byte, 32> hashed = sha256(key);
        std::copy(hashed.begin(), hashed.end(), k0.begin());
    } else {
        std::copy(key.begin(), key.end(), k0.begin());
    }

    std::array<std::byte, kBlock> ipad{};
    std::array<std::byte, kBlock> opad{};
    for (std::size_t i = 0; i < kBlock; ++i) {
        ipad[i] = k0[i] ^ std::byte{0x36};
        opad[i] = k0[i] ^ std::byte{0x5c};
    }

    // inner = H(ipad || message)
    Sha256 inner_hash;
    inner_hash.update(ipad);
    inner_hash.update(message);
    std::array<std::byte, 32> inner{};
    inner_hash.finish(inner);

    // out = H(opad || inner)
    Sha256 outer_hash;
    outer_hash.update(opad);
    outer_hash.update(inner);
    std::array<std::byte, 32> out{};
    outer_hash.finish(out);
    return out;
}

}  // namespace ac3::signing
