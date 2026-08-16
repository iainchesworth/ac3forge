#pragma once

// EMDF Atmos object-signing - clean-room.
//
// Computes the keyed EMDF-protection primary tag over an ac3forge Atmos
// syncframe and writes it into protection_bits_primary (recomputing crc2), so a
// decoder that validates the emdf_protection field will accept the frame's JOC
// object container instead of falling back to the 5.1 bed. See
// docs/concepts/object-signing.md.
//
// Provenance: the HMAC-SHA-256 construction (RFC 2104 / FIPS 180-4) and the
// choice of which frame regions are authenticated are derived from the public
// container layout this codec already emits (src/lib/src/emdf/emdf.cpp,
// ETSI TS 103 420 / the E-AC-3 syntax) and reuse this project's own clean-room
// parsing primitives (BitReader, decode_exponents, compute_bit_allocation, the
// spx helpers). The ONLY externally-provisioned input is the key (SigningKey),
// which the operator supplies at runtime and this code never embeds - the same
// posture a licensed tool (DEE, via iLok) takes with its own key. A stream
// signed with a key that does not match a given decoder's simply fails that
// decoder's check, exactly as an unsigned one does; nothing here reconstructs a
// key.
//
// verify_atmos_frame/verify_atmos_stream below check a tag this same signer
// wrote - round-trip testing, tamper detection on this project's own signed
// test assets, and CI/delivery QC. That is NOT the same thing as, and grants
// no interoperability with, a real Dolby-licensed decoder's own proprietary
// auth gate: that one uses a completely different key and scheme baked into
// Dolby's binary, and this project has deliberately never attempted to forge
// or replicate it. See docs/concepts/object-signing.md.

#include <cstddef>
#include <span>

#include "ac3/signing/signing_key.hpp"

namespace ac3::signing {

// Signs, in place, every syncframe in `stream` that carries an EMDF object
// container (OAMD payload), using `key`. Frames without a container are left
// untouched. Returns the number of frames signed.
//
// Scope: the ac3forge "atmos" output - a single independent 5.1 substream,
// frame-level exponent strategy and SNR, no coupling. A frame outside that
// subset asserts in debug and is left unsigned in release rather than signed
// wrong.
[[nodiscard]] int sign_atmos_stream(std::span<std::byte> stream, const SigningKey& key);

// One syncframe. Returns true if it carried a container and was signed.
[[nodiscard]] bool sign_atmos_frame(std::span<std::byte> frame, const SigningKey& key);

// A frame with no EMDF object container is neither "verified" nor "failed" -
// there is nothing in it to check - so that case is its own outcome
// (kNoContainer) rather than being folded into kMismatch, which would
// misreport every plain/non-Atmos frame as a signature failure.
enum class VerifyResult {
    kNoContainer,  // no EMDF object container - nothing to verify
    kValid,        // container present, tag matches `key`
    kMismatch,     // container present, tag does not match `key`
};

// Frame counts by outcome, mirroring sign_atmos_stream's own aggregate
// (frames signed) rather than a per-frame vector - a caller wants "how many
// verified, how many didn't, how many had nothing to check", the same shape
// apply_object_signing's own callers already consume.
struct VerifySummary {
    int valid = 0;
    int mismatch = 0;
    int no_container = 0;
};

// Checks every syncframe in `stream` against `key`, without modifying it.
[[nodiscard]] VerifySummary verify_atmos_stream(std::span<const std::byte> stream,
                                                const SigningKey& key);

// One syncframe. Mirrors sign_atmos_frame's exact construction (excise the
// framing/metadata/skip/CRC holes into message A, zero the tag bits in the
// container to build message B, HMAC(key, A||B) truncated to the primary
// protection field's width) but reads the existing protection_bits_primary
// bits instead of writing computed ones, and compares.
[[nodiscard]] VerifyResult verify_atmos_frame(std::span<const std::byte> frame,
                                              const SigningKey& key);

}  // namespace ac3::signing
