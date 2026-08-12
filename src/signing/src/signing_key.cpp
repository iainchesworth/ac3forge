#include "ac3/signing/signing_key.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace ac3::signing {
namespace {

// A key sits in freed heap after use unless scrubbed. std::fill on a soon-to-be
// freed buffer is a classic dead-store the optimizer may drop; volatile writes
// are not elidable, which is exactly the guarantee wanted here.
void secure_zero(std::vector<std::byte>& bytes) {
    volatile std::byte* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        p[i] = std::byte{0};
    }
}

// Standard base64 decode of an already-whitespace-free string. Returns the
// bytes only when `s` is canonical base64 (alphabet A-Za-z0-9+/, length a
// multiple of 4, '=' padding only at the end); nullopt otherwise, which the
// caller reads as "not base64, treat as raw".
std::optional<std::vector<std::byte>> try_base64_decode(std::string_view s) {
    if (s.empty() || s.size() % 4 != 0) {
        return std::nullopt;
    }
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::size_t pad = 0;
    if (s.back() == '=') {
        ++pad;
        if (s.size() >= 2 && s[s.size() - 2] == '=') {
            ++pad;
        }
    }
    std::vector<std::byte> out;
    out.reserve(s.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '=') {
            // Padding is only legal in the final `pad` positions.
            if (i < s.size() - pad) {
                return std::nullopt;
            }
            continue;
        }
        const int v = sextet(c);
        if (v < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

// A key source's contents -> key bytes. Base64 first (the CI/secret transport
// form - a GitHub secret is text and cannot hold a raw binary key), raw bytes
// otherwise. base64's alphabet is narrow enough that a genuinely random binary
// key effectively never validates as base64 (it would have to be all-base64
// characters AND a multiple of 4 bytes long), so the two are unambiguous in
// practice; hex is deliberately NOT accepted, since a hex string is itself
// valid base64 and the two could not be told apart. See
// docs/concepts/object-signing.md.
std::optional<std::vector<std::byte>> decode_key_content(std::string_view raw) {
    std::string stripped;
    stripped.reserve(raw.size());
    for (const char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            stripped.push_back(c);
        }
    }
    if (auto decoded = try_base64_decode(stripped)) {
        return decoded;
    }
    // Not base64: the content is the raw key, taken verbatim (a raw binary key
    // is byte-exact, so it is NOT whitespace-stripped the way the base64 test
    // above is).
    if (raw.empty()) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(raw.size());
    for (const char c : raw) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<std::string> read_file(std::string_view path) {
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::expected<SigningKey, KeyLoadError> key_from_content(std::string_view content,
                                                        std::string_view source) {
    auto key = decode_signing_key(std::span{reinterpret_cast<const std::byte*>(content.data()),
                                            content.size()});
    if (!key) {
        return std::unexpected(KeyLoadError{
            KeyErrorKind::kEmpty,
            std::string{"signing key from "} + std::string{source} + " is empty"});
    }
    return std::move(*key);
}

}  // namespace

SigningKey::SigningKey(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

SigningKey::~SigningKey() {
    secure_zero(bytes_);
}

std::optional<SigningKey> decode_signing_key(std::span<const std::byte> content) {
    const std::string_view sv{reinterpret_cast<const char*>(content.data()), content.size()};
    auto bytes = decode_key_content(sv);
    if (!bytes || bytes->empty()) {
        return std::nullopt;
    }
    return SigningKey{std::move(*bytes)};
}

std::expected<SigningKey, KeyLoadError> load_signing_key(std::string_view explicit_path) {
    // 1. --signing-key <path>
    if (!explicit_path.empty()) {
        auto content = read_file(explicit_path);
        if (!content) {
            return std::unexpected(KeyLoadError{
                KeyErrorKind::kUnreadable,
                std::string{"cannot read signing key file '"} + std::string{explicit_path} + "'"});
        }
        return key_from_content(*content,
                                std::string{"file '"} + std::string{explicit_path} + "'");
    }

    // 2. $AC3FORGE_SIGNING_KEY_FILE (a path)
    if (const char* env_path = std::getenv("AC3FORGE_SIGNING_KEY_FILE");
        env_path != nullptr && env_path[0] != '\0') {
        auto content = read_file(env_path);
        if (!content) {
            return std::unexpected(KeyLoadError{
                KeyErrorKind::kUnreadable,
                std::string{"cannot read signing key file '"} + env_path +
                    "' (from AC3FORGE_SIGNING_KEY_FILE)"});
        }
        return key_from_content(*content, std::string{"AC3FORGE_SIGNING_KEY_FILE ('"} + env_path +
                                              "')");
    }

    // 3. $AC3FORGE_SIGNING_KEY (inline base64 or raw)
    if (const char* env_inline = std::getenv("AC3FORGE_SIGNING_KEY");
        env_inline != nullptr && env_inline[0] != '\0') {
        return key_from_content(env_inline, "AC3FORGE_SIGNING_KEY");
    }

    return std::unexpected(KeyLoadError{KeyErrorKind::kAbsent, "no signing key provided"});
}

}  // namespace ac3::signing
