#include "shield_signing_hook.hpp"

#include <sys/types.h>  // off_t

#include <utility>

#include <android/asset_manager.h>
#include <android/log.h>

#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"

namespace ac3shield {
namespace {

constexpr char kLogTag[] = "ac3forge.shield.signing";

// The bundled key asset. Its contents are base64 or raw key bytes - CI writes
// the base64 ATMOS_SIGNING_KEY secret verbatim into it (_build.yml) and a local
// signed build can drop in either form; decode_signing_key() below handles
// both, the same way the desktop CLI does. Gitignored; a build without it is
// the safe unsigned app.
constexpr char kSigningKeyAsset[] = "signing.key";

// A function-local static rather than a namespace-scope global, so there is no
// non-trivial constructor running before main() - init_signing() fills it in
// once the AAssetManager is available, and the others read it.
ac3::signing::SigningKey& key_slot() {
    static ac3::signing::SigningKey key;
    return key;
}

}  // namespace

void init_signing(AAssetManager* asset_manager) {
    if (asset_manager == nullptr) {
        return;
    }
    AAsset* asset = AAssetManager_open(asset_manager, kSigningKeyAsset, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "no '%s' asset - object signing disabled, streaming bed51",
                            kSigningKeyAsset);
        return;
    }
    const off_t length = AAsset_getLength(asset);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    const int read_bytes = AAsset_read(asset, bytes.data(), static_cast<std::size_t>(length));
    AAsset_close(asset);
    if (length <= 0 || read_bytes != length) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "'%s' read incomplete (%d/%lld) - object signing disabled",
                            kSigningKeyAsset, read_bytes, static_cast<long long>(length));
        return;
    }
    auto key = ac3::signing::decode_signing_key(bytes);
    if (!key) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "'%s' held no usable key - object signing disabled", kSigningKeyAsset);
        return;
    }
    key_slot() = std::move(*key);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "loaded signing key - object container will be signed");
}

bool signing_available() {
    return !key_slot().empty();
}

bool maybe_sign_atmos_unit(std::vector<std::byte>& unit) {
    if (key_slot().empty()) {
        return false;
    }
    return ac3::signing::sign_atmos_frame(unit, key_slot());
}

}  // namespace ac3shield
