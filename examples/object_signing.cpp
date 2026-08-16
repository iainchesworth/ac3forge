// Sign an Atmos stream's EMDF object container.
//
// ac3::signing computes the keyed EMDF-protection tag a licensed decoder
// checks before it will decode a stream's OAMD/JOC container - see
// docs/concepts/object-signing.md. The key is always the operator's own to
// provision at runtime (an environment variable or a signing-key=<path> file
// in the CLI); the literal bytes below are a stand-in for that so this example
// needs nothing outside itself, not a real key for any real decoder.

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"

namespace {

std::vector<std::byte> as_bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

}  // namespace

int main() {
    constexpr int kObjects = 2;
    constexpr int kFrames = 31;  // one second

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};  // emit_object_metadata: default on

    std::vector<std::vector<float>> sources(kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (const auto& source : sources) {
        views.emplace_back(source);
    }
    const std::array<ac3::oba::ObjectPlacement, kObjects> placement{{
        {.position = {.x = 0.3, .y = 0.5, .z = 0.0}, .gain = 0.8},
        {.position = {.x = 0.7, .y = 0.5, .z = 0.0}, .gain = 0.8},
    }};

    std::vector<std::byte> stream;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("atmos encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const ac3::signing::SigningKey key{as_bytes("ac3forge-example-key-DO-NOT-USE")};
    const int signed_count = ac3::signing::sign_atmos_stream(stream, key);

    std::printf("signed %d of %d frames (%zu bytes)\n", signed_count, kFrames, stream.size());
    return signed_count == kFrames ? 0 : 1;
}
