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

// Runs `ac3cli <args>` with stdin redirected from `in_file` and stdout
// redirected to `out_file`, for exercising the "-" stdin/stdout convention
// (main.cpp's is_stdio_path()) the same way a real shell pipeline would.
// stderr goes to `log`, same diagnostic convention as run_cli above but kept
// separate from stdout, which here is the command's real binary output, not
// somewhere to also stash messages.
int run_cli_stdio(const std::string& args, const fs::path& in_file, const fs::path& out_file,
                  const fs::path& log) {
    const std::string command = "\"" + std::string(AC3CLI_EXE) + "\" " + args + " < \"" +
                                in_file.string() + "\" > \"" + out_file.string() + "\" 2> \"" +
                                log.string() + "\"";
#ifdef _WIN32
    // Same double-quote-wrapping workaround run_cli uses above, and for the
    // same reason - see its comment.
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
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

// A single mono channel at a chosen amplitude/frequency - unlike
// make_tone_channels above (which ladders frequency across the channels of
// ONE file), the dialnorm=auto tests below need independently-controlled,
// separately-loud tracks: two whole files for src=/map=, or the two channels
// of a hand-built dual-mono WAV, with levels deliberately far enough apart
// that a wrong (blended, swapped, or unrouted) measurement reads as a
// distinctly different number rather than a rounding nuance.
std::vector<float> make_tone(double amp, double hz, std::size_t frames, std::uint32_t sample_rate) {
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(
            amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                          static_cast<double>(sample_rate)));
    }
    return out;
}

// write_wav_f32 takes a std::span<const std::vector<float>>, which (unlike
// make_tone_channels' already-a-vector result above) a braced-init-list of
// make_tone(...) calls does not implicitly convert to - this takes the list
// as a real std::vector<std::vector<float>> parameter first so callers below
// can still write the channels inline as a brace list.
bool write_wav(const fs::path& path, std::vector<std::vector<float>> channels,
               std::uint32_t sample_rate) {
    return ac3::io::write_wav_f32(path.string(), channels, sample_rate).has_value();
}

// The integer dialnorm/dialnorm2 value the CLI's own "-> dialnorm N" /
// "-> dialnorm2 N" report line prints, straight out of the log text - reads
// the same reported number an operator would, rather than re-deriving one
// from the logged LKFS float and risking a second place rounding could
// disagree. The trailing space in `needle` matters: it is what keeps a
// "dialnorm" search from also matching inside "dialnorm2 30" (the character
// right after "dialnorm" there is '2', not a space).
std::optional<int> reported_value(const std::string& log, std::string_view field) {
    const std::string needle = std::string("-> ") + std::string(field) + " ";
    const auto pos = log.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return std::stoi(log.substr(pos + needle.size()));
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

// container= is 'record'/'live'-only, but like capture2= above its rejection
// happens in parse_options, before either command's own logic ever runs - so
// 'encode' (Needs::kNothing) can exercise the parsing without a real capture
// device, the same reasoning capture2='s own test comment gives.
TEST_CASE("container= rejects malformed tokens and accepts the two real ones",
          "[cli][container]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "container_parse_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("an unrecognised value is refused, not silently ignored") {
        const auto out_path = dir / "container_parse_bad.ac3";
        const auto log = dir / "container_parse_bad.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo container=avi",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("container") != std::string::npos);
    }

    SECTION("container=mkv parses (encode itself never reads it, so the run still succeeds "
           "and writes the plain elementary stream)") {
        const auto out_path = dir / "container_parse_mkv.ac3";
        const auto log = dir / "container_parse_mkv.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo container=mkv",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }

    SECTION("container=raw parses, spelling out the default") {
        const auto out_path = dir / "container_parse_raw.ac3";
        const auto log = dir / "container_parse_raw.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo container=raw",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
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

// The "-" stdin/stdout convention (roadmap item A4): 'ac3cli encode - -'
// reads the WAV from stdin and writes AC-3 to stdout instead of opening
// files by those literal names, and 'decode - -' the same in reverse - see
// is_stdio_path() in main.cpp. This is also the binary-safety proof
// CONTRIBUTING's validation discipline asks for: on Windows, std::cin/
// std::cout default to TEXT mode, which would either corrupt the compressed
// stream (0x0A -> 0x0D 0x0A) or truncate it early (a stray 0x1A read back as
// EOF) the moment either byte value appears - and in four frames of a real,
// non-silent 5.1 tone at 448 kbps, both values appear many times over. So a
// missing or wrong platform/stdio_binary.hpp call shows up here either as a
// decode failure or as a byte mismatch against the file-based reference,
// not as a subtle level difference.
TEST_CASE("encode/decode round trip through '-' matches the file-based one", "[cli][stdio]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    const auto wav_path = dir / "stdio_roundtrip_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // Reference: the classic, all-file round trip.
    const auto file_ac3 = dir / "stdio_roundtrip_file.ac3";
    const auto file_wav = dir / "stdio_roundtrip_file_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + file_ac3.string() +
                        "\" 448 couple",
                    dir / "stdio_roundtrip_file_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + file_ac3.string() + "\" \"" + file_wav.string() + "\"",
                    dir / "stdio_roundtrip_file_decode.log") == 0);
    const auto file_decoded = ac3::io::read_wav(file_wav.string());
    REQUIRE(file_decoded.has_value());

    // Same round trip, but '-' stands in for both paths at each step: the
    // WAV goes in over stdin and the AC-3 comes out over stdout, then that
    // AC-3 goes back in over stdin and the decoded WAV comes out over
    // stdout.
    const auto stdio_ac3 = dir / "stdio_roundtrip_stdio.ac3";
    const auto stdio_wav = dir / "stdio_roundtrip_stdio_decoded.wav";
    const auto encode_rc = run_cli_stdio("encode - - 448 couple", wav_path, stdio_ac3,
                                         dir / "stdio_roundtrip_encode.log");
    INFO(read_log(dir / "stdio_roundtrip_encode.log"));
    REQUIRE(encode_rc == 0);
    const auto decode_rc =
        run_cli_stdio("decode - -", stdio_ac3, stdio_wav, dir / "stdio_roundtrip_decode.log");
    INFO(read_log(dir / "stdio_roundtrip_decode.log"));
    REQUIRE(decode_rc == 0);

    // The elementary AC-3 stream must be byte-identical either way - "-" is
    // a routing change at the argument-parsing layer, not a different
    // encode path. Compared as a bool first, not the vectors themselves -
    // see the atmos-encode test above for why (Catch2 stringifying a
    // multi-KB mismatch for the diff message is slow and was observed to
    // crash outright).
    std::ifstream file_ac3_in{file_ac3, std::ios::binary};
    std::ifstream stdio_ac3_in{stdio_ac3, std::ios::binary};
    const std::vector<char> file_ac3_bytes{std::istreambuf_iterator<char>{file_ac3_in},
                                           std::istreambuf_iterator<char>{}};
    const std::vector<char> stdio_ac3_bytes{std::istreambuf_iterator<char>{stdio_ac3_in},
                                            std::istreambuf_iterator<char>{}};
    REQUIRE_FALSE(file_ac3_bytes.empty());
    const bool ac3_matches = file_ac3_bytes == stdio_ac3_bytes;
    CHECK(ac3_matches);

    // And the decoded PCM must match too, sample for sample.
    const auto stdio_decoded = ac3::io::read_wav(stdio_wav.string());
    REQUIRE(stdio_decoded.has_value());
    REQUIRE(stdio_decoded->channels.size() == file_decoded->channels.size());
    CHECK(stdio_decoded->sample_rate == file_decoded->sample_rate);
    const bool pcm_matches = stdio_decoded->channels == file_decoded->channels;
    CHECK(pcm_matches);
}

// eac3-encode and atmos-encode share run_encode/run_decode's read_wav_arg/
// write_frames helpers, so "-" reaches them too (see main.cpp's kCommands
// table and this task's own scope note) - covered separately from the
// encode/decode round trip above since each has its own positional argument
// shape (tools/layout for eac3-encode, objects for atmos-encode).
TEST_CASE("eac3-encode and atmos-encode accept '-' for input and output", "[cli][stdio]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 3 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    const auto wav_path = dir / "stdio_eac3_atmos_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto eac3_out = dir / "stdio_eac3_out.ec3";
    const auto eac3_rc = run_cli_stdio("eac3-encode - - 384 cpl 51", wav_path, eac3_out,
                                       dir / "stdio_eac3.log");
    INFO(read_log(dir / "stdio_eac3.log"));
    CHECK(eac3_rc == 0);
    CHECK(fs::file_size(eac3_out) > 0);

    const auto atmos_out = dir / "stdio_atmos_out.ec3";
    const auto atmos_rc = run_cli_stdio("atmos-encode - - 448 6", wav_path, atmos_out,
                                        dir / "stdio_atmos.log");
    INFO(read_log(dir / "stdio_atmos.log"));
    CHECK(atmos_rc == 0);
    CHECK(fs::file_size(atmos_out) > 0);
}

// Roadmap C4: dialnorm=auto/dialnorm2=auto used to be unconditionally
// rejected the moment src=/map= was in play (main.cpp's old "not yet
// supported with src=/map=" error), regardless of whether the routing would
// have made measurement ambiguous. The fix routes/renders the whole
// programme once as a measurement pre-pass - the same BS.1770-4 gated pass
// the single-file path already runs - before the real per-frame encode loop
// renders it again to actually encode it. `loud`/`quiet` are two whole WAV
// files, at clearly different levels, so a bug that measured the wrong
// source (or blended both) reads as a clearly wrong number rather than a
// coincidental match.
TEST_CASE("dialnorm=auto/dialnorm2=auto measure the routed programme with src=/map=",
          "[cli][dialnorm][src]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;  // 2s: several full 400ms BS.1770 gate windows

    const auto loud = dir / "dialnorm_src_loud.wav";
    const auto quiet = dir / "dialnorm_src_quiet.wav";
    REQUIRE(write_wav(loud, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
    REQUIRE(write_wav(quiet, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));

    // Solo measurements: each source alone, through a plain mono target, is
    // the ground truth every assertion below compares against.
    const auto loud_solo_log = dir / "dialnorm_loud_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + loud.string() + "\" \"" +
                        (dir / "loud_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    loud_solo_log) == 0);
    const auto loud_solo = reported_value(read_log(loud_solo_log), "dialnorm");
    REQUIRE(loud_solo.has_value());

    const auto quiet_solo_log = dir / "dialnorm_quiet_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + quiet.string() + "\" \"" +
                        (dir / "quiet_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    quiet_solo_log) == 0);
    const auto quiet_solo = reported_value(read_log(quiet_solo_log), "dialnorm");
    REQUIRE(quiet_solo.has_value());

    // The two solo levels have to actually differ, or a blending bug and a
    // correct per-source measurement could print the same number by
    // accident and this test would prove nothing.
    REQUIRE(*loud_solo != *quiet_solo);

    SECTION("stereo target: matches an equivalent single-file measurement exactly") {
        // Independent 2-channel equivalent of the src=/map= run below (same
        // two tones, already in coded-channel order) - the strongest
        // cross-check available: a straight L/R map= carries bit-identical
        // audio, so the routed measurement must match this file's own
        // single-file measurement exactly, not merely "some number".
        const auto equiv = dir / "dialnorm_equiv_stereo.wav";
        REQUIRE(write_wav(equiv,
                          {make_tone(0.9, 220.0, kFrames, kRate),
                           make_tone(0.5, 660.0, kFrames, kRate)},
                          kRate));
        const auto equiv_log = dir / "dialnorm_equiv_stereo.log";
        REQUIRE(run_cli("eac3-encode \"" + equiv.string() + "\" \"" +
                            (dir / "equiv_stereo.ec3").string() +
                            "\" 192 none stereo dialnorm=auto",
                        equiv_log) == 0);
        const auto equiv_measured = reported_value(read_log(equiv_log), "dialnorm");
        REQUIRE(equiv_measured.has_value());

        const auto out = dir / "dialnorm_stereo.ec3";
        const auto log = dir / "dialnorm_stereo.log";
        const auto rc = run_cli("eac3-encode \"" + loud.string() + "\" \"" + out.string() +
                                    "\" 192 none stereo src=\"" + quiet.string() +
                                    "\" map=0.0:L,1.0:R dialnorm=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        // Was unconditionally rejected before this fix.
        CHECK(text.find("not yet supported") == std::string::npos);
        const auto measured = reported_value(text, "dialnorm");
        REQUIRE(measured.has_value());
        CHECK(*measured == *equiv_measured);
    }

    SECTION("map= trim is measured on the rendered programme, not the raw source levels") {
        const auto trimmed_out = dir / "dialnorm_multi_trimmed.ec3";
        const auto trimmed_log = dir / "dialnorm_multi_trimmed.log";
        REQUIRE(run_cli("eac3-encode \"" + loud.string() + "\" \"" + trimmed_out.string() +
                            "\" 192 none stereo src=\"" + quiet.string() +
                            "\" map=0.0:L@-12,1.0:R dialnorm=auto",
                        trimmed_log) == 0);
        const auto trimmed = reported_value(read_log(trimmed_log), "dialnorm");
        REQUIRE(trimmed.has_value());

        const auto untrimmed_out = dir / "dialnorm_multi_untrimmed.ec3";
        const auto untrimmed_log = dir / "dialnorm_multi_untrimmed.log";
        REQUIRE(run_cli("eac3-encode \"" + loud.string() + "\" \"" + untrimmed_out.string() +
                            "\" 192 none stereo src=\"" + quiet.string() +
                            "\" map=0.0:L,1.0:R dialnorm=auto",
                        untrimmed_log) == 0);
        const auto untrimmed = reported_value(read_log(untrimmed_log), "dialnorm");
        REQUIRE(untrimmed.has_value());

        // Attenuating the dominant (loud) source by -12 dB before measuring
        // must measurably quieten the programme (a bigger dialnorm number).
        // If this measured the raw, unrouted source files instead of the
        // rendered/trimmed ones, the trim would have no effect at all.
        CHECK(*trimmed > *untrimmed);
    }

    SECTION("1+1 target via src=/map=: each source's channel is measured on its own") {
        const auto out = dir / "dialnorm_dualmono_multi.ec3";
        const auto log = dir / "dialnorm_dualmono_multi.log";
        const auto rc = run_cli("eac3-encode \"" + loud.string() + "\" \"" + out.string() +
                                    "\" 192 none 1+1 src=\"" + quiet.string() +
                                    "\" map=0.0:p1,1.0:p2 dialnorm=auto dialnorm2=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        const auto ch1 = reported_value(text, "dialnorm");
        const auto ch2 = reported_value(text, "dialnorm2");
        REQUIRE(ch1.has_value());
        REQUIRE(ch2.has_value());
        // p1 came from `loud` alone, p2 from `quiet` alone - a correct,
        // per-programme measurement matches each source's own solo number
        // exactly; a blended or swapped measurement would not.
        CHECK(*ch1 == *loud_solo);
        CHECK(*ch2 == *quiet_solo);
    }
}

// Same fix, plain AC-3 (run_encode_multi rather than run_eac3_encode_multi) -
// a separate function in main.cpp with its own copy of the measurement
// pre-pass, so it needs its own proof it was actually fixed too.
TEST_CASE("dialnorm=auto works with src=/map= on the plain AC-3 encode path too",
          "[cli][dialnorm][src]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;
    const auto loud = dir / "dialnorm_ac3_loud.wav";
    const auto quiet = dir / "dialnorm_ac3_quiet.wav";
    REQUIRE(write_wav(loud, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
    REQUIRE(write_wav(quiet, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));

    const auto out = dir / "dialnorm_ac3_multi.ac3";
    const auto log = dir / "dialnorm_ac3_multi.log";
    const auto rc = run_cli("encode \"" + loud.string() + "\" \"" + out.string() +
                                "\" 192 stereo src=\"" + quiet.string() +
                                "\" map=0.0:L,1.0:R dialnorm=auto",
                            log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == 0);
    CHECK(fs::exists(out));
    CHECK(text.find("not yet supported") == std::string::npos);
    CHECK(reported_value(text, "dialnorm").has_value());
}

// Roadmap C4's other half: dual mono (1+1) dialnorm=auto looked implemented
// already (measured_dialnorm_channel existed for Ch2), but Ch1's own
// measurement went through measured_dialnorm() with the target's acmod
// (kDualMono) instead - which runs a normal multi-channel BS.1770 pass
// across BOTH wav channels at once, silently reporting the combined loudness
// of Ch1+Ch2 as if they were a coherent stereo pair, rather than Ch1's own
// channel alone (§E1.3: the two programmes are unrelated and share no
// downmix). Ch1 and Ch2 are given comparable levels here specifically so
// that bug - a summed-power measurement roughly 3 dB louder than Ch1 alone -
// would read as a clearly different, clearly wrong dialnorm rather than a
// rounding nuance.
TEST_CASE("dialnorm=auto for 1+1 dual mono measures each programme's own channel, "
          "single-file path",
          "[cli][dialnorm][dual-mono]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;

    const auto ch1_tone = make_tone(0.5, 220.0, kFrames, kRate);
    const auto ch2_tone = make_tone(0.5, 660.0, kFrames, kRate);

    const auto ch1_solo_path = dir / "dm_ch1_solo.wav";
    REQUIRE(write_wav(ch1_solo_path, {ch1_tone}, kRate));
    const auto ch1_log = dir / "dm_ch1_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + ch1_solo_path.string() + "\" \"" +
                        (dir / "dm_ch1_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    ch1_log) == 0);
    const auto ch1_solo = reported_value(read_log(ch1_log), "dialnorm");
    REQUIRE(ch1_solo.has_value());

    const auto ch2_solo_path = dir / "dm_ch2_solo.wav";
    REQUIRE(write_wav(ch2_solo_path, {ch2_tone}, kRate));
    const auto ch2_log = dir / "dm_ch2_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + ch2_solo_path.string() + "\" \"" +
                        (dir / "dm_ch2_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    ch2_log) == 0);
    const auto ch2_solo = reported_value(read_log(ch2_log), "dialnorm");
    REQUIRE(ch2_solo.has_value());

    const auto dualmono_path = dir / "dm_dualmono.wav";
    REQUIRE(write_wav(dualmono_path, {ch1_tone, ch2_tone}, kRate));

    SECTION("eac3-encode") {
        const auto out = dir / "dm_eac3.ec3";
        const auto log = dir / "dm_eac3.log";
        const auto rc = run_cli("eac3-encode \"" + dualmono_path.string() + "\" \"" +
                                    out.string() +
                                    "\" 192 none 1+1 dialnorm=auto dialnorm2=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        const auto ch1 = reported_value(text, "dialnorm");
        const auto ch2 = reported_value(text, "dialnorm2");
        REQUIRE(ch1.has_value());
        REQUIRE(ch2.has_value());
        CHECK(*ch1 == *ch1_solo);
        CHECK(*ch2 == *ch2_solo);
    }

    SECTION("encode (plain AC-3)") {
        const auto out = dir / "dm_ac3.ac3";
        const auto log = dir / "dm_ac3.log";
        const auto rc = run_cli("encode \"" + dualmono_path.string() + "\" \"" + out.string() +
                                    "\" 192 1+1 dialnorm=auto dialnorm2=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        const auto ch1 = reported_value(text, "dialnorm");
        const auto ch2 = reported_value(text, "dialnorm2");
        REQUIRE(ch1.has_value());
        REQUIRE(ch2.has_value());
        CHECK(*ch1 == *ch1_solo);
        CHECK(*ch2 == *ch2_solo);
    }
}
