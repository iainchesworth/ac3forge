#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/oba/atmos.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"

// Internal crypto headers - on the include path for this target only (see
// tests/CMakeLists.txt), the same way the alsa backend's internal header is.
#include "hmac_sha256.hpp"
#include "sha256.hpp"

namespace {

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string to_hex(std::span<const std::byte> b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (const std::byte x : b) {
        const auto v = std::to_integer<unsigned>(x);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0xF]);
    }
    return out;
}

// A short synthetic tone, one frame long, for driving the Atmos encoder.
std::vector<float> tone(double hz, std::uint64_t start) {
    std::vector<float> out(static_cast<std::size_t>(ac3::kSamplesPerFrame));
    for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
    return out;
}

// Encodes `frames` one-object Atmos access units into a single contiguous
// stream, container emitted (or not) per `emit_objects`.
std::vector<std::byte> encode_atmos_stream(int frames, bool emit_objects) {
    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .num_bands_idx = 4, .emit_object_metadata = emit_objects}, 1};
    const std::array<ac3::oba::ObjectPlacement, 1> placement{{{}}};
    std::vector<std::span<const float>> views(1);
    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        const auto essence = tone(440.0, static_cast<std::uint64_t>(f) *
                                             static_cast<std::uint64_t>(ac3::kSamplesPerFrame));
        views[0] = essence;
        auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

ac3::signing::SigningKey make_key(std::uint8_t fill) {
    return ac3::signing::SigningKey{std::vector<std::byte>(32, std::byte{fill})};
}

}  // namespace

// --- Crypto known-answer vectors (FIPS 180-4 / RFC 4231) -------------------
// These lock the primitives against a spec, so a future refactor of the
// self-contained implementation can't silently change the tag it produces.
TEST_CASE("SHA-256 matches FIPS test vectors", "[signing][sha256]") {
    CHECK(to_hex(ac3::signing::sha256(as_bytes("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(ac3::signing::sha256(as_bytes(""))) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // A 56-byte message, exercising the pad-into-a-second-block boundary.
    CHECK(to_hex(ac3::signing::sha256(
              as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("HMAC-SHA-256 matches RFC 4231 test vectors", "[signing][hmac]") {
    SECTION("test case 1") {
        const std::vector<std::byte> key(20, std::byte{0x0b});
        CHECK(to_hex(ac3::signing::hmac_sha256(key, as_bytes("Hi There"))) ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    SECTION("test case 2") {
        CHECK(to_hex(ac3::signing::hmac_sha256(as_bytes("Jefe"),
                                               as_bytes("what do ya want for nothing?"))) ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    SECTION("test case 6 - key longer than the block size") {
        const std::vector<std::byte> key(131, std::byte{0xaa});
        CHECK(to_hex(ac3::signing::hmac_sha256(
                  key, as_bytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }
}

// --- Runtime key loading ----------------------------------------------------
TEST_CASE("load_signing_key reads a key file", "[signing][key]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path();

    SECTION("base64 contents decode to the raw key, whitespace ignored") {
        const fs::path p = dir / "ac3forge_test_key_b64.txt";
        {
            std::ofstream out{p};
            out << "AAECA/8=\n";  // base64 of {00,01,02,03,ff}
        }
        const auto key = ac3::signing::load_signing_key(p.string());
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == 5);
        CHECK(std::to_integer<int>(key->bytes()[0]) == 0x00);
        CHECK(std::to_integer<int>(key->bytes()[4]) == 0xff);
        fs::remove(p);
    }

    SECTION("non-base64 contents are taken as raw bytes") {
        const fs::path p = dir / "ac3forge_test_key_raw.bin";
        {
            std::ofstream out{p, std::ios::binary};
            // 5 bytes: length not a multiple of 4 and '!'/0x01 aren't base64,
            // so this is unambiguously raw.
            const char raw[] = {'k', 'e', 'y', '!', '\x01'};
            out.write(raw, sizeof raw);
        }
        const auto key = ac3::signing::load_signing_key(p.string());
        REQUIRE(key.has_value());
        CHECK(key->bytes().size() == 5);
        fs::remove(p);
    }

    SECTION("a missing path is an error, not an absent key") {
        const auto key =
            ac3::signing::load_signing_key((dir / "definitely_not_here_ac3forge.key").string());
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().kind == ac3::signing::KeyErrorKind::kUnreadable);
    }

    SECTION("an empty file resolves but yields no key") {
        const fs::path p = dir / "ac3forge_test_key_empty.txt";
        { std::ofstream out{p}; }
        const auto key = ac3::signing::load_signing_key(p.string());
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().kind == ac3::signing::KeyErrorKind::kEmpty);
        fs::remove(p);
    }
}

TEST_CASE("decode_signing_key accepts base64 or raw, and they agree", "[signing][key]") {
    const std::vector<std::byte> expected = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                                             std::byte{0x03}, std::byte{0xff}};

    SECTION("base64 decodes to the key bytes") {
        const std::string b64 = "AAECA/8=";
        const auto key = ac3::signing::decode_signing_key(as_bytes(b64));
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == expected.size());
        CHECK(std::equal(key->bytes().begin(), key->bytes().end(), expected.begin()));
    }

    SECTION("a raw binary key (non-base64 bytes) is taken verbatim") {
        // 32 bytes of 0xAB - 0xAB is not in the base64 alphabet, so unambiguous.
        const std::vector<std::byte> raw(32, std::byte{0xAB});
        const auto key = ac3::signing::decode_signing_key(raw);
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == 32);
        CHECK(std::to_integer<int>(key->bytes()[0]) == 0xAB);
    }

    SECTION("the base64 form and the raw form of one key produce the same key") {
        // Raw 32-byte key that contains a non-base64 byte so the raw form can't
        // be misread as base64; its base64 encoding must decode back to it.
        std::vector<std::byte> raw(32, std::byte{0xAB});
        raw[7] = std::byte{0x00};
        // Encode raw -> base64 here rather than hardcode, so the test can't drift.
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string enc;
        for (std::size_t i = 0; i < raw.size(); i += 3) {
            const unsigned b0 = std::to_integer<unsigned>(raw[i]);
            const unsigned b1 = i + 1 < raw.size() ? std::to_integer<unsigned>(raw[i + 1]) : 0;
            const unsigned b2 = i + 2 < raw.size() ? std::to_integer<unsigned>(raw[i + 2]) : 0;
            enc.push_back(kAlphabet[b0 >> 2]);
            enc.push_back(kAlphabet[((b0 & 0x3) << 4) | (b1 >> 4)]);
            enc.push_back(i + 1 < raw.size() ? kAlphabet[((b1 & 0xf) << 2) | (b2 >> 6)] : '=');
            enc.push_back(i + 2 < raw.size() ? kAlphabet[b2 & 0x3f] : '=');
        }
        const auto from_raw = ac3::signing::decode_signing_key(raw);
        const auto from_b64 = ac3::signing::decode_signing_key(as_bytes(enc));
        REQUIRE(from_raw.has_value());
        REQUIRE(from_b64.has_value());
        CHECK(std::equal(from_raw->bytes().begin(), from_raw->bytes().end(),
                         from_b64->bytes().begin(), from_b64->bytes().end()));
    }

    SECTION("empty content yields no key") {
        CHECK_FALSE(ac3::signing::decode_signing_key({}).has_value());
    }
}

// --- The signer over real encoder output ------------------------------------
TEST_CASE("sign_atmos_stream signs object frames deterministically", "[signing][emdf]") {
    const std::vector<std::byte> original = encode_atmos_stream(4, /*emit_objects=*/true);
    REQUIRE_FALSE(original.empty());

    const ac3::signing::SigningKey key_a = make_key(0x11);

    std::vector<std::byte> signed_a = original;
    const int n = ac3::signing::sign_atmos_stream(signed_a, key_a);

    SECTION("every object frame is signed and the bytes actually change") {
        CHECK(n == 4);
        CHECK(signed_a != original);
    }

    SECTION("signing is deterministic for a given key") {
        std::vector<std::byte> signed_again = original;
        CHECK(ac3::signing::sign_atmos_stream(signed_again, key_a) == n);
        CHECK(signed_again == signed_a);
    }

    SECTION("a different key produces a different tag") {
        std::vector<std::byte> signed_b = original;
        CHECK(ac3::signing::sign_atmos_stream(signed_b, make_key(0x22)) == n);
        CHECK(signed_b != signed_a);
    }
}

TEST_CASE("sign_atmos_stream is a no-op without a key or a container", "[signing][emdf]") {
    SECTION("an empty key signs nothing and leaves the stream untouched") {
        std::vector<std::byte> stream = encode_atmos_stream(2, /*emit_objects=*/true);
        const std::vector<std::byte> before = stream;
        CHECK(ac3::signing::sign_atmos_stream(stream, ac3::signing::SigningKey{}) == 0);
        CHECK(stream == before);
    }

    SECTION("a bed51 stream has no container to sign") {
        std::vector<std::byte> stream = encode_atmos_stream(2, /*emit_objects=*/false);
        const std::vector<std::byte> before = stream;
        CHECK(ac3::signing::sign_atmos_stream(stream, make_key(0x11)) == 0);
        CHECK(stream == before);
    }
}
