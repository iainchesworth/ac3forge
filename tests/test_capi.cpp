// ac3forge_c (roadmap F1) round-trips and error paths, exercised from C++ via
// Catch2 like every other test here - see examples/capi_encode_decode.c for
// the companion check that the header genuinely compiles as C, not merely as
// C++ parsing valid-C syntax.
//
// Real audio from the first frame onward matters: an all-zero frame takes
// the §7.2.2.1.1 all-zero bit-allocation path and exercises almost none of
// the encoder - see CONTRIBUTING.md on why silence is a bad test signal.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "ac3forge_c/ac3forge.h"

namespace {

void fill_tone(float* out, double hz, int frame, double rate) {
    for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        const double t = (frame * AC3FORGE_SAMPLES_PER_FRAME + n) / rate;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
}

}  // namespace

TEST_CASE("ac3forge_version reports a sane version", "[capi]") {
    const ac3forge_version_t version = ac3forge_version();
    CHECK(version.major >= 0);
    CHECK(version.full != nullptr);
}

TEST_CASE("ac3forge_status_message never returns null", "[capi]") {
    // Not testing an out-of-range cast to ac3forge_status_t here: for an
    // unfixed enum, a value outside the range its enumerators need is
    // unspecified per the standard, and GCC's -Wconversion (part of this
    // project's warnings-as-errors set) rightly flags constructing one. The
    // switch's own `default:` case (common.cpp) is simple enough not to need
    // a dedicated test for it.
    CHECK(std::string_view(ac3forge_status_message(AC3FORGE_OK)) == "ok");
    CHECK(ac3forge_status_message(AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD) != nullptr);
    CHECK(ac3forge_status_message(AC3FORGE_ERROR_DECODE_INVALID_STREAM) != nullptr);
}

TEST_CASE("ac3forge_encoder_config_init matches EncoderConfig{}'s own defaults", "[capi]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    CHECK(config.dialnorm == 31);
    CHECK(config.acmod == AC3FORGE_ACMOD_2_0);
    CHECK(config.fast_mdct == 1);
    CHECK(config.has_drc == 0);
    CHECK(config.has_dialnorm2 == 0);
}

TEST_CASE("AC-3 encode/decode round-trips through the C API", "[capi]") {
    ac3forge_encoder_config_t encoder_config;
    ac3forge_encoder_config_init(&encoder_config);
    encoder_config.bitrate_kbps = 192;
    encoder_config.acmod = AC3FORGE_ACMOD_2_0;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&encoder_config, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    CHECK(ac3forge_encoder_channel_count(encoder) == 2);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);
    REQUIRE(decoder != nullptr);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<std::byte> stream;

    for (int frame = 0; frame < 8; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};

        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                               &encoded) == AC3FORGE_OK);
        REQUIRE(encoded != nullptr);
        REQUIRE(ac3forge_bytes_size(encoded) > 0);

        ac3forge_decoded_frame_t* decoded = nullptr;
        const auto status = ac3forge_decoder_decode_frame(
            decoder, ac3forge_bytes_data(encoded), ac3forge_bytes_size(encoded), &decoded);
        REQUIRE(status == AC3FORGE_OK);
        REQUIRE(decoded != nullptr);

        CHECK(ac3forge_decoded_frame_acmod(decoded) == AC3FORGE_ACMOD_2_0);
        CHECK(ac3forge_decoded_frame_channel_count(decoded) == 2);
        CHECK(ac3forge_decoded_frame_samples_per_channel(decoded) == AC3FORGE_SAMPLES_PER_FRAME);
        CHECK(ac3forge_decoded_frame_dialnorm(decoded) == 31);
        CHECK(ac3forge_decoded_frame_channel_samples(decoded, 0) != nullptr);
        CHECK(ac3forge_decoded_frame_channel_samples(decoded, 1) != nullptr);
        CHECK(ac3forge_decoded_frame_channel_samples(decoded, 2) == nullptr);  // out of range

        ac3forge_decoded_frame_destroy(decoded);
        ac3forge_bytes_destroy(encoded);
    }

    ac3forge_decoder_destroy(decoder);
    ac3forge_encoder_destroy(encoder);
}

TEST_CASE("ac3forge_encoder_encode_frame rejects a mismatched channel/sample count", "[capi]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const float* one_channel[1] = {left.data()};
    ac3forge_bytes_t* encoded = nullptr;

    CHECK(ac3forge_encoder_encode_frame(encoder, one_channel, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                         &encoded) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(encoded == nullptr);

    const float* two_channels[2] = {left.data(), left.data()};
    CHECK(ac3forge_encoder_encode_frame(encoder, two_channels, 2, AC3FORGE_SAMPLES_PER_FRAME / 2,
                                         &encoded) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(encoded == nullptr);

    ac3forge_encoder_destroy(encoder);
}

TEST_CASE("ac3forge_decoder_decode_frame reports the same errors ac3::FrameDecoder does",
          "[capi]") {
    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&config, &decoder) == AC3FORGE_OK);

    const std::vector<uint8_t> garbage(16, 0xAB);
    ac3forge_decoded_frame_t* decoded = nullptr;
    const auto status =
        ac3forge_decoder_decode_frame(decoder, garbage.data(), garbage.size(), &decoded);
    // Not asserting which specific DecodeError this garbage maps to - that's an
    // implementation detail of the real bitstream parser, not something this
    // boundary layer should pin down. What matters here is that a decode
    // failure reports one of the mapped decode-error codes and leaves the
    // out-parameter untouched, exactly like every other failure path.
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(decoded == nullptr);

    ac3forge_decoder_destroy(decoder);
}

TEST_CASE("ac3forge_split_frames and ac3forge_stream_bsid see the same framing as the C++ API",
          "[capi]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};
        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                               &encoded) == AC3FORGE_OK);
        const auto* data = ac3forge_bytes_data(encoded);
        stream.insert(stream.end(), data, data + ac3forge_bytes_size(encoded));
        ac3forge_bytes_destroy(encoded);
    }
    ac3forge_encoder_destroy(encoder);

    ac3forge_spans_t* spans = nullptr;
    REQUIRE(ac3forge_split_frames(stream.data(), stream.size(), &spans) == AC3FORGE_OK);
    REQUIRE(spans != nullptr);
    CHECK(ac3forge_spans_count(spans) == 3);
    const auto first = ac3forge_spans_get(spans, 0);
    CHECK(first.offset == 0);
    CHECK(first.length > 0);
    ac3forge_spans_destroy(spans);

    int bsid = -1;
    REQUIRE(ac3forge_stream_bsid(stream.data(), stream.size(), &bsid) == AC3FORGE_OK);
    CHECK(bsid <= 8);  // classic AC-3, not Annex E
}

TEST_CASE("Atmos encode/decode round-trips OAMD position and JOC object audio", "[capi]") {
    ac3forge_atmos_config_t config;
    ac3forge_atmos_config_init(&config);

    ac3forge_atmos_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_atmos_encoder_create(&config, 1, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    REQUIRE(ac3forge_atmos_encoder_dynamic_object_count(encoder) == 1);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<float> object(AC3FORGE_SAMPLES_PER_FRAME);
    // The default placement (x=0.5, y=0.5, z=0.0, gain=1.0/0 dB) sits exactly
    // on OAMD's quantizer grid - see tests/test_oba.cpp's own comment on why
    // that makes an exact round-trip assertion valid rather than a tolerance.
    const ac3forge_object_placement_t placement{.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0,
                                                 .lfe_send = 0.0};

    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(object.data(), 1000.0, frame, 48000.0);
        const float* objects[1] = {object.data()};

        ac3forge_bytes_t* unit = nullptr;
        REQUIRE(ac3forge_atmos_encoder_encode_frame(encoder, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                                     &placement, 1, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);

        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, ac3forge_bytes_data(unit),
                                                        ac3forge_bytes_size(unit),
                                                        &substream) == AC3FORGE_OK);
        ac3forge_bytes_destroy(unit);

        if (substream == nullptr) {
            continue;  // held back for transient pre-noise processing; not used here, but tolerate it
        }

        CHECK(ac3forge_decoded_substream_has_object_metadata(substream) == 1);
        CHECK(ac3forge_decoded_substream_program_dynamic_only(substream) == 1);
        CHECK(ac3forge_decoded_substream_program_lfe(substream) == 1);
        CHECK(ac3forge_decoded_substream_program_dynamic_object_count(substream) == 1);

        double x = -1, y = -1, z = -1, gain_db = -1;
        ac3forge_decoded_substream_dynamic_object(substream, 0, &x, &y, &z, &gain_db);
        CHECK(x == 0.5);
        CHECK(y == 0.5);
        CHECK(z == 0.0);
        CHECK(gain_db == 0.0);

        REQUIRE(ac3forge_decoded_substream_object_audio_count(substream) == 1);
        CHECK(ac3forge_decoded_substream_object_audio(substream, 0) != nullptr);
        CHECK(ac3forge_decoded_substream_object_audio(substream, 1) == nullptr);  // out of range

        ac3forge_decoded_substream_destroy(substream);
    }

    ac3forge_eac3_decoder_destroy(decoder);
    ac3forge_atmos_encoder_destroy(encoder);
}
