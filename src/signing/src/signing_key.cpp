#include "ac3/signing/signing_key.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>

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

// A source's raw contents -> key bytes. Hex first (the documented, copy-paste
// friendly form), raw bytes only when the content is not valid hex, so a
// genuinely-binary 32-byte key file still works without the caller declaring
// which it is.
std::optional<std::vector<std::byte>> decode_key_content(std::string_view raw) {
    // Collect hex nibbles, ignoring ASCII whitespace. Any other character means
    // "this isn't hex" -> fall back to treating the whole content as raw bytes.
    std::string nibbles;
    nibbles.reserve(raw.size());
    bool looks_like_hex = true;
    for (const char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            nibbles.push_back(c);
        } else {
            looks_like_hex = false;
            break;
        }
    }

    if (looks_like_hex && !nibbles.empty() && nibbles.size() % 2 == 0) {
        std::vector<std::byte> out;
        out.reserve(nibbles.size() / 2);
        auto nibble_value = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;  // c is a hex digit by construction
        };
        for (std::size_t i = 0; i < nibbles.size(); i += 2) {
            const int hi = nibble_value(nibbles[i]);
            const int lo = nibble_value(nibbles[i + 1]);
            out.push_back(static_cast<std::byte>((hi << 4) | lo));
        }
        return out;
    }

    // Not hex (or an odd count of hex digits): take the content verbatim.
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
    auto bytes = decode_key_content(content);
    if (!bytes || bytes->empty()) {
        return std::unexpected(KeyLoadError{
            KeyErrorKind::kEmpty,
            std::string{"signing key from "} + std::string{source} + " is empty"});
    }
    return SigningKey{std::move(*bytes)};
}

}  // namespace

SigningKey::SigningKey(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

SigningKey::~SigningKey() {
    secure_zero(bytes_);
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

    // 3. $AC3FORGE_SIGNING_KEY (inline hex or raw)
    if (const char* env_inline = std::getenv("AC3FORGE_SIGNING_KEY");
        env_inline != nullptr && env_inline[0] != '\0') {
        return key_from_content(env_inline, "AC3FORGE_SIGNING_KEY");
    }

    return std::unexpected(KeyLoadError{KeyErrorKind::kAbsent, "no signing key provided"});
}

}  // namespace ac3::signing
