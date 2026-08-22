#pragma once

// SHA-256 (FIPS 180-4), self-contained. Internal to ac3::signing - not on the
// target's public include path, since callers want hmac_sha256 (and the EMDF
// signer above that), never the raw hash. No third-party dependency by design:
// the codec and everything linked beside it stay dependency-free (see the top
// of vcpkg.json), so this is a from-the-standard implementation rather than a
// pull of OpenSSL/mbedTLS for one primitive.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/signing/export.hpp"

namespace ac3::signing {

// Incremental so HMAC can feed it ipad/opad and the message in separate
// updates without first concatenating them into one buffer.
class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(std::span<const std::byte> data);
    // Finalizes into `out` and leaves the object reset(), ready to reuse.
    void finish(std::span<std::byte, 32> out);

private:
    void process_block(const std::uint8_t* block);

    std::array<std::uint32_t, 8> h_{};
    std::uint64_t total_bytes_ = 0;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
};

// Convenience one-shot over a whole buffer. Exported (unlike Sha256 itself) purely so
// tests/signing/test_signing.cpp - which reaches this private header directly to run the
// FIPS/RFC known-answer vectors, see tests/CMakeLists.txt's own comment - can resolve it when
// ac3::signing builds as a shared library (signing_objects' default-hidden visibility would
// otherwise drop it from the .so's export table). The header itself stays uninstalled and off
// the target's public include path, so this does not change what ac3::signing's own advertised
// public API is.
AC3SIGNING_EXPORT std::array<std::byte, 32> sha256(std::span<const std::byte> data);

}  // namespace ac3::signing
