#pragma once

// Private to the C API's own implementation: the opaque handle definitions,
// the enum-ordinal contract this whole translation layer leans on, and the
// exception-to-status_t boundary every entry point in ac3forge.h crosses
// through. Never installed - a consumer only ever sees the opaque forward
// declarations in ac3forge_c/ac3forge.h.

#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3forge_c/ac3forge.h"

// --- enum-ordinal contract ---------------------------------------------
// ac3forge_c's enums are declared with the same ordinals as their C++
// counterparts on purpose, so translation is a bare static_cast rather than
// a switch - see e.g. encoder.cpp's to_cpp()/from_cpp() pairs. These
// static_asserts are what makes that safe: a future change to either side's
// enumerator order fails the build here rather than silently mistranslating.
static_assert(static_cast<int>(ac3::SampleRate::k48000) == AC3FORGE_SAMPLE_RATE_48000);
static_assert(static_cast<int>(ac3::SampleRate::k44100) == AC3FORGE_SAMPLE_RATE_44100);
static_assert(static_cast<int>(ac3::SampleRate::k32000) == AC3FORGE_SAMPLE_RATE_32000);
static_assert(static_cast<int>(ac3::SampleRate::k24000) == AC3FORGE_SAMPLE_RATE_24000);
static_assert(static_cast<int>(ac3::SampleRate::k22050) == AC3FORGE_SAMPLE_RATE_22050);
static_assert(static_cast<int>(ac3::SampleRate::k16000) == AC3FORGE_SAMPLE_RATE_16000);

static_assert(static_cast<int>(ac3::Acmod::kDualMono) == AC3FORGE_ACMOD_DUAL_MONO);
static_assert(static_cast<int>(ac3::Acmod::k1_0) == AC3FORGE_ACMOD_1_0);
static_assert(static_cast<int>(ac3::Acmod::k2_0) == AC3FORGE_ACMOD_2_0);
static_assert(static_cast<int>(ac3::Acmod::k3_0) == AC3FORGE_ACMOD_3_0);
static_assert(static_cast<int>(ac3::Acmod::k2_1) == AC3FORGE_ACMOD_2_1);
static_assert(static_cast<int>(ac3::Acmod::k3_1) == AC3FORGE_ACMOD_3_1);
static_assert(static_cast<int>(ac3::Acmod::k2_2) == AC3FORGE_ACMOD_2_2);
static_assert(static_cast<int>(ac3::Acmod::k3_2) == AC3FORGE_ACMOD_3_2);

static_assert(static_cast<int>(ac3::meta::CentreMixLevel::kMinus3dB) == AC3FORGE_CMIXLEV_MINUS_3DB);
static_assert(static_cast<int>(ac3::meta::CentreMixLevel::kMinus4_5dB) ==
              AC3FORGE_CMIXLEV_MINUS_4_5DB);
static_assert(static_cast<int>(ac3::meta::CentreMixLevel::kMinus6dB) == AC3FORGE_CMIXLEV_MINUS_6DB);

static_assert(static_cast<int>(ac3::meta::SurroundMixLevel::kMinus3dB) ==
              AC3FORGE_SURMIXLEV_MINUS_3DB);
static_assert(static_cast<int>(ac3::meta::SurroundMixLevel::kMinus6dB) ==
              AC3FORGE_SURMIXLEV_MINUS_6DB);
static_assert(static_cast<int>(ac3::meta::SurroundMixLevel::kSilent) == AC3FORGE_SURMIXLEV_SILENT);

static_assert(static_cast<int>(ac3::meta::ProfileId::kFilmStandard) == AC3FORGE_DRC_FILM_STANDARD);
static_assert(static_cast<int>(ac3::meta::ProfileId::kFilmLight) == AC3FORGE_DRC_FILM_LIGHT);
static_assert(static_cast<int>(ac3::meta::ProfileId::kMusicStandard) ==
              AC3FORGE_DRC_MUSIC_STANDARD);
static_assert(static_cast<int>(ac3::meta::ProfileId::kMusicLight) == AC3FORGE_DRC_MUSIC_LIGHT);
static_assert(static_cast<int>(ac3::meta::ProfileId::kSpeech) == AC3FORGE_DRC_SPEECH);

static_assert(AC3FORGE_SAMPLES_PER_FRAME == ac3::kSamplesPerFrame);
static_assert(AC3FORGE_BLOCKS_PER_FRAME == ac3::kBlocksPerFrame);
static_assert(AC3FORGE_SAMPLES_PER_BLOCK == ac3::kSamplesPerBlock);

namespace ac3forge_c {

[[nodiscard]] inline ac3forge_sample_rate_t from_cpp(ac3::SampleRate rate) {
    return static_cast<ac3forge_sample_rate_t>(rate);
}
[[nodiscard]] inline ac3::SampleRate to_cpp(ac3forge_sample_rate_t rate) {
    return static_cast<ac3::SampleRate>(rate);
}
[[nodiscard]] inline ac3forge_acmod_t from_cpp(ac3::Acmod acmod) {
    return static_cast<ac3forge_acmod_t>(acmod);
}
[[nodiscard]] inline ac3::Acmod to_cpp(ac3forge_acmod_t acmod) {
    return static_cast<ac3::Acmod>(acmod);
}
[[nodiscard]] inline ac3::meta::CentreMixLevel to_cpp(ac3forge_centre_mix_level_t level) {
    return static_cast<ac3::meta::CentreMixLevel>(level);
}
[[nodiscard]] inline ac3forge_centre_mix_level_t from_cpp(ac3::meta::CentreMixLevel level) {
    return static_cast<ac3forge_centre_mix_level_t>(level);
}
[[nodiscard]] inline ac3::meta::SurroundMixLevel to_cpp(ac3forge_surround_mix_level_t level) {
    return static_cast<ac3::meta::SurroundMixLevel>(level);
}
[[nodiscard]] inline ac3forge_surround_mix_level_t from_cpp(ac3::meta::SurroundMixLevel level) {
    return static_cast<ac3forge_surround_mix_level_t>(level);
}
[[nodiscard]] inline ac3::meta::ProfileId to_cpp(ac3forge_drc_profile_t profile) {
    return static_cast<ac3::meta::ProfileId>(profile);
}
[[nodiscard]] inline ac3::meta::HeavyConfig to_cpp(const ac3forge_heavy_config_t& config) {
    return ac3::meta::HeavyConfig{.dialogue_target_dbfs = config.dialogue_target_dbfs,
                                   .peak_ceiling_dbfs = config.peak_ceiling_dbfs,
                                   .release_db_per_second = config.release_db_per_second};
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac3::FrameError error) {
    switch (error) {
        case ac3::FrameError::kInvalidBitrate: return AC3FORGE_ERROR_ENCODE_INVALID_BITRATE;
        case ac3::FrameError::kInvalidDialnorm: return AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM;
        case ac3::FrameError::kInvalidSubstream: return AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM;
        case ac3::FrameError::kInvalidChannelMap: return AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP;
        case ac3::FrameError::kTooManyChannels: return AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS;
        case ac3::FrameError::kInvalidMixLevel: return AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL;
        case ac3::FrameError::kInvalidObjectAudio:
            return AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac3::DecodeError error) {
    switch (error) {
        case ac3::DecodeError::kTruncated: return AC3FORGE_ERROR_DECODE_TRUNCATED;
        case ac3::DecodeError::kBadSyncWord: return AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD;
        case ac3::DecodeError::kBadCrc: return AC3FORGE_ERROR_DECODE_BAD_CRC;
        case ac3::DecodeError::kReservedValue: return AC3FORGE_ERROR_DECODE_RESERVED_VALUE;
        case ac3::DecodeError::kUnsupported: return AC3FORGE_ERROR_DECODE_UNSUPPORTED;
        case ac3::DecodeError::kInvalidStream: return AC3FORGE_ERROR_DECODE_INVALID_STREAM;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

// Every entry point in ac3forge.h that can fail funnels through this: `body`
// returns ac3forge_status_t on its own successful path (AC3FORGE_OK or an
// error this layer chose deliberately), and any C++ exception that escapes
// it - std::bad_alloc from an allocation this layer or the codec core makes,
// or anything else - is caught here instead of crossing into the caller's
// (possibly non-C++) frame, which is undefined behaviour. The codec core
// itself never throws (see ac3::FrameError/DecodeError's std::expected
// convention), so in practice only allocation failure and caller-supplied
// nullptr/out-of-range arguments reach the catch clauses.
template <class F>
[[nodiscard]] ac3forge_status_t guard(F&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        return AC3FORGE_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return AC3FORGE_ERROR_INTERNAL;
    }
}

}  // namespace ac3forge_c

// --- opaque handle definitions ------------------------------------------
// Each wraps exactly one C++ value; construction only ever happens inside
// this library (via std::make_unique/new, released as a raw pointer through
// an out-parameter), so a handle's lifetime is entirely caller-driven from
// the matching _create/_destroy pair - the same convention every handle in
// ac3forge.h documents.

struct ac3forge_bytes {
    std::vector<std::byte> data;
};

struct ac3forge_encoder {
    explicit ac3forge_encoder(const ac3::EncoderConfig& config) : impl(config) {}
    ac3::FrameEncoder impl;
};

struct ac3forge_decoder {
    explicit ac3forge_decoder(const ac3::DecoderConfig& config) : impl(config) {}
    ac3::FrameDecoder impl;
};

struct ac3forge_decoded_frame {
    ac3::DecodedFrame data;
};

struct ac3forge_eac3_decoder {
    explicit ac3forge_eac3_decoder(const ac3::DecoderConfig& config) : impl(config) {}
    ac3::Eac3Decoder impl;
};

struct ac3forge_decoded_substream {
    ac3::DecodedSubstream data;
};

struct ac3forge_decoded_access_unit {
    ac3::DecodedAccessUnit data;
};

struct ac3forge_atmos_encoder {
    ac3forge_atmos_encoder(const ac3::oba::AtmosConfig& config, int objects) : impl(config, objects) {}
    ac3::oba::AtmosEncoder impl;
};

struct ac3forge_spans {
    std::vector<ac3forge_span_t> items;
};
