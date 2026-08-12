#pragma once

// HMAC-SHA-256 (RFC 2104) over the SHA-256 in sha256.hpp. Internal to
// ac3::signing. The key is passed in by the caller every time - this holds no
// key of its own, which is the whole point of the restructure that moved the
// signer into the clean-room tree: the algorithm is public (RFC 2104 / FIPS
// 180-4), only the key is secret, and the key is supplied at runtime.

#include <array>
#include <cstddef>
#include <span>

namespace ac3::signing {

std::array<std::byte, 32> hmac_sha256(std::span<const std::byte> key,
                                      std::span<const std::byte> message);

}  // namespace ac3::signing
