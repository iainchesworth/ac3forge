#pragma once

// The signing key - supplied by the operator at runtime, never compiled in and
// never written to disk by this code. This is the one piece the clean-room
// signer deliberately does NOT carry: the HMAC construction and the
// authenticated-region layout are in-tree (see emdf_atmos_signer.hpp), but the
// key that makes a licensed decoder accept the tag is the operator's own to
// provision - exactly how DEE and other licensed tools receive theirs (iLok),
// not baked into the binary.

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ac3::signing {

// Owns the key bytes and zeroizes them on destruction, so a supplied key does
// not linger in freed heap after signing finishes. Copyable/movable; every
// copy scrubs its own bytes when it dies.
class SigningKey {
public:
    SigningKey() = default;
    explicit SigningKey(std::vector<std::byte> bytes);
    ~SigningKey();

    SigningKey(const SigningKey&) = default;
    SigningKey& operator=(const SigningKey&) = default;
    SigningKey(SigningKey&&) noexcept = default;
    SigningKey& operator=(SigningKey&&) noexcept = default;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

private:
    std::vector<std::byte> bytes_;
};

// Why a key load failed. kAbsent is not really an error - it means "no key was
// offered by any source" - so the caller can tell "operator asked to sign but
// gave no key" (hard error) apart from "no signing requested" (fine, leave the
// stream unsigned). Anything else is an outright misconfiguration.
enum class KeyErrorKind {
    kAbsent,      // no signing-key= path, no env var: nothing to load
    kUnreadable,  // a path was given but could not be opened/read
    kMalformed,   // reserved: contents could not be interpreted as a key
    kEmpty,       // a source resolved but held no bytes
};

struct KeyLoadError {
    KeyErrorKind kind;
    std::string message;  // human-facing, already names the source it tried
};

// Interprets `content` as a key: base64-decoded when it is valid base64 (the
// CI/secret transport form - a GitHub secret is text and cannot carry a raw
// binary key), otherwise taken as raw key bytes. Returns nullopt only when the
// content is empty. This is the single decode every path shares, exposed so a
// caller that already holds the bytes - the Shield app reading its bundled
// asset - decodes identically to the CLI. Hex is deliberately not a format: a
// hex string is itself valid base64, so the two cannot be auto-distinguished.
// See docs/concepts/object-signing.md.
[[nodiscard]] std::optional<SigningKey> decode_signing_key(std::span<const std::byte> content);

// Resolves a key from, in order: `explicit_path` if non-empty (the CLI's
// signing-key= option), then $AC3FORGE_SIGNING_KEY_FILE (a path), then
// $AC3FORGE_SIGNING_KEY (inline). File and inline contents are decoded by
// decode_signing_key() above (base64 or raw). The env fallbacks let CI provide
// a key without a persisted file while the file form stays the documented
// default (a value passed inline shows up in `ps`/shell history; a path does
// not).
[[nodiscard]] std::expected<SigningKey, KeyLoadError> load_signing_key(
    std::string_view explicit_path);

}  // namespace ac3::signing
