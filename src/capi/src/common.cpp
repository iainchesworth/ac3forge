#include "ac3/meta/drc.hpp"
#include "ac3/version.hpp"
#include "ac3forge_c/ac3forge.h"

extern "C" {

const char* ac3forge_status_message(ac3forge_status_t status) {
    switch (status) {
        case AC3FORGE_OK: return "ok";
        case AC3FORGE_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case AC3FORGE_ERROR_OUT_OF_MEMORY: return "out of memory";
        case AC3FORGE_ERROR_INTERNAL: return "internal error";
        case AC3FORGE_ERROR_ENCODE_INVALID_BITRATE: return "invalid bitrate";
        case AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM: return "invalid dialnorm";
        case AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM: return "invalid substream";
        case AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP: return "invalid channel map";
        case AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS: return "too many channels";
        case AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL: return "invalid mix level";
        case AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO: return "invalid object audio";
        case AC3FORGE_ERROR_DECODE_TRUNCATED: return "truncated frame";
        case AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD: return "bad sync word";
        case AC3FORGE_ERROR_DECODE_BAD_CRC: return "bad CRC";
        case AC3FORGE_ERROR_DECODE_RESERVED_VALUE: return "reserved value";
        case AC3FORGE_ERROR_DECODE_UNSUPPORTED: return "legal but unsupported syntax";
        case AC3FORGE_ERROR_DECODE_INVALID_STREAM: return "invalid stream";
    }
    return "unknown status";
}

ac3forge_version_t ac3forge_version(void) {
    return ac3forge_version_t{.major = ac3::version_major,
                               .minor = ac3::version_minor,
                               .patch = ac3::version_patch,
                               .full = ac3::version_full.data()};
}

void ac3forge_heavy_config_init(ac3forge_heavy_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac3::meta::HeavyConfig defaults{};
    *config = ac3forge_heavy_config_t{.dialogue_target_dbfs = defaults.dialogue_target_dbfs,
                                       .peak_ceiling_dbfs = defaults.peak_ceiling_dbfs,
                                       .release_db_per_second = defaults.release_db_per_second};
}

}  // extern "C"
