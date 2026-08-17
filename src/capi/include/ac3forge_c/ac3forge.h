#ifndef AC3FORGE_C_H
#define AC3FORGE_C_H

#include <stddef.h>
#include <stdint.h>

#include "ac3forge_c/export.h"

/* ac3forge's C API — roadmap item F1: a stable, minimal C-callable surface
 * over the encode/decode core, for bindings and embedding by callers that
 * cannot or do not want to link C++23.
 *
 * Conventions used throughout:
 *   - Every handle type (ac3forge_encoder_t, ac3forge_decoded_frame_t, ...)
 *     is opaque; only pointers to it ever cross this header. Each has a
 *     matching _destroy function. Passing NULL to a _destroy function is a
 *     no-op, matching free()'s own convention.
 *   - Every fallible function returns ac3forge_status_t. AC3FORGE_OK is
 *     always zero, so `if (ac3forge_xxx(...) != AC3FORGE_OK)` and
 *     `if (status)` are equally correct.
 *   - A function that produces a variable-length or structured result writes
 *     an owned handle through an out-parameter (the pointee is left
 *     untouched on failure); the caller reads it through accessor functions
 *     and then destroys it. Nothing is returned through raw caller-supplied
 *     buffers, so no accessor here requires the caller to predict a size in
 *     advance.
 *   - This library has no ABI-compatibility promise before v1.0 (see
 *     roadmap item F5): a rebuild against a newer ac3forge may require a
 *     recompile, not merely a relink. ac3forge_version() reports what was
 *     actually linked at runtime.
 *
 * See docs/library/c-api.md for a worked example and the full ownership
 * discussion; examples/capi_encode_decode.c is the same walkthrough as a
 * buildable program.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- *
 * Status / error codes
 * --------------------------------------------------------------------- */

/* Mirrors ac3::FrameError (encode side) and ac3::DecodeError (decode side)
 * one-for-one, plus a handful of codes this layer itself can raise. Grouped
 * with gaps between groups so a future addition to either C++ enum gets its
 * own number without renumbering anything already shipped. */
typedef enum ac3forge_status {
    AC3FORGE_OK = 0,

    AC3FORGE_ERROR_INVALID_ARGUMENT = 1,
    AC3FORGE_ERROR_OUT_OF_MEMORY = 2,
    AC3FORGE_ERROR_INTERNAL = 3, /* an exception crossed the C boundary; see docs/library/c-api.md */

    /* ac3::FrameError — FrameEncoder::encode_frame(), AtmosEncoder::encode_frame() */
    AC3FORGE_ERROR_ENCODE_INVALID_BITRATE = 10,
    AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM = 11,
    AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM = 12,
    AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP = 13,
    AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS = 14,
    AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL = 15,
    AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO = 16,

    /* ac3::DecodeError — FrameDecoder::decode_frame(), Eac3Decoder::decode_substream()/decode_access_unit() */
    AC3FORGE_ERROR_DECODE_TRUNCATED = 30,
    AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD = 31,
    AC3FORGE_ERROR_DECODE_BAD_CRC = 32,
    AC3FORGE_ERROR_DECODE_RESERVED_VALUE = 33,
    AC3FORGE_ERROR_DECODE_UNSUPPORTED = 34,
    AC3FORGE_ERROR_DECODE_INVALID_STREAM = 35
} ac3forge_status_t;

/* A short, static, human-readable description of `status` — e.g. for a log
 * line. The returned pointer is to library-owned storage valid for the
 * process lifetime; never free() it. */
AC3FORGEC_API const char* ac3forge_status_message(ac3forge_status_t status);

/* --------------------------------------------------------------------- *
 * Version
 * --------------------------------------------------------------------- */

typedef struct ac3forge_version {
    int major;
    int minor;
    int patch;
    /* Semver plus prerelease suffix (e.g. "0.8.0-beta.1"), library-owned
     * storage valid for the process lifetime — mirrors ac3::version_full. */
    const char* full;
} ac3forge_version_t;

AC3FORGEC_API ac3forge_version_t ac3forge_version(void);

/* --------------------------------------------------------------------- *
 * Shared enums (ac3::SampleRate, ac3::Acmod)
 * --------------------------------------------------------------------- */

/* Ordinals match ac3::SampleRate exactly (A/52 Table 5.6 fscod, plus the
 * three Annex E fscod2 reduced rates). */
typedef enum ac3forge_sample_rate {
    AC3FORGE_SAMPLE_RATE_48000 = 0,
    AC3FORGE_SAMPLE_RATE_44100 = 1,
    AC3FORGE_SAMPLE_RATE_32000 = 2,
    AC3FORGE_SAMPLE_RATE_24000 = 3, /* E-AC-3 only */
    AC3FORGE_SAMPLE_RATE_22050 = 4, /* E-AC-3 only */
    AC3FORGE_SAMPLE_RATE_16000 = 5  /* E-AC-3 only */
} ac3forge_sample_rate_t;

/* Ordinals match ac3::Acmod exactly (A/52 Table 5.8). kDualMono (0) is 1+1:
 * two independent programmes sharing one syncframe, not a channel count. */
typedef enum ac3forge_acmod {
    AC3FORGE_ACMOD_DUAL_MONO = 0, /* 1+1: Ch1, Ch2 */
    AC3FORGE_ACMOD_1_0 = 1,       /* C */
    AC3FORGE_ACMOD_2_0 = 2,       /* L, R */
    AC3FORGE_ACMOD_3_0 = 3,       /* L, C, R */
    AC3FORGE_ACMOD_2_1 = 4,       /* L, R, S */
    AC3FORGE_ACMOD_3_1 = 5,       /* L, C, R, S */
    AC3FORGE_ACMOD_2_2 = 6,       /* L, R, SL, SR */
    AC3FORGE_ACMOD_3_2 = 7        /* L, C, R, SL, SR */
} ac3forge_acmod_t;

/* One audio block is always 256 samples (A/52 §4.1); one syncframe is always
 * six blocks. Exposed as constants because encode_frame()'s channel spans
 * and decoded-frame accessors are both sized against them. */
#define AC3FORGE_SAMPLES_PER_BLOCK 256
#define AC3FORGE_BLOCKS_PER_FRAME 6
#define AC3FORGE_SAMPLES_PER_FRAME 1536

/* Table 5.9 (§5.4.2.4) / Table 5.10 (§5.4.2.5). */
typedef enum ac3forge_centre_mix_level {
    AC3FORGE_CMIXLEV_MINUS_3DB = 0,
    AC3FORGE_CMIXLEV_MINUS_4_5DB = 1,
    AC3FORGE_CMIXLEV_MINUS_6DB = 2
} ac3forge_centre_mix_level_t;

typedef enum ac3forge_surround_mix_level {
    AC3FORGE_SURMIXLEV_MINUS_3DB = 0,
    AC3FORGE_SURMIXLEV_MINUS_6DB = 1,
    AC3FORGE_SURMIXLEV_SILENT = 2
} ac3forge_surround_mix_level_t;

/* The five conventional Dolby DRC curves (ac3::meta::ProfileId) — the same
 * named presets ac3cli's own --drc flag accepts. The full custom
 * ac3::meta::Profile curve (attack/release timing, boost ratios, ...) is an
 * internal tuning knob, not part of this minimal stable surface. */
typedef enum ac3forge_drc_profile {
    AC3FORGE_DRC_FILM_STANDARD = 0,
    AC3FORGE_DRC_FILM_LIGHT = 1,
    AC3FORGE_DRC_MUSIC_STANDARD = 2,
    AC3FORGE_DRC_MUSIC_LIGHT = 3,
    AC3FORGE_DRC_SPEECH = 4
} ac3forge_drc_profile_t;

/* ac3::meta::HeavyConfig verbatim (§7.7.2) — small enough, and specific
 * enough per-field, to expose directly rather than behind a preset. */
typedef struct ac3forge_heavy_config {
    double dialogue_target_dbfs; /* default -20.0 */
    double peak_ceiling_dbfs;    /* default -0.5 */
    double release_db_per_second; /* default 20.0 */
} ac3forge_heavy_config_t;

AC3FORGEC_API void ac3forge_heavy_config_init(ac3forge_heavy_config_t* config);

/* --------------------------------------------------------------------- *
 * AC-3 encoder (ac3::FrameEncoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_encoder ac3forge_encoder_t;

/* Mirrors ac3::EncoderConfig. `has_*` flags stand in for std::optional<T>,
 * since C has no direct equivalent — the paired field is read only when its
 * flag is non-zero. Call ac3forge_encoder_config_init() first so every field
 * this struct doesn't set explicitly carries the same default EncoderConfig{}
 * does; a zero-initialized struct is NOT equivalent (e.g. dialnorm 0 is
 * invalid — §5.4.2.8 reserves it — where EncoderConfig's real default is 31). */
typedef struct ac3forge_encoder_config {
    ac3forge_sample_rate_t sample_rate;
    uint32_t bitrate_kbps;
    int dialnorm; /* 1..31, §5.4.2.8 */

    int has_dialnorm2; /* dual mono (acmod == AC3FORGE_ACMOD_DUAL_MONO) only */
    int dialnorm2;

    int chbwcod; /* 0..60, or -1 for auto-from-bitrate */
    ac3forge_acmod_t acmod;
    int lfe;
    int coupling;
    int cplbegf; /* -1 = auto */
    int cplendf; /* -1 = auto */
    int fast_mdct;

    int has_drc;
    ac3forge_drc_profile_t drc_profile;
    int has_heavy;
    ac3forge_heavy_config_t heavy;

    /* Ch2's own DRC/heavy — dual mono only, no fallback to the above. */
    int has_drc2;
    ac3forge_drc_profile_t drc2_profile;
    int has_heavy2;
    ac3forge_heavy_config_t heavy2;

    ac3forge_centre_mix_level_t cmixlev;
    ac3forge_surround_mix_level_t surmixlev;
} ac3forge_encoder_config_t;

/* Fills `config` with the same defaults as ac3::EncoderConfig{}. */
AC3FORGEC_API void ac3forge_encoder_config_init(ac3forge_encoder_config_t* config);

AC3FORGEC_API ac3forge_status_t ac3forge_encoder_create(const ac3forge_encoder_config_t* config,
                                                         ac3forge_encoder_t** out_encoder);
AC3FORGEC_API void ac3forge_encoder_destroy(ac3forge_encoder_t* encoder);

/* Full-bandwidth channels (per config.acmod) plus, when config.lfe is set,
 * the LFE channel last — the same count encode_frame() below expects. */
AC3FORGEC_API size_t ac3forge_encoder_channel_count(const ac3forge_encoder_t* encoder);

/* An owned, immutable byte buffer — the result type for every function here
 * that produces one encoded frame's worth of bytes. */
typedef struct ac3forge_bytes ac3forge_bytes_t;

AC3FORGEC_API const uint8_t* ac3forge_bytes_data(const ac3forge_bytes_t* bytes);
AC3FORGEC_API size_t ac3forge_bytes_size(const ac3forge_bytes_t* bytes);
AC3FORGEC_API void ac3forge_bytes_destroy(ac3forge_bytes_t* bytes);

/* channels: `channel_count` pointers (must equal
 * ac3forge_encoder_channel_count(encoder)), each to exactly
 * AC3FORGE_SAMPLES_PER_FRAME samples nominally in [-1, 1), in AC-3 channel
 * order (Table 5.8) with LFE last. On success, *out_frame receives one
 * complete syncframe; the caller must destroy it. *out_frame is left
 * untouched on failure. */
AC3FORGEC_API ac3forge_status_t ac3forge_encoder_encode_frame(ac3forge_encoder_t* encoder,
                                                               const float* const* channels,
                                                               size_t channel_count,
                                                               size_t samples_per_channel,
                                                               ac3forge_bytes_t** out_frame);

/* --------------------------------------------------------------------- *
 * AC-3 decoder (ac3::FrameDecoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_decoder ac3forge_decoder_t;

/* Mirrors ac3::DecoderConfig. */
typedef struct ac3forge_decoder_config {
    double drc_scale;      /* 0.0..1.0, default 0.0 (§7.7.1's "Partial Compression") */
    int heavy_compression; /* default 0 */
} ac3forge_decoder_config_t;

AC3FORGEC_API void ac3forge_decoder_config_init(ac3forge_decoder_config_t* config);

AC3FORGEC_API ac3forge_status_t ac3forge_decoder_create(const ac3forge_decoder_config_t* config,
                                                         ac3forge_decoder_t** out_decoder);
AC3FORGEC_API void ac3forge_decoder_destroy(ac3forge_decoder_t* decoder);

/* One decoded syncframe (ac3::DecodedFrame), read through the accessors
 * below. */
typedef struct ac3forge_decoded_frame ac3forge_decoded_frame_t;

/* `frame` must be exactly one syncframe, same precondition as
 * FrameDecoder::decode_frame(). On success, *out_frame receives the decode
 * result; the caller must destroy it. */
AC3FORGEC_API ac3forge_status_t ac3forge_decoder_decode_frame(ac3forge_decoder_t* decoder,
                                                               const uint8_t* frame,
                                                               size_t frame_size,
                                                               ac3forge_decoded_frame_t** out_frame);

AC3FORGEC_API ac3forge_sample_rate_t ac3forge_decoded_frame_sample_rate(
    const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API uint32_t ac3forge_decoded_frame_bitrate_kbps(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API ac3forge_acmod_t ac3forge_decoded_frame_acmod(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API int ac3forge_decoded_frame_lfe(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API int ac3forge_decoded_frame_dialnorm(const ac3forge_decoded_frame_t* frame);

AC3FORGEC_API int ac3forge_decoded_frame_has_compr(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API uint8_t ac3forge_decoded_frame_compr(const ac3forge_decoded_frame_t* frame);
/* block_index in [0, AC3FORGE_BLOCKS_PER_FRAME). */
AC3FORGEC_API uint8_t ac3forge_decoded_frame_dynrng(const ac3forge_decoded_frame_t* frame,
                                                     int block_index);

/* Ch2's words — meaningful only when acmod() == AC3FORGE_ACMOD_DUAL_MONO. */
AC3FORGEC_API int ac3forge_decoded_frame_has_dialnorm2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API int ac3forge_decoded_frame_dialnorm2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API int ac3forge_decoded_frame_has_compr2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API uint8_t ac3forge_decoded_frame_compr2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_API uint8_t ac3forge_decoded_frame_dynrng2(const ac3forge_decoded_frame_t* frame,
                                                      int block_index);

AC3FORGEC_API size_t ac3forge_decoded_frame_channel_count(const ac3forge_decoded_frame_t* frame);
/* Always AC3FORGE_SAMPLES_PER_FRAME; exposed for a caller that would rather
 * not depend on the macro. */
AC3FORGEC_API size_t ac3forge_decoded_frame_samples_per_channel(
    const ac3forge_decoded_frame_t* frame);
/* channel_index in [0, channel_count()), AC-3 coded order (Table 5.8), LFE
 * last when lfe() is set. The returned pointer is valid until `frame` is
 * destroyed. */
AC3FORGEC_API const float* ac3forge_decoded_frame_channel_samples(
    const ac3forge_decoded_frame_t* frame, size_t channel_index);
/* True where that block used the short (block-switched) transform.
 * channel_index only ranges over the full-bandwidth channels (no LFE/coupling
 * entry — see DecodedFrame::blksw); out of range returns 0. */
AC3FORGEC_API int ac3forge_decoded_frame_block_switched(const ac3forge_decoded_frame_t* frame,
                                                         size_t channel_index, int block_index);

AC3FORGEC_API void ac3forge_decoded_frame_destroy(ac3forge_decoded_frame_t* frame);

/* --------------------------------------------------------------------- *
 * E-AC-3 / Atmos decode (ac3::Eac3Decoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_eac3_decoder ac3forge_eac3_decoder_t;

AC3FORGEC_API ac3forge_status_t ac3forge_eac3_decoder_create(
    const ac3forge_decoder_config_t* config, ac3forge_eac3_decoder_t** out_decoder);
AC3FORGEC_API void ac3forge_eac3_decoder_destroy(ac3forge_eac3_decoder_t* decoder);

typedef struct ac3forge_decoded_substream ac3forge_decoded_substream_t;
typedef struct ac3forge_decoded_access_unit ac3forge_decoded_access_unit_t;

/* Same std::nullopt-via-out-parameter convention as the C++ API: a return of
 * AC3FORGE_OK with *out_substream left NULL means this frame's PCM is being
 * held back pending transient pre-noise processing (§3.7) — not an error.
 * Call ac3forge_eac3_decoder_flush() at end of stream to collect it. */
AC3FORGEC_API ac3forge_status_t ac3forge_eac3_decoder_decode_substream(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* frame, size_t frame_size,
    ac3forge_decoded_substream_t** out_substream);

/* Same held-back convention as decode_substream, for the same reason (see
 * ac3::Eac3Decoder::decode_access_unit's own comment). `unit` must be
 * delimited exactly as ac3forge_split_access_units() would delimit it. */
AC3FORGEC_API ac3forge_status_t ac3forge_eac3_decoder_decode_access_unit(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    ac3forge_decoded_access_unit_t** out_unit);

/* Releases whichever frames transient pre-noise processing is still holding
 * back. *out_substreams receives a library-owned array of *out_count owned
 * handles (each must still be individually destroyed); *out_count is 0 (and
 * *out_substreams NULL) for a stream that never used the tool. Free the array
 * itself with ac3forge_decoded_substream_array_destroy(). */
AC3FORGEC_API ac3forge_status_t ac3forge_eac3_decoder_flush(
    ac3forge_eac3_decoder_t* decoder, ac3forge_decoded_substream_t*** out_substreams,
    size_t* out_count);
AC3FORGEC_API void ac3forge_decoded_substream_array_destroy(
    ac3forge_decoded_substream_t** substreams, size_t count);

/* --- ac3::DecodedSubstream accessors ------------------------------------ */

AC3FORGEC_API int ac3forge_decoded_substream_is_independent(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_id(const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API ac3forge_sample_rate_t ac3forge_decoded_substream_sample_rate(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API ac3forge_acmod_t ac3forge_decoded_substream_acmod(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_lfe(const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_dialnorm(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_has_compr(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API uint8_t ac3forge_decoded_substream_compr(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API uint8_t ac3forge_decoded_substream_dynrng(
    const ac3forge_decoded_substream_t* substream, int block_index);
AC3FORGEC_API int ac3forge_decoded_substream_has_dialnorm2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_dialnorm2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_has_compr2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API uint8_t ac3forge_decoded_substream_compr2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API uint8_t ac3forge_decoded_substream_dynrng2(
    const ac3forge_decoded_substream_t* substream, int block_index);
AC3FORGEC_API int ac3forge_decoded_substream_numblkscod(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_has_chanmap(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API uint16_t ac3forge_decoded_substream_chanmap(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_last_dependent(
    const ac3forge_decoded_substream_t* substream);
/* Table E2.5 location map this substream's channels occupy — chanmap() when
 * present, the acmod/lfe-derived map otherwise. */
AC3FORGEC_API uint16_t ac3forge_decoded_substream_location_map(
    const ac3forge_decoded_substream_t* substream);

AC3FORGEC_API size_t ac3forge_decoded_substream_channel_count(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API size_t ac3forge_decoded_substream_samples_per_channel(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API const float* ac3forge_decoded_substream_channel_samples(
    const ac3forge_decoded_substream_t* substream, size_t channel_index);
AC3FORGEC_API int ac3forge_decoded_substream_block_switched(
    const ac3forge_decoded_substream_t* substream, size_t channel_index, int block_index);

/* --- object audio: OAMD (ac3::oba::DecodedProgram) + JOC reconstruction -- */

AC3FORGEC_API int ac3forge_decoded_substream_has_object_metadata(
    const ac3forge_decoded_substream_t* substream);
/* Below are only meaningful when has_object_metadata() is non-zero. */
AC3FORGEC_API int ac3forge_decoded_substream_program_dynamic_only(
    const ac3forge_decoded_substream_t* substream);
/* b_lfe_present — dynamic_only programmes only; see program_bed() for the
 * bed-instance branch's own LFE flag (bit AC3FORGE_BED_LFE). */
AC3FORGEC_API int ac3forge_decoded_substream_program_lfe(
    const ac3forge_decoded_substream_t* substream);
/* Table 12 bed-instance channel assignment bitmask (0 when dynamic_only). */
AC3FORGEC_API uint16_t ac3forge_decoded_substream_program_bed(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API int ac3forge_decoded_substream_program_dynamic_object_count(
    const ac3forge_decoded_substream_t* substream);
/* object_index in [0, program_dynamic_object_count()). Position is §4.2.1's
 * room-anchored, left-handed, normalized-to-the-room-cuboid system: x in
 * [0, 1] left to right, y in [0, 1] front to back, z in [-1, 1] floor to
 * ceiling. gain_db is §5.6.1.4's object gain. */
AC3FORGEC_API void ac3forge_decoded_substream_dynamic_object(
    const ac3forge_decoded_substream_t* substream, int object_index, double* out_x, double* out_y,
    double* out_z, double* out_gain_db);

/* JOC's reconstructed per-object audio, parallel to the dynamic objects
 * above (same index = same object) — 0 when no JOC payload rode alongside
 * the OAMD one. Each waveform is samples_per_channel() samples long. */
AC3FORGEC_API size_t ac3forge_decoded_substream_object_audio_count(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_API const float* ac3forge_decoded_substream_object_audio(
    const ac3forge_decoded_substream_t* substream, size_t object_index);

AC3FORGEC_API void ac3forge_decoded_substream_destroy(ac3forge_decoded_substream_t* substream);

/* --- Table 12 bed-instance channel assignment bits (program_bed()) ------ */
#define AC3FORGE_BED_LR (1u << 9)
#define AC3FORGE_BED_C (1u << 8)
#define AC3FORGE_BED_LFE (1u << 7)
#define AC3FORGE_BED_LS_RS (1u << 6)
#define AC3FORGE_BED_LB_RB (1u << 5)
#define AC3FORGE_BED_TFL_TFR (1u << 4)
#define AC3FORGE_BED_TSL_TSR (1u << 3)
#define AC3FORGE_BED_TBL_TBR (1u << 2)
#define AC3FORGE_BED_LW_RW (1u << 1)
#define AC3FORGE_BED_LFE2 (1u << 0)

/* --- ac3::eac3::Location (Table E2.5), used by location_map()/layout ---- */
typedef enum ac3forge_location {
    AC3FORGE_LOCATION_L = 0,
    AC3FORGE_LOCATION_C = 1,
    AC3FORGE_LOCATION_R = 2,
    AC3FORGE_LOCATION_LS = 3,
    AC3FORGE_LOCATION_RS = 4,
    AC3FORGE_LOCATION_LC = 5,
    AC3FORGE_LOCATION_RC = 6,
    AC3FORGE_LOCATION_LRS = 7,
    AC3FORGE_LOCATION_RRS = 8,
    AC3FORGE_LOCATION_CS = 9,
    AC3FORGE_LOCATION_TS = 10,
    AC3FORGE_LOCATION_LSD = 11,
    AC3FORGE_LOCATION_RSD = 12,
    AC3FORGE_LOCATION_LW = 13,
    AC3FORGE_LOCATION_RW = 14,
    AC3FORGE_LOCATION_VHL = 15,
    AC3FORGE_LOCATION_VHR = 16,
    AC3FORGE_LOCATION_VHC = 17,
    AC3FORGE_LOCATION_LTS = 18,
    AC3FORGE_LOCATION_RTS = 19,
    AC3FORGE_LOCATION_LFE2 = 20,
    AC3FORGE_LOCATION_LFE = 21
} ac3forge_location_t;

/* --- ac3::DecodedAccessUnit accessors ----------------------------------- */

/* Same held-back-frame convention as decode_substream/decode_access_unit
 * above; see ac3::Eac3Decoder::decode_access_unit's own comment. */
AC3FORGEC_API ac3forge_sample_rate_t ac3forge_decoded_access_unit_sample_rate(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API ac3forge_acmod_t ac3forge_decoded_access_unit_acmod(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API int ac3forge_decoded_access_unit_dialnorm(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API int ac3forge_decoded_access_unit_has_compr(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API uint8_t ac3forge_decoded_access_unit_compr(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API uint8_t ac3forge_decoded_access_unit_dynrng(
    const ac3forge_decoded_access_unit_t* unit, int block_index);
AC3FORGEC_API int ac3forge_decoded_access_unit_numblkscod(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API int ac3forge_decoded_access_unit_substream_count(
    const ac3forge_decoded_access_unit_t* unit);

/* The rendered programme, laid out in Table E2.5 location order — parallel
 * to layout() below, except for dual mono (see layout_count()'s own
 * comment). */
AC3FORGEC_API size_t ac3forge_decoded_access_unit_channel_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API size_t ac3forge_decoded_access_unit_samples_per_channel(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API const float* ac3forge_decoded_access_unit_channel_samples(
    const ac3forge_decoded_access_unit_t* unit, size_t channel_index);

/* 0 for dual mono (acmod == AC3FORGE_ACMOD_DUAL_MONO): 1+1 has no Table
 * E2.5 layout, its two channels are unrelated programmes — see
 * ac3::DecodedAccessUnit::layout's own comment. Otherwise equal to
 * channel_count() above. */
AC3FORGEC_API size_t ac3forge_decoded_access_unit_layout_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API ac3forge_location_t ac3forge_decoded_access_unit_layout_location(
    const ac3forge_decoded_access_unit_t* unit, size_t index);

AC3FORGEC_API int ac3forge_decoded_access_unit_has_object_metadata(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API int ac3forge_decoded_access_unit_program_dynamic_only(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API int ac3forge_decoded_access_unit_program_lfe(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API uint16_t ac3forge_decoded_access_unit_program_bed(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API int ac3forge_decoded_access_unit_program_dynamic_object_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API void ac3forge_decoded_access_unit_dynamic_object(
    const ac3forge_decoded_access_unit_t* unit, int object_index, double* out_x, double* out_y,
    double* out_z, double* out_gain_db);
AC3FORGEC_API size_t ac3forge_decoded_access_unit_object_audio_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_API const float* ac3forge_decoded_access_unit_object_audio(
    const ac3forge_decoded_access_unit_t* unit, size_t object_index);

AC3FORGEC_API void ac3forge_decoded_access_unit_destroy(ac3forge_decoded_access_unit_t* unit);

/* --------------------------------------------------------------------- *
 * Stream framing helpers (ac3::split_frames / split_access_units / stream_bsid)
 * --------------------------------------------------------------------- */

/* A library-owned array of (offset, length) spans into the SAME buffer the
 * caller passed to ac3forge_split_frames()/ac3forge_split_access_units() —
 * the caller must keep that buffer alive and unmodified for as long as this
 * result is in use. */
typedef struct ac3forge_span {
    size_t offset;
    size_t length;
} ac3forge_span_t;

typedef struct ac3forge_spans ac3forge_spans_t;

AC3FORGEC_API size_t ac3forge_spans_count(const ac3forge_spans_t* spans);
AC3FORGEC_API ac3forge_span_t ac3forge_spans_get(const ac3forge_spans_t* spans, size_t index);
AC3FORGEC_API void ac3forge_spans_destroy(ac3forge_spans_t* spans);

/* Splits a raw elementary stream into syncframes by sync word and declared
 * size. Handles both AC-3 and E-AC-3. */
AC3FORGEC_API ac3forge_status_t ac3forge_split_frames(const uint8_t* stream, size_t stream_size,
                                                       ac3forge_spans_t** out_spans);
/* Groups those syncframes into access units — a new one begins at each
 * independent substream. */
AC3FORGEC_API ac3forge_status_t ac3forge_split_access_units(const uint8_t* stream,
                                                             size_t stream_size,
                                                             ac3forge_spans_t** out_spans);
/* bsid at bit 40, without committing to either generation. Fails only if
 * `frame` is too short to hold a header. */
AC3FORGEC_API ac3forge_status_t ac3forge_stream_bsid(const uint8_t* frame, size_t frame_size,
                                                      int* out_bsid);

/* --------------------------------------------------------------------- *
 * Atmos encode (ac3::oba::AtmosEncoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_atmos_encoder ac3forge_atmos_encoder_t;

/* Mirrors ac3::oba::AtmosConfig. */
typedef struct ac3forge_atmos_config {
    ac3forge_sample_rate_t sample_rate;
    uint32_t bitrate_kbps; /* default 448 */
    int dialnorm;
    int num_bands_idx; /* index into joc::kNumBands (Table 50); default 4 */
    int fine_quant;
    int emit_object_metadata; /* default 1 — see AtmosConfig's own comment on turning this off */
    int fast_mdct;
} ac3forge_atmos_config_t;

AC3FORGEC_API void ac3forge_atmos_config_init(ac3forge_atmos_config_t* config);

/* One object's placement for one frame — mirrors ac3::oba::ObjectPlacement.
 * Position is §4.2.1's room-anchored system, same ranges as
 * ac3forge_decoded_substream_dynamic_object()'s out_x/out_y/out_z above. */
typedef struct ac3forge_object_placement {
    double x, y, z;
    double gain;     /* linear, default 1.0 */
    double lfe_send; /* linear, default 0.0 — the only route an object reaches the LFE */
} ac3forge_object_placement_t;

AC3FORGEC_API ac3forge_status_t ac3forge_atmos_encoder_create(
    const ac3forge_atmos_config_t* config, int object_count,
    ac3forge_atmos_encoder_t** out_encoder);
AC3FORGEC_API void ac3forge_atmos_encoder_destroy(ac3forge_atmos_encoder_t* encoder);
AC3FORGEC_API int ac3forge_atmos_encoder_dynamic_object_count(
    const ac3forge_atmos_encoder_t* encoder);

/* objects: `object_count` (as given to _create) mono spans, each exactly
 * AC3FORGE_SAMPLES_PER_FRAME samples; placements: the same count, one per
 * object in the same order. On success, *out_unit receives one complete
 * E-AC-3 access unit (a single independent substream carrying the 5.1 bed,
 * with the EMDF object container in its aux data when
 * config.emit_object_metadata was set); the caller must destroy it. */
AC3FORGEC_API ac3forge_status_t ac3forge_atmos_encoder_encode_frame(
    ac3forge_atmos_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const ac3forge_object_placement_t* placements,
    size_t placement_count, ac3forge_bytes_t** out_unit);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AC3FORGE_C_H */
