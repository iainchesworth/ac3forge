#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"

// src/cli/main.cpp compiles directly into the ac3cli executable, everything
// in an anonymous namespace - there is no library surface parse_options,
// gather_frame or run_atmos_encode's own logic could be linked into this
// binary and called directly. So these are integration tests: they run the
// real, built ac3cli.exe as a subprocess (the same binary a build/verify
// step produces) and inspect what it actually wrote, rather than
// re-implementing its argument parsing against a copy of the source.
//
// AC3CLI_EXE (see tests/CMakeLists.txt) is the absolute path to that binary,
// supplied by CMake via $<TARGET_FILE:ac3cli> - these tests do not run at
// all if AC3FORGE_BUILD_CLI is OFF, the same way the alsa/android platform
// tests above do not run outside their own backend.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::temp_directory_path() / "ac3forge_cli_tests";
    fs::create_directories(dir);
    return dir;
}

// Runs `ac3cli <args>`, both streams redirected to `log` so a failing
// assertion can print exactly what the binary said. Returns whatever
// std::system reports - on Windows (cmd.exe /c ...) that is the child
// process's own exit code for a plain non-shell-builtin invocation like
// this one, which is all ac3cli ever returns (0 or 1 - see main.cpp).
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    // std::system() on Windows hands this to `cmd.exe /c <command>`; since
    // `command` both contains spaces AND starts with its own quoted
    // executable path, the CRT's own argument quoting backslash-escapes
    // those embedded quotes (\") when it wraps `command` for the /c
    // argument - and cmd.exe does not understand \" as an escaped quote, so
    // the escaped command comes out corrupted ("The filename, directory
    // name, or volume label syntax is incorrect", confirmed by reproducing
    // this outside Catch2 too). Wrapping the whole thing in one more pair of
    // quotes first is the standard workaround: cmd.exe's own "strip a
    // matching outer quote pair" rule then removes exactly this pair,
    // handing cmd the original, uncorrupted command line beneath it. POSIX's
    // `sh -c` has no such rule - the same extra pair there would make `sh`
    // read the entire command (redirections included) as one big quoted
    // word, which is exactly the "not found" this guard avoids.
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// A short, genuinely non-silent multichannel WAV - per this project's own
// testing convention (see memory: "Codec validation needs real audio"),
// silence gives false passes a real tone does not: a silent leading region
// looks identical to a bug that silenced the whole file, but a tone that
// goes missing does not.
std::vector<std::vector<float>> make_tone_channels(std::size_t channels, std::size_t frames,
                                                    std::uint32_t sample_rate) {
    std::vector<std::vector<float>> out(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        const double hz = 220.0 * std::pow(2.0, static_cast<double>(c) * 0.5);
        for (std::size_t n = 0; n < frames; ++n) {
            out[c][n] = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                              static_cast<double>(sample_rate)));
        }
    }
    return out;
}

// RMS over [from, from + count) of one channel - used to tell "silence" from
// "the tone is playing" without demanding exact sample equality, which a
// lossy codec never gives.
double rms(const std::vector<float>& channel, std::size_t from, std::size_t count) {
    double sum_sq = 0.0;
    const std::size_t end = std::min(from + count, channel.size());
    for (std::size_t n = from; n < end; ++n) {
        sum_sq += static_cast<double>(channel[n]) * static_cast<double>(channel[n]);
    }
    const std::size_t n = end > from ? end - from : 1;
    return std::sqrt(sum_sq / static_cast<double>(n));
}

}  // namespace

TEST_CASE("offset= rejects malformed tokens", "[cli][offset]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "offset_parse_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("missing colon") {
        const auto out_path = dir / "offset_parse_colon.ac3";
        const auto log = dir / "offset_parse_colon.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=1.5",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("non-numeric source index") {
        const auto out_path = dir / "offset_parse_index.ac3";
        const auto log = dir / "offset_parse_index.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=x:1.0",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("non-numeric seconds") {
        const auto out_path = dir / "offset_parse_seconds.ac3";
        const auto log = dir / "offset_parse_seconds.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=0:soon",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("negative seconds") {
        const auto out_path = dir / "offset_parse_negative.ac3";
        const auto log = dir / "offset_parse_negative.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=0:-1",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("well-formed offset= parses and runs") {
        const auto out_path = dir / "offset_parse_ok.ac3";
        const auto log = dir / "offset_parse_ok.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=0:0.1",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }
}

TEST_CASE("fast-mdct is default-on with =off as the negation", "[cli][fast-mdct]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "fastmdct_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("fast-mdct=off encodes down the direct path") {
        const auto out_path = dir / "fastmdct_off.ac3";
        const auto log = dir / "fastmdct_off.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo fast-mdct=off",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }

    SECTION("the bare opt-in word from the default-off era still parses") {
        const auto out_path = dir / "fastmdct_bare.ac3";
        const auto log = dir / "fastmdct_bare.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo fast-mdct",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }

    SECTION("any value other than off is refused, not ignored") {
        const auto out_path = dir / "fastmdct_bad.ac3";
        const auto log = dir / "fastmdct_bad.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo fast-mdct=fast",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("fast-mdct") != std::string::npos);
    }

    SECTION("eac3 spells it tools=nofastmdct, and the old fastmdct token is a no-op") {
        const auto off_path = dir / "fastmdct_eac3_off.ec3";
        const auto log = dir / "fastmdct_eac3.log";
        fs::remove(off_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    off_path.string() + "\" 192 nofastmdct stereo",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(off_path));

        const auto legacy_path = dir / "fastmdct_eac3_legacy.ec3";
        fs::remove(legacy_path);
        const auto rc2 = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                     legacy_path.string() + "\" 192 fastmdct stereo",
                                 log);
        CHECK(rc2 == 0);
        CHECK(fs::exists(legacy_path));
    }
}

// capture2= is 'live'-only, but its rejection happens in parse_options,
// before Needs::kCapture is even checked (see run_main: parse_options runs
// on the whole trailing-options span before the per-command needs gate) -
// so a malformed token is refused the same way regardless of which command
// carries it, and 'encode' (Needs::kNothing) lets this run without a real
// capture device, the same way the offset= tests above do.
TEST_CASE("capture2= rejects malformed tokens", "[cli][capture2]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "capture2_parse_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("non-numeric value") {
        const auto out_path = dir / "capture2_parse_nonnumeric.ac3";
        const auto log = dir / "capture2_parse_nonnumeric.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo capture2=x",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("capture2=") != std::string::npos);
    }

    SECTION("negative value") {
        const auto out_path = dir / "capture2_parse_negative.ac3";
        const auto log = dir / "capture2_parse_negative.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo capture2=-1",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("capture2=") != std::string::npos);
    }

    SECTION("empty value") {
        const auto out_path = dir / "capture2_parse_empty.ac3";
        const auto log = dir / "capture2_parse_empty.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo capture2=",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("capture2=") != std::string::npos);
    }
}

TEST_CASE("offset= applies leading silence and grows the programme", "[cli][offset]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "offset_apply_in.wav";
    constexpr std::size_t kFrames = 4000;
    constexpr std::uint32_t kSampleRate = 48000;
    const auto channels = make_tone_channels(2, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // Without offset=: the classic single-file path, untouched by this
    // feature - the programme is exactly as long as the source.
    const auto plain_ac3 = dir / "offset_apply_plain.ac3";
    const auto plain_wav = dir / "offset_apply_plain_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + plain_ac3.string() +
                        "\" 192 stereo",
                    dir / "offset_apply_plain_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + plain_ac3.string() + "\" \"" + plain_wav.string() + "\"",
                    dir / "offset_apply_plain_decode.log") == 0);
    const auto plain_decoded = ac3::io::read_wav(plain_wav.string());
    REQUIRE(plain_decoded.has_value());

    // With offset=0:0.5: 0.5 s (24000 samples at 48 kHz) of leading silence
    // ahead of the source's own audio.
    constexpr double kOffsetSeconds = 0.5;
    constexpr std::size_t kOffsetSamples = static_cast<std::size_t>(kOffsetSeconds * kSampleRate);
    const auto offset_ac3 = dir / "offset_apply_offset.ac3";
    const auto offset_wav = dir / "offset_apply_offset_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + offset_ac3.string() +
                        "\" 192 stereo offset=0:0.5",
                    dir / "offset_apply_offset_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + offset_ac3.string() + "\" \"" + offset_wav.string() + "\"",
                    dir / "offset_apply_offset_decode.log") == 0);
    const auto offset_decoded = ac3::io::read_wav(offset_wav.string());
    REQUIRE(offset_decoded.has_value());

    // The programme is still as long as the longest source once the offset
    // is applied - strictly longer than the same encode without offset=,
    // covering the leading silence PLUS the source's own length, not just
    // the source's own raw length.
    CHECK(offset_decoded->frame_count() > plain_decoded->frame_count());
    CHECK(offset_decoded->frame_count() >= kOffsetSamples + kFrames);

    // Leading kOffsetSamples: silence (a lossy lower bound, not exact zero).
    for (const auto& channel : offset_decoded->channels) {
        CHECK(rms(channel, 0, kOffsetSamples) < 0.02);
    }
    // Just past the offset boundary: the tone is playing again.
    for (const auto& channel : offset_decoded->channels) {
        CHECK(rms(channel, kOffsetSamples + 200, 1000) > 0.1);
    }
}

TEST_CASE("atmos-encode with a keyframes file authors motion", "[cli][atmos-encode]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "atmos_encode_paths_in.wav";
    // Four full 1536-sample frames (128 ms @ 48 kHz) so every frame the
    // per-frame loop visits is a real, unpadded one - six channels, one
    // object per channel, matching atmos-encode's own default addressing
    // (object index == WAV channel index), the same numbering the GUI's
    // exportObjectPaths keys its file by.
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // Object 0 authored to sweep across the room over the clip; every other
    // object index is left unmentioned, so it should keep atmos-encode's own
    // default static placement (see run_atmos_encode's fallback).
    const auto paths_path = dir / "atmos_encode_paths.txt";
    {
        std::ofstream paths{paths_path};
        REQUIRE(paths.is_open());
        paths << "0 0.0  0.9 0.9 0.0  0.6 0.0\n";
        paths << "0 0.09 0.1 0.1 0.0  0.6 0.0\n";
    }

    const auto static_ec3 = dir / "atmos_encode_static.ec3";
    const auto motion_ec3 = dir / "atmos_encode_motion.ec3";

    const auto static_rc =
        run_cli("atmos-encode \"" + wav_path.string() + "\" \"" + static_ec3.string() +
                    "\" 448 6",
                dir / "atmos_encode_static.log");
    const auto motion_rc =
        run_cli("atmos-encode \"" + wav_path.string() + "\" \"" + motion_ec3.string() +
                    "\" 448 6 \"" + paths_path.string() + "\"",
                dir / "atmos_encode_motion.log");

    INFO(read_log(dir / "atmos_encode_static.log"));
    CHECK(static_rc == 0);
    INFO(read_log(dir / "atmos_encode_motion.log"));
    CHECK(motion_rc == 0);
    REQUIRE(fs::exists(static_ec3));
    REQUIRE(fs::exists(motion_ec3));
    CHECK(fs::file_size(static_ec3) > 0);
    CHECK(fs::file_size(motion_ec3) > 0);

    // Authored motion changes the per-frame OAMD/JOC side data the static
    // run reuses unchanged every frame - the two streams must differ.
    std::ifstream static_in{static_ec3, std::ios::binary};
    std::ifstream motion_in{motion_ec3, std::ios::binary};
    const std::vector<char> static_bytes{std::istreambuf_iterator<char>{static_in},
                                         std::istreambuf_iterator<char>{}};
    const std::vector<char> motion_bytes{std::istreambuf_iterator<char>{motion_in},
                                         std::istreambuf_iterator<char>{}};
    // Compared as a bool, not the vectors themselves - CHECK'ing the
    // containers directly asks Catch2 to stringify both (every byte) for a
    // potential diff message, which for multi-KB streams is at best slow and
    // was observed to crash outright.
    const bool differs = static_bytes != motion_bytes;
    CHECK(differs);
}

// keep-partial (item 34): a bare trailing token, same style as heavy/
// mixmeta/sign-objects, that keeps whatever frames a failed encode already
// produced at <name>.partial.<ext> instead of discarding them - see
// write_partial_output in main.cpp. FrameError's own causes (kInvalidBitrate
// and friends - see silent_frame.hpp) are all checked against the fixed
// config (bitrate, tools, channel count, dialnorm...), never per-frame
// audio content, so for a GIVEN invocation either every frame fails
// (nothing to keep - frames stays empty) or none do; there is no reachable
// "some frames succeeded, then a later one failed" case through ac3cli's
// own command line today. These tests cover exactly what IS reachable: the
// token parses and is inert on a run that succeeds, and produces no
// spurious partial file on a run that fails with nothing yet encoded -
// matching EncoderController's own `keep_partial && !frames.empty()` guard
// for the GUI's equivalent preference.
TEST_CASE("keep-partial", "[cli][keep-partial]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "keep_partial_in.wav";
    const auto channels = make_tone_channels(6, 3 * static_cast<std::size_t>(ac3::kSamplesPerFrame),
                                             48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("is inert on a run that succeeds - no spurious partial file") {
        const auto out_path = dir / "keep_partial_ok.ec3";
        const auto partial_path = dir / "keep_partial_ok.partial.ec3";
        const auto log = dir / "keep_partial_ok.log";
        fs::remove(out_path);
        fs::remove(partial_path);

        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 cpl 51 keep-partial",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK_FALSE(fs::exists(partial_path));
    }

    // 64 kbps with every Annex E tool on over a 5.1 bed cannot hold the
    // side information at all, let alone any mantissas - refused on the
    // very first frame, deterministically, regardless of the source
    // material (confirmed empirically: this is a config-level ceiling, not
    // a content-dependent one - see this test's own top comment).
    SECTION("a run that fails before any frame succeeds keeps nothing, "
           "keep-partial or not") {
        const auto out_path = dir / "keep_partial_fail.ec3";
        const auto partial_path = dir / "keep_partial_fail.partial.ec3";
        const auto log = dir / "keep_partial_fail.log";
        fs::remove(out_path);
        fs::remove(partial_path);

        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 64 all 51 keep-partial",
                                log);
        INFO(read_log(log));
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK_FALSE(fs::exists(partial_path));
    }

    SECTION("an unrecognised bare token still fails to parse - keep-partial "
           "is not silently accepting everything") {
        const auto out_path = dir / "keep_partial_typo.ec3";
        const auto log = dir / "keep_partial_typo.log";
        fs::remove(out_path);

        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 cpl 51 keep-partiel",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("bare heavy2 token turns on Ch2 heavy compression on a 1+1 encode",
          "[cli][encode][heavy2]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "heavy2_token_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    // Two channels, one two-channel file: layout 1+1's "Ch1, Ch2 in one file"
    // shape (prepare_dual_mono_source), so no in2.wav positional is needed.
    // AC-3, not E-AC-3: run_decode's classic-AC3 path is the one that reports
    // compr2 presence (run_decode_eac3's dual-mono branch prints nothing about
    // metadata at all), so that is the path this test needs to observe through.
    const auto channels = make_tone_channels(2, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto plain_ac3 = dir / "heavy2_token_plain.ac3";
    const auto plain_wav = dir / "heavy2_token_plain_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + plain_ac3.string() +
                        "\" 192 1+1",
                    dir / "heavy2_token_plain_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + plain_ac3.string() + "\" \"" + plain_wav.string() + "\"",
                    dir / "heavy2_token_plain_decode.log") == 0);
    const auto plain_log = read_log(dir / "heavy2_token_plain_decode.log");
    INFO(plain_log);
    CHECK(plain_log.find("compr2 present") == std::string::npos);

    // Before the fix, a bare 'heavy2' token was not recognised as an option
    // by run_main's is_option classification (only parse_options itself knew
    // about it), so it fell through to encode's args[] instead - landing on
    // the optional in2.wav positional and making the command fail outright
    // (prepare_dual_mono_source rejects a 2-channel first file once a second
    // input path is given). Succeeding at all is therefore already part of
    // what this proves; 'compr2 present' in the decode is the rest.
    const auto heavy2_ac3 = dir / "heavy2_token_heavy2.ac3";
    const auto heavy2_wav = dir / "heavy2_token_heavy2_decoded.wav";
    const auto encode_rc =
        run_cli("encode \"" + wav_path.string() + "\" \"" + heavy2_ac3.string() +
                    "\" 192 1+1 heavy2",
                dir / "heavy2_token_heavy2_encode.log");
    INFO(read_log(dir / "heavy2_token_heavy2_encode.log"));
    REQUIRE(encode_rc == 0);
    const auto decode_rc = run_cli("decode \"" + heavy2_ac3.string() + "\" \"" +
                                       heavy2_wav.string() + "\"",
                                   dir / "heavy2_token_heavy2_decode.log");
    REQUIRE(decode_rc == 0);
    const auto heavy2_log = read_log(dir / "heavy2_token_heavy2_decode.log");
    INFO(heavy2_log);
    CHECK(heavy2_log.find("compr2 present") != std::string::npos);
}
