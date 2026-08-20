// ac3forge_c (roadmap F1) round-trips and error paths, exercised from C++ via
// Catch2 like every other test here - see examples/capi_encode_decode.c for
// the companion check that the header genuinely compiles as C, not merely as
// C++ parsing valid-C syntax.
//
// Real audio from the first frame onward matters: an all-zero frame takes
// the §7.2.2.1.1 all-zero bit-allocation path and exercises almost none of
// the encoder - see CONTRIBUTING.md on why silence is a bad test signal.
//
// The E-AC-3 half of the surface is decode-only (the Atmos encoder is the C
// header's only Annex E producer), so the E-AC-3 tests below drive the C++
// encoder - the same tool combinations tests/encoder/test_eac3.cpp proves stack
// correctly - and hold the C decode surface to the behaviour
// tests/decoder/test_eac3_decoder.cpp establishes for the C++ one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3forge_c/ac3forge.h"

namespace {

void fill_tone(float* out, double hz, int frame, double rate) {
    for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        const double t = (frame * AC3FORGE_SAMPLES_PER_FRAME + n) / rate;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
}

// Direct sample SNR with the codec's 256-sample delay, skipping the warm-up
// frame at each end - the same measurement tests/decoder/test_eac3_decoder.cpp makes,
// so a C-boundary round trip is held against real decoded audio, not just a
// header parse.
double snr_db(const std::vector<float>& input, const std::vector<float>& decoded) {
    constexpr std::size_t kDelay = 256;
    constexpr std::size_t kSkip = 1536;
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = kSkip; i + kSkip < input.size(); ++i) {
        const double x = static_cast<double>(input[i - kDelay]);
        const double d = static_cast<double>(decoded[i]) - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

struct Eac3Stream {
    std::vector<uint8_t> bytes;              // the concatenated syncframes
    std::vector<std::vector<float>> source;  // per coded channel, full length
};

// Phase-continuous tones, one per coded channel, through the C++ E-AC-3
// encoder - the raw-byte input side of every C-API decode test below.
Eac3Stream encode_eac3_stream(ac3::eac3::FrameEncoder& encoder, const std::vector<double>& tones,
                              int frames) {
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(tones.size() == nchans);
    Eac3Stream out;
    out.source.resize(nchans);
    std::vector<std::vector<float>> block(nchans, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<std::span<const float>> views(nchans);
    for (int frame = 0; frame < frames; ++frame) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            views[ch] = block[ch];
            out.source[ch].insert(out.source[ch].end(), block[ch].begin(), block[ch].end());
        }
        const auto encoded = encoder.encode_frame(views);
        REQUIRE(encoded.has_value());
        const auto* data = reinterpret_cast<const uint8_t*>(encoded->data());
        out.bytes.insert(out.bytes.end(), data, data + encoded->size());
    }
    return out;
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
    // on OAMD's quantizer grid - see tests/oba/test_oba.cpp's own comment on why
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

TEST_CASE("E-AC-3 substreams round-trip through the C API across the Annex E tool combinations",
          "[capi][eac3]") {
    struct ToolCombo {
        const char* name;
        ac3::eac3::FrameConfig config;
    };
    const ToolCombo combos[] = {
        {"plain", {.bitrate_kbps = 192}},
        {"coupling", {.bitrate_kbps = 192, .coupling = true}},
        {"enhanced coupling", {.bitrate_kbps = 192, .coupling = true, .enhanced = true}},
        {"spx", {.bitrate_kbps = 192, .spx = true}},
        {"aht", {.bitrate_kbps = 192, .aht = true}},
        {"coupling+spx+aht", {.bitrate_kbps = 192, .coupling = true, .spx = true, .aht = true}},
        {"auto tools", {.bitrate_kbps = 192, .auto_tools = true}},
    };
    for (const auto& combo : combos) {
        INFO(combo.name);
        ac3::eac3::FrameEncoder encoder{combo.config};
        const auto stream = encode_eac3_stream(encoder, {1000.0, 800.0}, 4);

        ac3forge_decoder_config_t config;
        ac3forge_decoder_config_init(&config);
        ac3forge_eac3_decoder_t* decoder = nullptr;
        REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

        int bsid = -1;
        REQUIRE(ac3forge_stream_bsid(stream.bytes.data(), stream.bytes.size(), &bsid) ==
                AC3FORGE_OK);
        CHECK(bsid == 16);  // Annex E framing, where the AC-3 test above saw <= 8

        ac3forge_spans_t* spans = nullptr;
        REQUIRE(ac3forge_split_frames(stream.bytes.data(), stream.bytes.size(), &spans) ==
                AC3FORGE_OK);
        REQUIRE(ac3forge_spans_count(spans) == 4);

        std::vector<std::vector<float>> rendered(2);
        for (std::size_t i = 0; i < ac3forge_spans_count(spans); ++i) {
            const auto span = ac3forge_spans_get(spans, i);
            ac3forge_decoded_substream_t* substream = nullptr;
            REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder,
                                                           stream.bytes.data() + span.offset,
                                                           span.length,
                                                           &substream) == AC3FORGE_OK);
            // Nothing here uses transient pre-noise, so no frame is ever held
            // back - every decode returns a substream immediately.
            REQUIRE(substream != nullptr);

            CHECK(ac3forge_decoded_substream_is_independent(substream) == 1);
            CHECK(ac3forge_decoded_substream_id(substream) == 0);
            CHECK(ac3forge_decoded_substream_sample_rate(substream) == AC3FORGE_SAMPLE_RATE_48000);
            CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_2_0);
            CHECK(ac3forge_decoded_substream_lfe(substream) == 0);
            CHECK(ac3forge_decoded_substream_dialnorm(substream) == 31);
            CHECK(ac3forge_decoded_substream_numblkscod(substream) == 3);  // six blocks
            CHECK(ac3forge_decoded_substream_has_chanmap(substream) == 0);
            CHECK(ac3forge_decoded_substream_last_dependent(substream) == 0);
            CHECK(ac3forge_decoded_substream_location_map(substream) != 0);
            CHECK(ac3forge_decoded_substream_has_compr(substream) == 0);
            CHECK(ac3forge_decoded_substream_dynrng(substream, 0) == 0);
            REQUIRE(ac3forge_decoded_substream_channel_count(substream) == 2);
            CHECK(ac3forge_decoded_substream_samples_per_channel(substream) ==
                  AC3FORGE_SAMPLES_PER_FRAME);
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const float* samples = ac3forge_decoded_substream_channel_samples(substream, ch);
                REQUIRE(samples != nullptr);
                rendered[ch].insert(rendered[ch].end(), samples,
                                    samples + AC3FORGE_SAMPLES_PER_FRAME);
                // A steady sub-2 kHz tone never trips §8.2.2's transient
                // detector (see tests/decoder/test_eac3_decoder.cpp's tone-choice
                // comment), so no block may report the short transform.
                for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
                    CHECK(ac3forge_decoded_substream_block_switched(substream, ch, blk) == 0);
                }
            }
            ac3forge_decoded_substream_destroy(substream);
        }
        ac3forge_spans_destroy(spans);
        ac3forge_eac3_decoder_destroy(decoder);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            CHECK(snr_db(stream.source[ch], rendered[ch]) > 20.0);
        }
    }
}

TEST_CASE("E-AC-3 access units with a dependent substream cross the C API intact",
          "[capi][eac3]") {
    // 5.1.2: a 3/2+LFE bed plus one dependent substream carrying Vhl/Vhr -
    // the same layout family tests/decoder/test_eac3_decoder.cpp proves against the
    // C++ decoder; here the C access-unit surface is what walks it.
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = ac3::Acmod::k2_0,
                        .chanmap = ac3::eac3::chanmap::k512Height}}};
    ac3::eac3::AccessUnitEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 8);

    const std::vector<double> tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0};
    std::vector<std::vector<float>> block(8, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<std::span<const float>> views(8);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 3; ++frame) {
        for (std::size_t ch = 0; ch < 8; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        const auto* data = reinterpret_cast<const uint8_t*>(unit->bytes.data());
        stream.insert(stream.end(), data, data + unit->bytes.size());
    }

    // Framing first: three access units of two syncframes each.
    ac3forge_spans_t* units = nullptr;
    REQUIRE(ac3forge_split_access_units(stream.data(), stream.size(), &units) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(units) == 3);
    ac3forge_spans_t* frames = nullptr;
    REQUIRE(ac3forge_split_frames(stream.data(), stream.size(), &frames) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(frames) == 6);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<double> energy(8, 0.0);
    for (std::size_t i = 0; i < ac3forge_spans_count(units); ++i) {
        const auto span = ac3forge_spans_get(units, i);
        ac3forge_decoded_access_unit_t* unit = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit(decoder, stream.data() + span.offset,
                                                         span.length, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);

        CHECK(ac3forge_decoded_access_unit_sample_rate(unit) == AC3FORGE_SAMPLE_RATE_48000);
        CHECK(ac3forge_decoded_access_unit_acmod(unit) == AC3FORGE_ACMOD_3_2);
        CHECK(ac3forge_decoded_access_unit_dialnorm(unit) == 31);
        CHECK(ac3forge_decoded_access_unit_has_compr(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_compr(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_dynrng(unit, 0) == 0);  // no DRC profile configured
        CHECK(ac3forge_decoded_access_unit_numblkscod(unit) == 3);
        CHECK(ac3forge_decoded_access_unit_substream_count(unit) == 2);
        REQUIRE(ac3forge_decoded_access_unit_channel_count(unit) == 8);
        CHECK(ac3forge_decoded_access_unit_samples_per_channel(unit) == AC3FORGE_SAMPLES_PER_FRAME);

        // Table E2.5 location order: L first, the heights in the middle, the
        // LFE last of these eight. An out-of-range index takes the documented
        // L default.
        REQUIRE(ac3forge_decoded_access_unit_layout_count(unit) == 8);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 0) == AC3FORGE_LOCATION_L);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 5) == AC3FORGE_LOCATION_VHL);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 7) == AC3FORGE_LOCATION_LFE);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 8) == AC3FORGE_LOCATION_L);

        // No object metadata rode along, so the object accessors take their
        // no-programme defaults rather than reading anything.
        CHECK(ac3forge_decoded_access_unit_has_object_metadata(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_dynamic_only(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_lfe(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_bed(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_dynamic_object_count(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_object_audio_count(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_object_audio(unit, 0) == nullptr);
        double x = -1, y = -1, z = -1, gain_db = -1;
        ac3forge_decoded_access_unit_dynamic_object(unit, 0, &x, &y, &z, &gain_db);
        CHECK(x == 0.5);
        CHECK(y == 0.5);
        CHECK(z == 0.0);
        CHECK(gain_db == 0.0);

        for (std::size_t ch = 0; ch < 8; ++ch) {
            const float* samples = ac3forge_decoded_access_unit_channel_samples(unit, ch);
            REQUIRE(samples != nullptr);
            for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
                energy[ch] += static_cast<double>(samples[n]) * static_cast<double>(samples[n]);
            }
        }
        CHECK(ac3forge_decoded_access_unit_channel_samples(unit, 8) == nullptr);

        ac3forge_decoded_access_unit_destroy(unit);
    }
    // Real audio landed in every rendered channel, not just a header parse.
    for (std::size_t ch = 0; ch < 8; ++ch) {
        INFO("channel " << ch);
        CHECK(energy[ch] > 1.0);
    }
    ac3forge_eac3_decoder_destroy(decoder);

    // The same stream substream by substream, so the dependent's own bsi
    // fields cross the boundary too: the independent's acmod/lfe speak for
    // themselves, the dependent speaks through its chanmap.
    ac3forge_eac3_decoder_t* frame_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &frame_decoder) == AC3FORGE_OK);
    const auto ind_span = ac3forge_spans_get(frames, 0);
    ac3forge_decoded_substream_t* independent = nullptr;
    REQUIRE(ac3forge_eac3_decoder_decode_substream(frame_decoder, stream.data() + ind_span.offset,
                                                   ind_span.length, &independent) == AC3FORGE_OK);
    REQUIRE(independent != nullptr);
    CHECK(ac3forge_decoded_substream_is_independent(independent) == 1);
    CHECK(ac3forge_decoded_substream_acmod(independent) == AC3FORGE_ACMOD_3_2);
    CHECK(ac3forge_decoded_substream_lfe(independent) == 1);
    CHECK(ac3forge_decoded_substream_has_chanmap(independent) == 0);
    CHECK(ac3forge_decoded_substream_chanmap(independent) == 0);
    CHECK(ac3forge_decoded_substream_channel_count(independent) == 6);
    CHECK(ac3forge_decoded_substream_location_map(independent) != 0);
    ac3forge_decoded_substream_destroy(independent);

    const auto dep_span = ac3forge_spans_get(frames, 1);
    ac3forge_decoded_substream_t* dependent = nullptr;
    REQUIRE(ac3forge_eac3_decoder_decode_substream(frame_decoder, stream.data() + dep_span.offset,
                                                   dep_span.length, &dependent) == AC3FORGE_OK);
    REQUIRE(dependent != nullptr);
    CHECK(ac3forge_decoded_substream_is_independent(dependent) == 0);
    CHECK(ac3forge_decoded_substream_id(dependent) == 0);
    CHECK(ac3forge_decoded_substream_acmod(dependent) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_substream_has_chanmap(dependent) == 1);
    CHECK(ac3forge_decoded_substream_chanmap(dependent) == ac3::eac3::chanmap::k512Height);
    CHECK(ac3forge_decoded_substream_location_map(dependent) == ac3::eac3::chanmap::k512Height);
    CHECK(ac3forge_decoded_substream_last_dependent(dependent) == 1);
    ac3forge_decoded_substream_destroy(dependent);
    ac3forge_eac3_decoder_destroy(frame_decoder);

    ac3forge_spans_destroy(units);
    ac3forge_spans_destroy(frames);
}

TEST_CASE("E-AC-3 dual mono metadata crosses the C boundary on both decode surfaces",
          "[capi][eac3]") {
    // 1+1 with each programme's own dialnorm, DRC profile and heavy
    // compression - the configuration that populates every optional metadata
    // field the substream accessors expose, Ch2's included. The tones sit
    // well above the -20 dBFS dialogue target, so both compressors and both
    // range controllers actually act rather than idle at unity.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192,
         .acmod = ac3::Acmod::kDualMono,
         .dialnorm = 27,
         .dialnorm2 = 25,
         .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
         .heavy = ac3::meta::HeavyConfig{},
         .drc2 = ac3::meta::profile(ac3::meta::ProfileId::kMusicLight),
         .heavy2 = ac3::meta::HeavyConfig{}}};
    const auto stream = encode_eac3_stream(encoder, {900.0, 500.0}, 3);

    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_eac3_decoder_t* substream_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &substream_decoder) == AC3FORGE_OK);
    // A dual mono frame is a whole access unit on its own, so the same bytes
    // exercise the unit surface too - including its layout_count == 0 rule.
    ac3forge_eac3_decoder_t* unit_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &unit_decoder) == AC3FORGE_OK);

    ac3forge_spans_t* spans = nullptr;
    REQUIRE(ac3forge_split_frames(stream.bytes.data(), stream.bytes.size(), &spans) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(spans) == 3);

    bool saw_compr = false;
    bool saw_compr2 = false;
    bool saw_dynrng = false;
    bool saw_dynrng2 = false;
    bool saw_unit_compr = false;
    bool saw_unit_dynrng = false;
    for (std::size_t i = 0; i < ac3forge_spans_count(spans); ++i) {
        const auto span = ac3forge_spans_get(spans, i);

        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_substream(substream_decoder,
                                                       stream.bytes.data() + span.offset,
                                                       span.length, &substream) == AC3FORGE_OK);
        REQUIRE(substream != nullptr);
        CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_DUAL_MONO);
        CHECK(ac3forge_decoded_substream_dialnorm(substream) == 27);
        REQUIRE(ac3forge_decoded_substream_has_dialnorm2(substream) == 1);
        CHECK(ac3forge_decoded_substream_dialnorm2(substream) == 25);
        REQUIRE(ac3forge_decoded_substream_has_compr(substream) == 1);
        REQUIRE(ac3forge_decoded_substream_has_compr2(substream) == 1);
        saw_compr = saw_compr || ac3forge_decoded_substream_compr(substream) != 0;
        saw_compr2 = saw_compr2 || ac3forge_decoded_substream_compr2(substream) != 0;
        for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
            saw_dynrng = saw_dynrng || ac3forge_decoded_substream_dynrng(substream, blk) != 0;
            saw_dynrng2 = saw_dynrng2 || ac3forge_decoded_substream_dynrng2(substream, blk) != 0;
        }
        CHECK(ac3forge_decoded_substream_dynrng(substream, -1) == 0);
        CHECK(ac3forge_decoded_substream_dynrng(substream, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_substream_dynrng2(substream, -1) == 0);
        CHECK(ac3forge_decoded_substream_dynrng2(substream, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_substream_channel_count(substream) == 2);
        ac3forge_decoded_substream_destroy(substream);

        ac3forge_decoded_access_unit_t* unit = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit(unit_decoder,
                                                         stream.bytes.data() + span.offset,
                                                         span.length, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);
        CHECK(ac3forge_decoded_access_unit_acmod(unit) == AC3FORGE_ACMOD_DUAL_MONO);
        CHECK(ac3forge_decoded_access_unit_dialnorm(unit) == 27);
        CHECK(ac3forge_decoded_access_unit_substream_count(unit) == 1);
        CHECK(ac3forge_decoded_access_unit_channel_count(unit) == 2);
        // 1+1 has no Table E2.5 layout - two unrelated programmes - so the
        // count is 0 and any index takes the L default.
        CHECK(ac3forge_decoded_access_unit_layout_count(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 0) == AC3FORGE_LOCATION_L);
        REQUIRE(ac3forge_decoded_access_unit_has_compr(unit) == 1);
        saw_unit_compr = saw_unit_compr || ac3forge_decoded_access_unit_compr(unit) != 0;
        for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
            saw_unit_dynrng =
                saw_unit_dynrng || ac3forge_decoded_access_unit_dynrng(unit, blk) != 0;
        }
        CHECK(ac3forge_decoded_access_unit_dynrng(unit, -1) == 0);
        CHECK(ac3forge_decoded_access_unit_dynrng(unit, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        ac3forge_decoded_access_unit_destroy(unit);
    }
    CHECK(saw_compr);
    CHECK(saw_compr2);
    CHECK(saw_dynrng);
    CHECK(saw_dynrng2);
    CHECK(saw_unit_compr);
    CHECK(saw_unit_dynrng);

    ac3forge_spans_destroy(spans);
    ac3forge_eac3_decoder_destroy(unit_decoder);
    ac3forge_eac3_decoder_destroy(substream_decoder);
}

TEST_CASE("the C API holds back and flushes transient pre-noise frames like the C++ decoder",
          "[capi][eac3]") {
    // Mirrors "transient pre-noise processing holds a frame back then releases
    // it corrected" (tests/decoder/test_eac3_decoder.cpp). The silent frames here are
    // the tool's own semantics - frames that never switch a block release
    // immediately - not the test signal; the transient itself is real audio.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .transient_prenoise = true}};

    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

    // A fresh decoder has nothing buffered: flush hands over the documented
    // empty shape, not an error.
    ac3forge_decoded_substream_t** flushed = nullptr;
    std::size_t flushed_count = 99;
    REQUIRE(ac3forge_eac3_decoder_flush(decoder, &flushed, &flushed_count) == AC3FORGE_OK);
    CHECK(flushed == nullptr);
    CHECK(flushed_count == 0);

    const std::vector<float> silence(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const std::vector<std::span<const float>> silent_views{silence, silence};
    const auto decode_one = [&](const std::vector<std::byte>& frame,
                                ac3forge_decoded_substream_t** out) {
        return ac3forge_eac3_decoder_decode_substream(
            decoder, reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), out);
    };

    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silent_views);
        REQUIRE(frame.has_value());
        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(decode_one(*frame, &substream) == AC3FORGE_OK);
        REQUIRE(substream != nullptr);
        ac3forge_decoded_substream_destroy(substream);
    }

    constexpr int kOnset = 960;
    std::vector<float> transient(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    for (int n = kOnset; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    const std::vector<std::span<const float>> transient_views{transient, transient};
    const auto transient_frame = encoder.encode_frame(transient_views);
    REQUIRE(transient_frame.has_value());

    // transproce turns on: AC3FORGE_OK with a NULL substream is the held-back
    // signal, not an error - the header documents exactly this pair.
    ac3forge_decoded_substream_t* held = nullptr;
    REQUIRE(decode_one(*transient_frame, &held) == AC3FORGE_OK);
    CHECK(held == nullptr);

    const auto after = encoder.encode_frame(silent_views);
    REQUIRE(after.has_value());
    ac3forge_decoded_substream_t* released = nullptr;
    REQUIRE(decode_one(*after, &released) == AC3FORGE_OK);
    REQUIRE(released != nullptr);
    CHECK(ac3forge_decoded_substream_channel_count(released) == 2);
    ac3forge_decoded_substream_destroy(released);

    // The "after" frame is itself the one now being held back (buffered mode
    // is sticky once the tool has fired); flush is what hands it over at end
    // of stream.
    REQUIRE(ac3forge_eac3_decoder_flush(decoder, &flushed, &flushed_count) == AC3FORGE_OK);
    REQUIRE(flushed != nullptr);
    REQUIRE(flushed_count == 1);
    CHECK(ac3forge_decoded_substream_channel_count(flushed[0]) == 2);
    ac3forge_decoded_substream_array_destroy(flushed, flushed_count);

    ac3forge_eac3_decoder_destroy(decoder);
}

TEST_CASE("E-AC-3 C decode entry points reject bad arguments and bad bitstreams",
          "[capi][eac3]") {
    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);

    ac3forge_eac3_decoder_t* decoder = nullptr;
    CHECK(ac3forge_eac3_decoder_create(nullptr, &decoder) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(decoder == nullptr);
    REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

    const std::vector<uint8_t> garbage(16, 0xAB);
    ac3forge_decoded_substream_t* substream = nullptr;
    ac3forge_decoded_access_unit_t* unit = nullptr;
    ac3forge_decoded_substream_t** flushed = nullptr;
    std::size_t count = 0;

    CHECK(ac3forge_eac3_decoder_decode_substream(nullptr, garbage.data(), garbage.size(),
                                                 &substream) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_substream(decoder, nullptr, 0, &substream) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_substream(decoder, garbage.data(), garbage.size(),
                                                 nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit(nullptr, garbage.data(), garbage.size(),
                                                   &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit(decoder, nullptr, 0, &unit) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit(decoder, garbage.data(), garbage.size(),
                                                   nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_flush(nullptr, &flushed, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_flush(decoder, nullptr, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_flush(decoder, &flushed, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);

    // Same convention as the AC-3 decode-error test above: a bitstream
    // failure maps into the decode-error range and the out-parameter is left
    // untouched; which exact code the parser picks is its own business.
    auto status = ac3forge_eac3_decoder_decode_substream(decoder, garbage.data(), garbage.size(),
                                                         &substream);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(substream == nullptr);
    status = ac3forge_eac3_decoder_decode_access_unit(decoder, garbage.data(), garbage.size(),
                                                      &unit);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(unit == nullptr);

    // A real frame decodes on this same decoder; the same frame cut short is
    // an error again, not a crash and not a false success.
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    const auto stream = encode_eac3_stream(encoder, {1000.0, 800.0}, 1);
    REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, stream.bytes.data(),
                                                   stream.bytes.size(), &substream) == AC3FORGE_OK);
    REQUIRE(substream != nullptr);
    ac3forge_decoded_substream_destroy(substream);
    substream = nullptr;
    status = ac3forge_eac3_decoder_decode_substream(decoder, stream.bytes.data(),
                                                    stream.bytes.size() / 2, &substream);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(substream == nullptr);

    // The framing helpers police their own arguments the same way.
    ac3forge_spans_t* spans = nullptr;
    CHECK(ac3forge_split_frames(nullptr, 0, &spans) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_split_frames(garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_split_access_units(nullptr, 0, &spans) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_split_access_units(garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    status = ac3forge_split_access_units(garbage.data(), garbage.size(), &spans);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(spans == nullptr);

    int bsid = -1;
    CHECK(ac3forge_stream_bsid(nullptr, 0, &bsid) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_stream_bsid(garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    // bsid sits at bit 40; five bytes cannot hold it.
    CHECK(ac3forge_stream_bsid(garbage.data(), 5, &bsid) == AC3FORGE_ERROR_DECODE_TRUNCATED);

    ac3forge_eac3_decoder_destroy(decoder);
}

TEST_CASE("C accessors take their documented defaults on null handles", "[capi]") {
    // Every accessor tolerates NULL and answers with the default its header
    // comment names, and every _destroy is a free()-style no-op on NULL -
    // the whole-surface sweep of the convention the header leads with.
    ac3forge_encoder_destroy(nullptr);
    ac3forge_decoder_destroy(nullptr);
    ac3forge_eac3_decoder_destroy(nullptr);
    ac3forge_atmos_encoder_destroy(nullptr);
    ac3forge_bytes_destroy(nullptr);
    ac3forge_decoded_frame_destroy(nullptr);
    ac3forge_decoded_substream_destroy(nullptr);
    ac3forge_decoded_access_unit_destroy(nullptr);
    ac3forge_spans_destroy(nullptr);
    ac3forge_decoded_substream_array_destroy(nullptr, 5);

    CHECK(ac3forge_encoder_channel_count(nullptr) == 0);
    CHECK(ac3forge_atmos_encoder_dynamic_object_count(nullptr) == 0);
    CHECK(ac3forge_bytes_data(nullptr) == nullptr);
    CHECK(ac3forge_bytes_size(nullptr) == 0);
    CHECK(ac3forge_spans_count(nullptr) == 0);
    const auto span = ac3forge_spans_get(nullptr, 0);
    CHECK(span.offset == 0);
    CHECK(span.length == 0);

    CHECK(ac3forge_decoded_frame_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_decoded_frame_bitrate_kbps(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_frame_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dialnorm(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_has_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dynrng(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_frame_has_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_has_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dynrng2(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_frame_channel_count(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_samples_per_channel(nullptr) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_decoded_frame_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_decoded_frame_block_switched(nullptr, 0, 0) == 0);

    CHECK(ac3forge_decoded_substream_is_independent(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_id(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_decoded_substream_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_substream_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dialnorm(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_has_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dynrng(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_substream_has_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_has_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dynrng2(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_substream_numblkscod(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_has_chanmap(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_chanmap(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_last_dependent(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_location_map(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_channel_count(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_samples_per_channel(nullptr) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_decoded_substream_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_decoded_substream_block_switched(nullptr, 0, 0) == 0);
    CHECK(ac3forge_decoded_substream_has_object_metadata(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_dynamic_only(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_bed(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_dynamic_object_count(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_object_audio_count(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_object_audio(nullptr, 0) == nullptr);
    double x = -1, y = -1, z = -1, gain_db = -1;
    ac3forge_decoded_substream_dynamic_object(nullptr, 0, &x, &y, &z, &gain_db);
    CHECK(x == 0.5);
    CHECK(y == 0.5);
    CHECK(z == 0.0);
    CHECK(gain_db == 0.0);
    // NULL out-parameters are individually optional too.
    ac3forge_decoded_substream_dynamic_object(nullptr, 0, nullptr, nullptr, nullptr, nullptr);

    CHECK(ac3forge_decoded_access_unit_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_decoded_access_unit_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_access_unit_dialnorm(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_has_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_dynrng(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_access_unit_numblkscod(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_substream_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_channel_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_samples_per_channel(nullptr) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_decoded_access_unit_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_decoded_access_unit_layout_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_layout_location(nullptr, 0) == AC3FORGE_LOCATION_L);
    CHECK(ac3forge_decoded_access_unit_has_object_metadata(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_dynamic_only(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_bed(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_dynamic_object_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_object_audio_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_object_audio(nullptr, 0) == nullptr);
    x = -1;
    y = -1;
    z = -1;
    gain_db = -1;
    ac3forge_decoded_access_unit_dynamic_object(nullptr, 0, &x, &y, &z, &gain_db);
    CHECK(x == 0.5);
    CHECK(y == 0.5);
    CHECK(z == 0.0);
    CHECK(gain_db == 0.0);
}

TEST_CASE("every ac3forge status code carries its own message", "[capi]") {
    constexpr ac3forge_status_t codes[] = {AC3FORGE_OK,
                                           AC3FORGE_ERROR_INVALID_ARGUMENT,
                                           AC3FORGE_ERROR_OUT_OF_MEMORY,
                                           AC3FORGE_ERROR_INTERNAL,
                                           AC3FORGE_ERROR_ENCODE_INVALID_BITRATE,
                                           AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM,
                                           AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM,
                                           AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP,
                                           AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS,
                                           AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL,
                                           AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO,
                                           AC3FORGE_ERROR_DECODE_TRUNCATED,
                                           AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD,
                                           AC3FORGE_ERROR_DECODE_BAD_CRC,
                                           AC3FORGE_ERROR_DECODE_RESERVED_VALUE,
                                           AC3FORGE_ERROR_DECODE_UNSUPPORTED,
                                           AC3FORGE_ERROR_DECODE_INVALID_STREAM};
    for (const auto code : codes) {
        const char* message = ac3forge_status_message(code);
        REQUIRE(message != nullptr);
        // Every enumerator has its own case; the fallthrough string is
        // reserved for values outside the enum.
        CHECK(std::string_view(message) != "unknown status");
    }
}

TEST_CASE("C config initializers report the C++ defaults and tolerate NULL", "[capi]") {
    // NULL is documented as a no-op for every _init, matching _destroy's own
    // free()-like convention.
    ac3forge_heavy_config_init(nullptr);
    ac3forge_encoder_config_init(nullptr);
    ac3forge_decoder_config_init(nullptr);
    ac3forge_atmos_config_init(nullptr);

    ac3forge_heavy_config_t heavy;
    ac3forge_heavy_config_init(&heavy);
    CHECK(heavy.dialogue_target_dbfs == -20.0);
    CHECK(heavy.peak_ceiling_dbfs == -0.5);
    CHECK(heavy.release_db_per_second == 20.0);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    CHECK(decoder_config.drc_scale == 0.0);
    CHECK(decoder_config.heavy_compression == 0);

    ac3forge_atmos_config_t atmos;
    ac3forge_atmos_config_init(&atmos);
    CHECK(atmos.sample_rate == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(atmos.bitrate_kbps == 448);
    CHECK(atmos.dialnorm == 31);
    CHECK(atmos.num_bands_idx == 4);
    CHECK(atmos.fine_quant == 0);
    CHECK(atmos.emit_object_metadata == 1);
    CHECK(atmos.fast_mdct == 1);
}

TEST_CASE("Atmos C API tool variants round-trip objects and beds", "[capi][atmos]") {
    struct Variant {
        const char* name;
        int num_bands_idx;
        int fine_quant;
        int emit_object_metadata;
        int fast_mdct;
    };
    // The knobs the C header exposes on its one Annex E producer: the JOC
    // band count at both ends of Table 50, the fine quantizer, the reference
    // MDCT, and the container-omitted bed-only mode.
    const Variant variants[] = {
        {"fine quantizer, 5 bands", 2, 1, 1, 1},
        {"23 bands, reference MDCT", 7, 0, 1, 0},
        {"bed only, container omitted", 4, 0, 0, 1},
    };
    for (const auto& variant : variants) {
        INFO(variant.name);
        ac3forge_atmos_config_t config;
        ac3forge_atmos_config_init(&config);
        config.num_bands_idx = variant.num_bands_idx;
        config.fine_quant = variant.fine_quant;
        config.emit_object_metadata = variant.emit_object_metadata;
        config.fast_mdct = variant.fast_mdct;

        ac3forge_atmos_encoder_t* encoder = nullptr;
        REQUIRE(ac3forge_atmos_encoder_create(&config, 2, &encoder) == AC3FORGE_OK);
        CHECK(ac3forge_atmos_encoder_dynamic_object_count(encoder) == 2);

        ac3forge_decoder_config_t decoder_config;
        ac3forge_decoder_config_init(&decoder_config);
        ac3forge_eac3_decoder_t* decoder = nullptr;
        REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

        std::vector<float> object_a(AC3FORGE_SAMPLES_PER_FRAME);
        std::vector<float> object_b(AC3FORGE_SAMPLES_PER_FRAME);
        // Both placements sit on OAMD's quantizer grid (see the Atmos
        // round-trip test above), so position equality is exact.
        const ac3forge_object_placement_t placements[2] = {
            {.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0, .lfe_send = 0.0},
            {.x = 0.0, .y = 1.0, .z = 0.0, .gain = 1.0, .lfe_send = 0.0}};

        for (int frame = 0; frame < 2; ++frame) {
            fill_tone(object_a.data(), 1000.0, frame, 48000.0);
            fill_tone(object_b.data(), 600.0, frame, 48000.0);
            const float* objects[2] = {object_a.data(), object_b.data()};

            ac3forge_bytes_t* unit = nullptr;
            REQUIRE(ac3forge_atmos_encoder_encode_frame(encoder, objects, 2,
                                                        AC3FORGE_SAMPLES_PER_FRAME, placements, 2,
                                                        &unit) == AC3FORGE_OK);
            ac3forge_decoded_substream_t* substream = nullptr;
            REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, ac3forge_bytes_data(unit),
                                                           ac3forge_bytes_size(unit),
                                                           &substream) == AC3FORGE_OK);
            ac3forge_bytes_destroy(unit);
            REQUIRE(substream != nullptr);

            // Whatever the variant, a legacy decoder sees an ordinary 5.1 bed.
            CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_3_2);
            CHECK(ac3forge_decoded_substream_lfe(substream) == 1);
            REQUIRE(ac3forge_decoded_substream_channel_count(substream) == 6);
            CHECK(ac3forge_decoded_substream_channel_samples(substream, 0) != nullptr);

            if (variant.emit_object_metadata != 0) {
                CHECK(ac3forge_decoded_substream_has_object_metadata(substream) == 1);
                CHECK(ac3forge_decoded_substream_program_dynamic_only(substream) == 1);
                CHECK(ac3forge_decoded_substream_program_dynamic_object_count(substream) == 2);
                CHECK(ac3forge_decoded_substream_object_audio_count(substream) == 2);
                CHECK(ac3forge_decoded_substream_object_audio(substream, 1) != nullptr);
                double x = -1, y = -1, z = -1, gain_db = -1;
                ac3forge_decoded_substream_dynamic_object(substream, 1, &x, &y, &z, &gain_db);
                CHECK(x == 0.0);
                CHECK(y == 1.0);
                CHECK(z == 0.0);
                CHECK(gain_db == 0.0);
            } else {
                // The container was omitted, so nothing object-shaped
                // survives - the objects live on only inside the bed mix.
                CHECK(ac3forge_decoded_substream_has_object_metadata(substream) == 0);
                CHECK(ac3forge_decoded_substream_object_audio_count(substream) == 0);
            }
            ac3forge_decoded_substream_destroy(substream);
        }
        ac3forge_eac3_decoder_destroy(decoder);
        ac3forge_atmos_encoder_destroy(encoder);
    }
}

TEST_CASE("C encode entry points surface the encoder's own error codes", "[capi]") {
    // Each case maps one ac3::FrameError onto its C status through the
    // internal from_cpp() bridge - the encode-side sibling of the decode
    // range checks above.
    std::vector<float> samples(AC3FORGE_SAMPLES_PER_FRAME);
    fill_tone(samples.data(), 1000.0, 0, 48000.0);
    const float* stereo[2] = {samples.data(), samples.data()};

    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    ac3forge_encoder_t* encoder = nullptr;
    CHECK(ac3forge_encoder_create(nullptr, &encoder) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_encoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(encoder == nullptr);

    // 100 kbps is not one of Table 5.18's 19 nominal rates, so AC-3 (unlike
    // E-AC-3, which takes it - see tests/encoder/test_eac3.cpp) must refuse it.
    config.bitrate_kbps = 100;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    ac3forge_bytes_t* encoded = nullptr;
    CHECK(ac3forge_encoder_encode_frame(encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_BITRATE);
    CHECK(encoded == nullptr);
    ac3forge_encoder_destroy(encoder);

    // Dialnorm 0 is reserved (§5.4.2.8).
    ac3forge_encoder_config_init(&config);
    config.dialnorm = 0;
    encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    CHECK(ac3forge_encoder_encode_frame(encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM);
    CHECK(encoded == nullptr);
    ac3forge_encoder_destroy(encoder);

    // 1+1 without Ch2's own dialnorm2 is exactly as invalid as a missing
    // dialnorm would be.
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_DUAL_MONO;
    encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    CHECK(ac3forge_encoder_encode_frame(encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM);
    CHECK(encoded == nullptr);
    ac3forge_encoder_destroy(encoder);

    // The Atmos encoder rides the E-AC-3 frame writer, so its failures come
    // back through the same mapping.
    ac3forge_atmos_config_t atmos_config;
    ac3forge_atmos_config_init(&atmos_config);
    ac3forge_atmos_encoder_t* atmos = nullptr;
    CHECK(ac3forge_atmos_encoder_create(nullptr, 1, &atmos) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_create(&atmos_config, -1, &atmos) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_create(&atmos_config, 1, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(atmos == nullptr);

    const ac3forge_object_placement_t placement{.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0,
                                                .lfe_send = 0.0};
    const float* objects[1] = {samples.data()};
    ac3forge_bytes_t* unit = nullptr;

    // §E2.3.1.3: frmsiz is 11 bits, so 2000 kbps needs more words than any
    // legal E-AC-3 frame can signal.
    atmos_config.bitrate_kbps = 2000;
    REQUIRE(ac3forge_atmos_encoder_create(&atmos_config, 1, &atmos) == AC3FORGE_OK);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_ENCODE_INVALID_BITRATE);
    CHECK(unit == nullptr);

    // Mismatched counts and NULL spans are this layer's own argument checks,
    // caught before the encoder runs at all.
    CHECK(ac3forge_atmos_encoder_encode_frame(nullptr, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 2,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 0,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME / 2,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, nullptr, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    const float* null_object[1] = {nullptr};
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, null_object, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(unit == nullptr);
    ac3forge_atmos_encoder_destroy(atmos);

    // TS 103 420 §8.3.2.2 caps the complexity index at 16; the bed's LFE
    // counts, so sixteen dynamic objects push the programme to seventeen.
    atmos_config.bitrate_kbps = 448;
    atmos = nullptr;
    REQUIRE(ac3forge_atmos_encoder_create(&atmos_config, 16, &atmos) == AC3FORGE_OK);
    std::vector<const float*> many_objects(16, samples.data());
    std::vector<ac3forge_object_placement_t> many_placements(16, placement);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, many_objects.data(), 16,
                                              AC3FORGE_SAMPLES_PER_FRAME, many_placements.data(),
                                              16,
                                              &unit) == AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO);
    CHECK(unit == nullptr);
    ac3forge_atmos_encoder_destroy(atmos);
}

TEST_CASE("AC-3 dual mono metadata crosses the C boundary per channel", "[capi]") {
    // The AC-3 sibling of the E-AC-3 dual mono test above, for the decoded
    // frame's own Ch2 accessors.
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.bitrate_kbps = 192;
    config.acmod = AC3FORGE_ACMOD_DUAL_MONO;
    config.dialnorm = 27;
    config.has_dialnorm2 = 1;
    config.dialnorm2 = 25;
    config.has_drc = 1;
    config.drc_profile = AC3FORGE_DRC_FILM_STANDARD;
    config.has_heavy = 1;  // the _init defaults are a real heavy profile
    config.has_drc2 = 1;
    config.drc2_profile = AC3FORGE_DRC_MUSIC_LIGHT;
    config.has_heavy2 = 1;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<float> ch1(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> ch2(AC3FORGE_SAMPLES_PER_FRAME);
    bool saw_compr = false;
    bool saw_compr2 = false;
    bool saw_dynrng = false;
    bool saw_dynrng2 = false;
    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(ch1.data(), 900.0, frame, 48000.0);
        fill_tone(ch2.data(), 500.0, frame, 48000.0);
        const float* channels[2] = {ch1.data(), ch2.data()};

        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              &encoded) == AC3FORGE_OK);
        ac3forge_decoded_frame_t* decoded = nullptr;
        REQUIRE(ac3forge_decoder_decode_frame(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded),
                                              &decoded) == AC3FORGE_OK);
        ac3forge_bytes_destroy(encoded);
        REQUIRE(decoded != nullptr);

        CHECK(ac3forge_decoded_frame_acmod(decoded) == AC3FORGE_ACMOD_DUAL_MONO);
        CHECK(ac3forge_decoded_frame_sample_rate(decoded) == AC3FORGE_SAMPLE_RATE_48000);
        CHECK(ac3forge_decoded_frame_bitrate_kbps(decoded) == 192);
        CHECK(ac3forge_decoded_frame_lfe(decoded) == 0);
        CHECK(ac3forge_decoded_frame_dialnorm(decoded) == 27);
        REQUIRE(ac3forge_decoded_frame_has_dialnorm2(decoded) == 1);
        CHECK(ac3forge_decoded_frame_dialnorm2(decoded) == 25);
        REQUIRE(ac3forge_decoded_frame_has_compr(decoded) == 1);
        REQUIRE(ac3forge_decoded_frame_has_compr2(decoded) == 1);
        saw_compr = saw_compr || ac3forge_decoded_frame_compr(decoded) != 0;
        saw_compr2 = saw_compr2 || ac3forge_decoded_frame_compr2(decoded) != 0;
        for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
            saw_dynrng = saw_dynrng || ac3forge_decoded_frame_dynrng(decoded, blk) != 0;
            saw_dynrng2 = saw_dynrng2 || ac3forge_decoded_frame_dynrng2(decoded, blk) != 0;
        }
        CHECK(ac3forge_decoded_frame_dynrng(decoded, -1) == 0);
        CHECK(ac3forge_decoded_frame_dynrng(decoded, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_frame_dynrng2(decoded, -1) == 0);
        CHECK(ac3forge_decoded_frame_dynrng2(decoded, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_frame_block_switched(decoded, 0, 0) == 0);
        CHECK(ac3forge_decoded_frame_block_switched(decoded, 2, 0) == 0);   // channel OOR
        CHECK(ac3forge_decoded_frame_block_switched(decoded, 0, -1) == 0);  // block OOR

        ac3forge_decoded_frame_destroy(decoded);
    }
    CHECK(saw_compr);
    CHECK(saw_compr2);
    CHECK(saw_dynrng);
    CHECK(saw_dynrng2);

    ac3forge_decoder_destroy(decoder);
    ac3forge_encoder_destroy(encoder);
}
