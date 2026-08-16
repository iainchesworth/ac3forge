#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
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

// Same idea as run_cli_stdio above, but only out_path is "-" - for src=/map=
// commands, which take their input from a real in_path plus src="<file>"
// (there is no second stdin to route a multi-source run's extra files
// through), so only stdout needs redirecting away from the shell's own
// inherited one. stderr still goes to its own `log`, kept separate from
// `out_file` for the same reason run_cli_stdio's does.
int run_cli_stdout(const std::string& args, const fs::path& out_file, const fs::path& log) {
    const std::string command = "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" +
                                out_file.string() + "\" 2> \"" + log.string() + "\"";
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

// Finds `label` in `log` and parses the (possibly signed, possibly
// fractional) number immediately following it, skipping whitespace in
// between - `ac3cli qc`'s own report lines are "label<spaces>value...", laid
// out with std::format field widths this deliberately does not need to know:
// skipping runs of whitespace instead of a fixed offset means a column-width
// tweak in main.cpp can never silently break these tests the way a
// fixed-offset substring slice would.
std::optional<double> value_after(const std::string& log, std::string_view label) {
    const auto pos = log.find(label);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t i = pos + label.size();
    while (i < log.size() && std::isspace(static_cast<unsigned char>(log[i])) != 0) {
        ++i;
    }
    // std::from_chars for floating point does not accept a leading '+' (only
    // '-' is part of its grammar) - qc's own report uses {:+.2f} throughout
    // so every positive number is printed with one, and it has to be
    // skipped here rather than included in the range handed to from_chars
    // below, unlike '-' which from_chars parses directly.
    if (i < log.size() && log[i] == '+') {
        ++i;
    }
    const std::size_t start = i;
    if (i < log.size() && log[i] == '-') {
        ++i;
    }
    while (i < log.size() &&
          (std::isdigit(static_cast<unsigned char>(log[i])) != 0 || log[i] == '.')) {
        ++i;
    }
    if (i == start) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(log.data() + static_cast<std::ptrdiff_t>(start),
                                           log.data() + static_cast<std::ptrdiff_t>(i), value);
    (void)ptr;
    return ec == std::errc{} ? std::optional<double>(value) : std::nullopt;
}

// The PASS/FAIL of the first "verdict: " line at or after `from` - qc's own
// per-preset overall verdict (as opposed to its two sub-verdicts, loudness
// and true peak, which this deliberately does not match on).
std::optional<bool> gate_verdict_after(const std::string& log, std::size_t from) {
    if (from == std::string::npos) {
        return std::nullopt;
    }
    constexpr std::string_view kNeedle = "verdict: ";
    const auto pos = log.find(kNeedle, from);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const auto value_pos = pos + kNeedle.size();
    if (log.compare(value_pos, 4, "PASS") == 0) {
        return true;
    }
    if (log.compare(value_pos, 4, "FAIL") == 0) {
        return false;
    }
    return std::nullopt;
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

    SECTION("eac3-encode also honors fast-mdct=off directly, not just tools=nofastmdct") {
        const auto out_path = dir / "fastmdct_eac3_direct_off.ec3";
        const auto log = dir / "fastmdct_eac3_direct.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 192 none stereo fast-mdct=off",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        // format_tools() only ever emits "nofastmdct" (never a positive "fastmdct"
        // token), so its presence here is proof fast-mdct=off actually reached
        // Tools::fast_mdct rather than merely parsing without effect.
        CHECK(text.find("nofastmdct") != std::string::npos);
    }

    SECTION("eac3-encode-multi (src=/map=) also honors fast-mdct=off directly") {
        const auto out_path = dir / "fastmdct_eac3_multi_off.ec3";
        const auto log = dir / "fastmdct_eac3_multi.log";
        fs::remove(out_path);
        // wav_path is 2-channel; parse_assignment requires a token for every
        // channel of every source, so the reused second copy needs its own
        // (unused) channels explicitly marked none rather than left absent.
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() +
                                    "\" 192 none stereo src=\"" + wav_path.string() +
                                    "\" map=0.0:L,0.1:R,1.0:none,1.1:none fast-mdct=off",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK(text.find("nofastmdct") != std::string::npos);
    }

    SECTION("the [tools] positional still wins when fast-mdct=off is also given") {
        const auto out_path = dir / "fastmdct_eac3_precedence.ec3";
        const auto log = dir / "fastmdct_eac3_precedence.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 192 fastmdct stereo fast-mdct=off",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        // meta.fast_mdct=false (fast-mdct=off) seeds p.tools.fast_mdct, but
        // parse_tools() runs after and overwrites the field on a literal
        // fastmdct/nofastmdct token - so the explicit tools token wins.
        CHECK(text.find("nofastmdct") == std::string::npos);
    }

    SECTION("eac3-sine has no [tools] argument, but honors fast-mdct=off directly") {
        const auto out_path = dir / "fastmdct_eac3_sine_off.ec3";
        const auto log = dir / "fastmdct_eac3_sine.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-sine \"" + out_path.string() + "\" 1 192 1000 50 stereo "
                                    "fast-mdct=off",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
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

// sign-objects/signing-key= used to be wired into 'atmos' only - 'atmos-path'
// and 'atmos-encode' accepted both flags (parse_options does not know which
// command it is parsing for) and silently ignored them. All three now call
// apply_object_signing, so all three should report a signed frame count.
// decode_signing_key() falls back to raw bytes for anything that is not
// valid base64 (see signing_key.hpp), so any non-empty file is a usable key
// here - the signature's own correctness is test_signing.cpp's concern, not
// this integration test's.
TEST_CASE("sign-objects reaches atmos, atmos-path and atmos-encode alike",
          "[cli][atmos][signing]") {
    const auto dir = scratch_dir();
    const auto key_path = dir / "sign_objects_test.key";
    {
        std::ofstream key{key_path, std::ios::binary};
        REQUIRE(key.is_open());
        key << "not-a-real-key-just-test-material";
    }
    const std::string signing_args =
        " sign-objects signing-key=\"" + key_path.string() + "\"";

    SECTION("atmos") {
        const auto out_path = dir / "sign_objects_atmos.ec3";
        const auto log = dir / "sign_objects_atmos.log";
        const auto rc =
            run_cli("atmos \"" + out_path.string() + "\" 1 448 2 4 objects" + signing_args, log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("signed") != std::string::npos);
    }

    SECTION("atmos-path") {
        const auto paths_path = dir / "sign_objects_paths.txt";
        {
            std::ofstream paths{paths_path};
            REQUIRE(paths.is_open());
            paths << "0 0.0 0.5 0.5 0.0 0.6 0.0\n";
        }
        const auto out_path = dir / "sign_objects_atmos_path.ec3";
        const auto log = dir / "sign_objects_atmos_path.log";
        const auto rc = run_cli("atmos-path \"" + out_path.string() + "\" \"" +
                                    paths_path.string() + "\" 1 448 1" + signing_args,
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("signed") != std::string::npos);
    }

    SECTION("atmos-encode") {
        const auto wav_path = dir / "sign_objects_in.wav";
        const auto channels = make_tone_channels(2, 4000, 48000);
        REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());
        const auto out_path = dir / "sign_objects_atmos_encode.ec3";
        const auto log = dir / "sign_objects_atmos_encode.log";
        const auto rc = run_cli("atmos-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 2" + signing_args,
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("signed") != std::string::npos);
    }

    SECTION("atmos-path without sign-objects stays unsigned, same as before") {
        // Deliberately not named "*unsigned*": that spelling contains
        // "signed" as a substring, which the assertion below would then
        // find in the echoed file path rather than in any real message.
        const auto paths_path = dir / "sign_objects_paths_plain.txt";
        {
            std::ofstream paths{paths_path};
            REQUIRE(paths.is_open());
            paths << "0 0.0 0.5 0.5 0.0 0.6 0.0\n";
        }
        const auto out_path = dir / "sign_objects_atmos_path_plain.ec3";
        const auto log = dir / "sign_objects_atmos_path_plain.log";
        const auto rc = run_cli("atmos-path \"" + out_path.string() + "\" \"" +
                                    paths_path.string() + "\" 1 448 1",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK(text.find("signed") == std::string::npos);
    }
}

// verify-objects mirrors sign-objects' UX on the read side: decode/monitor
// check each frame's EMDF object signature against signing-key= (same option
// sign-objects already uses) instead of just decoding blind. This is a
// deliberate opt-in design (see docs/concepts/object-signing.md and
// Eac3Decoder's own stance that the protection field is opaque per spec) -
// the last SECTION here is the one that matters most: decoding the very same
// signed stream with no verify-objects token at all must succeed exactly as
// it always has, proving the bypass is real and not just documented.
TEST_CASE("verify-objects checks a decode against the signer's own tag",
          "[cli][atmos][signing][verify]") {
    const auto dir = scratch_dir();
    const auto key_path = dir / "verify_objects_test.key";
    const auto wrong_key_path = dir / "verify_objects_wrong.key";
    {
        std::ofstream key{key_path, std::ios::binary};
        REQUIRE(key.is_open());
        key << "not-a-real-key-just-test-material";
    }
    {
        std::ofstream key{wrong_key_path, std::ios::binary};
        REQUIRE(key.is_open());
        key << "a-completely-different-key-for-mismatch";
    }

    const auto signed_ec3 = dir / "verify_objects_signed.ec3";
    const auto signed_log = dir / "verify_objects_signed.log";
    const auto sign_rc =
        run_cli("atmos \"" + signed_ec3.string() +
                    "\" 1 448 2 4 objects sign-objects signing-key=\"" + key_path.string() + "\"",
                signed_log);
    REQUIRE(sign_rc == 0);
    REQUIRE(fs::exists(signed_ec3));

    SECTION("the same key verifies every frame and the decode succeeds") {
        const auto out_wav = dir / "verify_objects_ok.wav";
        const auto log = dir / "verify_objects_ok.log";
        const auto rc = run_cli("decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() +
                                    "\" verify-objects signing-key=\"" + key_path.string() + "\"",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_wav));
        CHECK(text.find("valid") != std::string::npos);
        CHECK(text.find("0 mismatched") != std::string::npos);
    }

    SECTION("a wrong key mismatches and the command refuses") {
        const auto out_wav = dir / "verify_objects_wrong_key.wav";
        const auto log = dir / "verify_objects_wrong_key.log";
        const auto rc = run_cli("decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() +
                                    "\" verify-objects signing-key=\"" +
                                    wrong_key_path.string() + "\"",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK(text.find("mismatch") != std::string::npos);
    }

    SECTION("verify-objects with no key anywhere is a hard error, same shape as sign-objects") {
        const auto out_wav = dir / "verify_objects_no_key.wav";
        const auto log = dir / "verify_objects_no_key.log";
        const auto rc = run_cli(
            "decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() + "\" verify-objects",
            log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK(text.find("needs a key") != std::string::npos);
    }

    SECTION("decoding the same signed stream WITHOUT verify-objects still succeeds - the "
           "bypass") {
        const auto out_wav = dir / "verify_objects_bypass.wav";
        const auto log = dir / "verify_objects_bypass.log";
        const auto rc =
            run_cli("decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() + "\"", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_wav));
    }
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

// A CLI-docs audit ahead of v0.6.0-beta.1 found that "everything but the
// encoded stream goes to stderr, so a '-' pipe is never corrupted" was false
// in two places, neither caught by the round-trip test above because it
// never turns dialnorm=auto or src=/map= on: finish_measurement() (behind
// every dialnorm=auto/dialnorm2=auto path) printed its "measured N LKFS ->
// dialnorm M" line with the no-stream std::println overload, which always
// targets stdout regardless of out_path; and run_eac3_encode_multi/
// run_encode_multi's own final summary/routing report used that same
// unconditional stdout instead of threading status_stream(out_path) through
// the way their single-source run_eac3_encode/run_encode siblings already
// did. Both are silent with a real file out_path - the leaked text just
// lands in the terminal beside the file - so this only shows up once
// out_path is "-" and that leaked text starts sharing stdout with the
// encoded stream itself, which is exactly what each SECTION below pipes
// through. The assertion is the same byte-for-byte comparison the round-trip
// test above uses: any leaked line ahead of or after the stream makes the
// piped bytes longer than (and different from) the clean file-based
// reference, not merely "looks corrupted".
TEST_CASE("dialnorm=auto and src=/map= keep '-' output free of interleaved status text",
          "[cli][stdio][dialnorm][src]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;  // 2s, same duration the src=/map= dialnorm
                                            // tests below use - long enough to clear the
                                            // BS.1770 absolute gate reliably.

    SECTION("dialnorm=auto alone: piped eac3-encode matches the file-based one") {
        const auto wav_path = dir / "stdio_dialnorm_in.wav";
        REQUIRE(write_wav(wav_path, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));

        const auto file_out = dir / "stdio_dialnorm_file.ec3";
        REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + file_out.string() +
                            "\" 96 none mono dialnorm=auto",
                        dir / "stdio_dialnorm_file.log") == 0);

        const auto piped_out = dir / "stdio_dialnorm_piped.ec3";
        const auto piped_log = dir / "stdio_dialnorm_piped.log";
        const auto rc = run_cli_stdio("eac3-encode - - 96 none mono dialnorm=auto", wav_path,
                                      piped_out, piped_log);
        INFO(read_log(piped_log));
        REQUIRE(rc == 0);

        std::ifstream file_in{file_out, std::ios::binary};
        std::ifstream piped_in{piped_out, std::ios::binary};
        const std::vector<char> file_bytes{std::istreambuf_iterator<char>{file_in},
                                           std::istreambuf_iterator<char>{}};
        const std::vector<char> piped_bytes{std::istreambuf_iterator<char>{piped_in},
                                            std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(file_bytes.empty());
        const bool matches = file_bytes == piped_bytes;
        CHECK(matches);
    }

    SECTION("src=/map=: piped E-AC-3 multi-source output matches the file-based one") {
        const auto in1 = dir / "stdio_src_eac3_a.wav";
        const auto in2 = dir / "stdio_src_eac3_b.wav";
        REQUIRE(write_wav(in1, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
        REQUIRE(write_wav(in2, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));
        // Same option combination as one command, so the only thing that
        // differs between the file-based reference and the piped run below
        // is out_path itself ("<file>" vs the bare - token).
        const std::string args_tail =
            " 192 none stereo src=\"" + in2.string() + "\" map=0.0:L,1.0:R dialnorm=auto";

        const auto file_out = dir / "stdio_src_eac3_file.ec3";
        REQUIRE(run_cli("eac3-encode \"" + in1.string() + "\" \"" + file_out.string() + "\"" +
                            args_tail,
                        dir / "stdio_src_eac3_file.log") == 0);

        const auto piped_out = dir / "stdio_src_eac3_piped.ec3";
        const auto piped_log = dir / "stdio_src_eac3_piped.log";
        const auto rc = run_cli_stdout("eac3-encode \"" + in1.string() + "\" -" + args_tail,
                                       piped_out, piped_log);
        INFO(read_log(piped_log));
        REQUIRE(rc == 0);

        std::ifstream file_in{file_out, std::ios::binary};
        std::ifstream piped_in{piped_out, std::ios::binary};
        const std::vector<char> file_bytes{std::istreambuf_iterator<char>{file_in},
                                           std::istreambuf_iterator<char>{}};
        const std::vector<char> piped_bytes{std::istreambuf_iterator<char>{piped_in},
                                            std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(file_bytes.empty());
        const bool matches = file_bytes == piped_bytes;
        CHECK(matches);
    }

    SECTION("src=/map=: piped AC-3 multi-source output matches the file-based one") {
        const auto in1 = dir / "stdio_src_ac3_a.wav";
        const auto in2 = dir / "stdio_src_ac3_b.wav";
        REQUIRE(write_wav(in1, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
        REQUIRE(write_wav(in2, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));
        // run_encode_multi's own copy of the same summary/routing report bug
        // - a separate function from run_eac3_encode_multi (the previous
        // SECTION), so it needs its own proof it was fixed too.
        const std::string args_tail =
            " 192 stereo src=\"" + in2.string() + "\" map=0.0:L,1.0:R dialnorm=auto";

        const auto file_out = dir / "stdio_src_ac3_file.ac3";
        REQUIRE(run_cli("encode \"" + in1.string() + "\" \"" + file_out.string() + "\"" +
                            args_tail,
                        dir / "stdio_src_ac3_file.log") == 0);

        const auto piped_out = dir / "stdio_src_ac3_piped.ac3";
        const auto piped_log = dir / "stdio_src_ac3_piped.log";
        const auto rc = run_cli_stdout("encode \"" + in1.string() + "\" -" + args_tail,
                                       piped_out, piped_log);
        INFO(read_log(piped_log));
        REQUIRE(rc == 0);

        std::ifstream file_in{file_out, std::ios::binary};
        std::ifstream piped_in{piped_out, std::ios::binary};
        const std::vector<char> file_bytes{std::istreambuf_iterator<char>{file_in},
                                           std::istreambuf_iterator<char>{}};
        const std::vector<char> piped_bytes{std::istreambuf_iterator<char>{piped_in},
                                            std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(file_bytes.empty());
        const bool matches = file_bytes == piped_bytes;
        CHECK(matches);
    }
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

// route() only ever carries kLocation content into the routing it returns
// (see assignment.hpp's own comment on kObject/kObjectMono/kProgramme* rows
// contributing nothing) - map= itself happily parses a row aimed at obj (the
// destination is legal syntax, see kAssignmentSyntax), but this CLI has no
// object-assembly path of its own to catch what route() drops; that is the
// GUI's (encoder_controller.cpp's encodeObjects). Before this fix, such a
// channel's audio just vanished with no diagnostic at all; now
// routing_for_sources() warns about each row route() cannot carry, naming
// the source/channel and the destination that ate it.
TEST_CASE("map= to an object destination warns instead of silently discarding it",
          "[cli][src][obj]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);

    const auto primary = dir / "obj_warn_primary.wav";
    const auto extra = dir / "obj_warn_extra.wav";
    REQUIRE(write_wav(primary, {make_tone(0.5, 220.0, kFrames, kRate)}, kRate));
    REQUIRE(write_wav(extra, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));

    SECTION("channel 1.0 mapped to obj: warns, names the row, still encodes") {
        const auto out = dir / "obj_warn.ec3";
        const auto log = dir / "obj_warn.log";
        const auto rc = run_cli("eac3-encode \"" + primary.string() + "\" \"" + out.string() +
                                    "\" 192 none stereo src=\"" + extra.string() +
                                    "\" map=0.0:L,1.0:obj",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        // Named: which row (source 1, channel 0) and what it resolved to.
        CHECK(text.find("1.0") != std::string::npos);
        CHECK(text.find("obj") != std::string::npos);
        CHECK(text.find("warning") != std::string::npos);
    }

    SECTION("a location-only map= stays exactly as quiet as before") {
        const auto out = dir / "obj_warn_quiet.ec3";
        const auto log = dir / "obj_warn_quiet.log";
        const auto rc = run_cli("eac3-encode \"" + primary.string() + "\" \"" + out.string() +
                                    "\" 192 none stereo src=\"" + extra.string() +
                                    "\" map=0.0:L,1.0:R",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("warning") == std::string::npos);
    }
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

// Roadmap C2: `ac3cli qc` - decode a stream, measure it with the real
// BS.1770-4 meter, and compare against the embedded dialnorm/compr and,
// optionally, a named delivery-spec gate. See main.cpp's run_qc/
// report_qc_programme and ac3/meta/qc.hpp for the implementation these tests
// exercise through the real, built binary (this file's own top comment on
// why: main.cpp has no library surface to link against directly).

TEST_CASE("qc preset= rejects an unknown name and accepts every real one", "[cli][qc]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_preset_parse_in.wav";
    const auto channels =
        make_tone_channels(6, 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame), 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());
    const auto ac3_path = dir / "qc_preset_parse.ac3";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + ac3_path.string() + "\" 192 51",
                    dir / "qc_preset_parse_encode.log") == 0);

    SECTION("an unrecognised value is refused, not silently ignored") {
        const auto log = dir / "qc_preset_bad.log";
        const auto rc = run_cli("qc \"" + ac3_path.string() + "\" preset=bogus", log);
        CHECK(rc != 0);
        CHECK(read_log(log).find("preset") != std::string::npos);
    }

    SECTION("preset=ebu-r128-s2 parses and runs a gate check") {
        const auto log = dir / "qc_preset_ebu.log";
        const auto rc = run_cli("qc \"" + ac3_path.string() + "\" preset=ebu-r128-s2", log);
        const auto text = read_log(log);
        INFO(text);
        (void)rc;  // reflects the gate verdict here, not a parse failure - see below instead
        CHECK(text.find("unknown qc preset") == std::string::npos);
        CHECK(text.find("ebu-r128-s2:") != std::string::npos);
        CHECK(text.find("gates:") != std::string::npos);
    }

    SECTION("preset=atsc-a85 parses and runs a gate check") {
        const auto log = dir / "qc_preset_atsc.log";
        run_cli("qc \"" + ac3_path.string() + "\" preset=atsc-a85", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("unknown qc preset") == std::string::npos);
        CHECK(text.find("atsc-a85:") != std::string::npos);
    }

    SECTION("preset=netflix parses and runs a gate check") {
        const auto log = dir / "qc_preset_netflix.log";
        run_cli("qc \"" + ac3_path.string() + "\" preset=netflix", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("unknown qc preset") == std::string::npos);
        CHECK(text.find("netflix:") != std::string::npos);
    }

    SECTION("preset=all checks every preset") {
        const auto log = dir / "qc_preset_all.log";
        run_cli("qc \"" + ac3_path.string() + "\" preset=all", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("ebu-r128-s2:") != std::string::npos);
        CHECK(text.find("atsc-a85:") != std::string::npos);
        CHECK(text.find("netflix:") != std::string::npos);
    }

    SECTION("no preset at all just measures - no 'gates:' section, exit 0") {
        const auto log = dir / "qc_preset_none.log";
        const auto rc = run_cli("qc \"" + ac3_path.string() + "\"", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("gates:") == std::string::npos);
    }
}

TEST_CASE("qc reports the embedded dialnorm and a sane, self-consistent measured loudness",
          "[cli][qc]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_measure_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    // 96000 samples = 2 s: BS.1770's absolute gate needs a full 400 ms
    // block before integrated_lkfs() reports anything at all (LoudnessMeter's
    // own doc comment) - a handful of AC-3 frames is not enough, the same
    // reason the dialnorm=auto tests above all use this same duration.
    constexpr std::size_t kFrames = 96000;
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto ac3_path = dir / "qc_measure.ac3";
    constexpr int kDialnorm = 17;
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + ac3_path.string() +
                        "\" 256 51 dialnorm=" + std::to_string(kDialnorm),
                    dir / "qc_measure_encode.log") == 0);

    const auto log = dir / "qc_measure.log";
    const auto rc = run_cli("qc \"" + ac3_path.string() + "\"", log);
    const auto text = read_log(log);
    INFO(text);
    // Measure-only (no preset=) never gates - a successful decode always
    // exits 0, whatever the numbers turn out to be.
    CHECK(rc == 0);

    const auto dialnorm = value_after(text, "dialnorm");
    REQUIRE(dialnorm.has_value());
    CHECK(*dialnorm == Catch::Approx(kDialnorm));

    const auto measured = value_after(text, "integrated loudness");
    REQUIRE(measured.has_value());
    // Real, non-silent, non-clipping multichannel content: comfortably
    // inside BS.1770's legal range, nowhere near the -70 LKFS "no
    // meaningful loudness" floor and never above digital 0 dBFS.
    CHECK(*measured > -70.0);
    CHECK(*measured < 0.0);

    const auto claimed = value_after(text, "claimed");
    REQUIRE(claimed.has_value());
    CHECK(*claimed == Catch::Approx(-static_cast<double>(kDialnorm)));

    const auto delta = value_after(text, "delta");
    REQUIRE(delta.has_value());
    // Self-consistency: the printed delta must be exactly measured -
    // claimed - the same arithmetic report_qc_programme performs internally
    // to produce it, and exactly what a sign or operand-order bug there
    // would break.
    CHECK(*delta == Catch::Approx(*measured - *claimed).margin(0.01));
}

// Roadmap C2's own explicit ask: a case where the embedded dialnorm and the
// measured loudness deliberately disagree, so the delta-reporting path is
// genuinely exercised rather than merely the agreement case above. dialnorm=1
// claims the loudest legal dialogue level (-1 LKFS) against a deliberately
// quiet (0.05 amplitude, well under make_tone_channels' own 0.5) real 5.1 mix
// - measured loudness cannot plausibly reach anywhere near -1 LKFS for
// content this quiet, so the mismatch is large and its sign (measured is
// quieter than claimed) is guaranteed by construction, not a coincidence of
// whatever the codec happened to produce.
TEST_CASE("qc's dialnorm-vs-measured delta is genuinely exercised when they deliberately disagree",
          "[cli][qc][dialnorm]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_mismatch_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    // 96000 samples = 2 s: BS.1770's absolute gate needs a full 400 ms
    // block before integrated_lkfs() reports anything at all (LoudnessMeter's
    // own doc comment) - a handful of AC-3 frames is not enough, the same
    // reason the dialnorm=auto tests above all use this same duration.
    constexpr std::size_t kFrames = 96000;
    std::vector<std::vector<float>> quiet_channels;
    quiet_channels.reserve(6);
    for (int ch = 0; ch < 6; ++ch) {
        quiet_channels.push_back(
            make_tone(0.05, 200.0 + 137.0 * static_cast<double>(ch), kFrames, kSampleRate));
    }
    REQUIRE(write_wav(wav_path, quiet_channels, kSampleRate));

    const auto ec3_path = dir / "qc_mismatch.ec3";
    constexpr int kWrongDialnorm = 1;  // claims -1 LKFS - as loud as dialnorm can say
    REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + ec3_path.string() +
                        "\" 192 none 51 dialnorm=" + std::to_string(kWrongDialnorm),
                    dir / "qc_mismatch_encode.log") == 0);

    const auto log = dir / "qc_mismatch.log";
    run_cli("qc \"" + ec3_path.string() + "\" preset=atsc-a85", log);
    const auto text = read_log(log);
    INFO(text);

    const auto dialnorm = value_after(text, "dialnorm");
    REQUIRE(dialnorm.has_value());
    CHECK(*dialnorm == Catch::Approx(kWrongDialnorm));

    const auto measured = value_after(text, "integrated loudness");
    REQUIRE(measured.has_value());

    const auto delta = value_after(text, "delta");
    REQUIRE(delta.has_value());
    // Deliberately, substantially negative - measured is far quieter than
    // the -1 LKFS dialnorm=1 claims, by construction (see this test's own
    // comment above), not a rounding nuance a correct-but-tiny delta could
    // also produce.
    CHECK(*delta < -5.0);

    // The measurement-derived dialnorm this mismatch implies is nowhere near
    // the wrong embedded value - the disagreement is real, not cosmetic.
    const auto implied_pos = text.find("measurement-derived dialnorm would be");
    REQUIRE(implied_pos != std::string::npos);
    const auto implied = value_after(text.substr(implied_pos), "would be");
    REQUIRE(implied.has_value());
    CHECK(*implied != kWrongDialnorm);
    CHECK(*implied > kWrongDialnorm + 10);  // a much quieter (larger) dialnorm code

    // The gate line's own arithmetic is self-consistent too: ATSC A/85's
    // published target/tolerance/ceiling (Sec.6: -24 LKFS +/-2 dB, true peak
    // <= -2 dBTP - see ac3/meta/qc.hpp's own citation) reproduced here as
    // literals to check the CLI's printed numbers against, not derived from
    // them. This does not assume which way the verdict lands (a quiet
    // dialnorm=1 mismatch says nothing about where -22-ish LKFS content
    // happens to sit relative to a -24 LKFS target, and it turns out to
    // land inside tolerance here) - only that the printed verdict is exactly
    // what evaluate_qc_gate's own |delta| <= tolerance / measured <= ceiling
    // rule would produce from the printed numbers.
    constexpr double kAtscTargetLkfs = -24.0;
    constexpr double kAtscToleranceLu = 2.0;
    constexpr double kAtscMaxTruePeakDbtp = -2.0;
    const auto gate_pos = text.find("atsc-a85:");
    REQUIRE(gate_pos != std::string::npos);
    const auto gate_text = text.substr(gate_pos);
    const auto gate_delta = value_after(gate_text, "delta");
    REQUIRE(gate_delta.has_value());
    CHECK(*gate_delta == Catch::Approx(*measured - kAtscTargetLkfs).margin(0.01));
    const auto expect_loudness_pass = std::abs(*gate_delta) <= kAtscToleranceLu;

    const auto peak_section = gate_text.substr(gate_text.find("true peak"));
    const auto gate_peak_measured = value_after(peak_section, "measured");
    REQUIRE(gate_peak_measured.has_value());
    const auto expect_peak_pass = *gate_peak_measured <= kAtscMaxTruePeakDbtp;

    const auto overall = gate_verdict_after(text, gate_pos);
    REQUIRE(overall.has_value());
    CHECK(*overall == (expect_loudness_pass && expect_peak_pass));
}

TEST_CASE(
    "qc preset=all checks every preset, and the exit code matches whether every verdict passed",
    "[cli][qc]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_all_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    // 96000 samples = 2 s: BS.1770's absolute gate needs a full 400 ms
    // block before integrated_lkfs() reports anything at all (LoudnessMeter's
    // own doc comment) - a handful of AC-3 frames is not enough, the same
    // reason the dialnorm=auto tests above all use this same duration.
    constexpr std::size_t kFrames = 96000;
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());
    const auto ec3_path = dir / "qc_all.ec3";
    REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + ec3_path.string() +
                        "\" 256 cpl 51 dialnorm=auto",
                    dir / "qc_all_encode.log") == 0);

    const auto log = dir / "qc_all.log";
    const auto rc = run_cli("qc \"" + ec3_path.string() + "\" preset=all", log);
    const auto text = read_log(log);
    INFO(text);

    for (const std::string_view name : {"ebu-r128-s2:", "atsc-a85:", "netflix:"}) {
        CHECK(text.find(name) != std::string::npos);
    }

    const auto ebu_pass = gate_verdict_after(text, text.find("ebu-r128-s2:"));
    const auto atsc_pass = gate_verdict_after(text, text.find("atsc-a85:"));
    const auto netflix_pass = gate_verdict_after(text, text.find("netflix:"));
    REQUIRE(ebu_pass.has_value());
    REQUIRE(atsc_pass.has_value());
    REQUIRE(netflix_pass.has_value());

    // The exit code (this project's own binary 0/1 convention - see this
    // file's own run_cli comment) must match "every requested gate passed",
    // recomputed from the same three verdicts the log itself printed -
    // whichever way the real BS.1770 numbers actually land, a bug that ORs
    // instead of ANDs the per-preset verdicts (or ignores one preset
    // entirely) shows up here as rc disagreeing with this recomputation.
    const bool expect_success = *ebu_pass && *atsc_pass && *netflix_pass;
    CHECK((rc == 0) == expect_success);
}

// silence/eac3-silence build their frames from build_silent_stereo_frame/
// build_silent_access_unit, both already covered directly (test_frame.cpp,
// test_eac3.cpp) - what neither of those unit-tests can see is the CLI
// dispatch row itself: whether 'silence <out> [seconds] [bitrate_kbps]' and
// 'eac3-silence <out> [seconds] [bitrate_kbps] [layout]' actually route argv
// to the right parameters. main.cpp's own comment on the command table
// above records six real argv-index bugs found consolidating the old
// if-chain into that table, none of which a unit test on the frame builders
// alone could have caught - only running the built binary can.

TEST_CASE("silence writes the documented default duration/bitrate and decodes to genuine "
         "silence",
         "[cli][silence]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "silence_default.ac3";
    const auto log = dir / "silence_default.log";
    fs::remove(out_path);
    REQUIRE(run_cli("silence \"" + out_path.string() + "\"", log) == 0);

    // Defaults are 5 s at 192 kbps (see main.cpp's kCommands row and
    // run_silence's own u32(2, 5)/u32(3, 192) fallbacks) - pinned here
    // against the same frame_size_bytes() the encoder itself uses, not a
    // transcribed byte count.
    const std::uint64_t expect_count = (5ull * 48000 + 1535) / 1536;
    const auto frame_bytes = ac3::frame_size_bytes(ac3::SampleRate::k48000, 192);
    REQUIRE(frame_bytes.has_value());
    REQUIRE(fs::exists(out_path));
    CHECK(fs::file_size(out_path) == expect_count * *frame_bytes);

    const auto wav_path = dir / "silence_default.wav";
    REQUIRE(run_cli("decode \"" + out_path.string() + "\" \"" + wav_path.string() + "\"",
                    dir / "silence_default_decode.log") == 0);
    const auto decoded = ac3::io::read_wav(wav_path.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 2);
    REQUIRE(decoded->frame_count() > 0);
    for (const auto& channel : decoded->channels) {
        CHECK(rms(channel, 0, channel.size()) == 0.0);
    }
}

TEST_CASE("silence threads its seconds and bitrate arguments to the right parameters, "
         "not swapped",
         "[cli][silence]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "silence_explicit.ac3";
    const auto log = dir / "silence_explicit.log";
    fs::remove(out_path);
    // 2 seconds at 384 kbps: distinct enough from the 5 s/192 kbps default
    // that a swapped or misindexed argv read produces a file size neither
    // combination could coincidentally match.
    REQUIRE(run_cli("silence \"" + out_path.string() + "\" 2 384", log) == 0);

    const std::uint64_t expect_count = (2ull * 48000 + 1535) / 1536;
    const auto frame_bytes = ac3::frame_size_bytes(ac3::SampleRate::k48000, 384);
    REQUIRE(frame_bytes.has_value());
    REQUIRE(fs::exists(out_path));
    CHECK(fs::file_size(out_path) == expect_count * *frame_bytes);
}

TEST_CASE("silence rejects an illegal bitrate and leaves no file behind", "[cli][silence]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "silence_bad_bitrate.ac3";
    const auto log = dir / "silence_bad_bitrate.log";
    fs::remove(out_path);
    const auto rc = run_cli("silence \"" + out_path.string() + "\" 1 193", log);
    CHECK(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    CHECK(read_log(log).find("bitrate") != std::string::npos);
}

TEST_CASE("eac3-silence threads its layout argument through to the reported label and "
         "substream count, and decodes to genuine silence",
         "[cli][eac3-silence]") {
    const auto dir = scratch_dir();

    const auto stereo_path = dir / "eac3_silence_stereo.ec3";
    const auto stereo_log = dir / "eac3_silence_stereo.log";
    fs::remove(stereo_path);
    REQUIRE(run_cli("eac3-silence \"" + stereo_path.string() + "\"", stereo_log) == 0);
    const auto stereo_text = read_log(stereo_log);
    INFO(stereo_text);
    CHECK(stereo_text.find("2/0 stereo") != std::string::npos);  // the "stereo" default's label
    CHECK(stereo_text.find("1 substreams") != std::string::npos);

    const auto surround_path = dir / "eac3_silence_51.ec3";
    const auto surround_log = dir / "eac3_silence_51.log";
    fs::remove(surround_path);
    REQUIRE(run_cli("eac3-silence \"" + surround_path.string() + "\" 1 192 51", surround_log) ==
           0);
    const auto surround_text = read_log(surround_log);
    INFO(surround_text);
    CHECK(surround_text.find("5.1") != std::string::npos);
    CHECK(surround_text.find("2/0 stereo") == std::string::npos);

    const auto wav_path = dir / "eac3_silence_51.wav";
    REQUIRE(run_cli("decode \"" + surround_path.string() + "\" \"" + wav_path.string() + "\"",
                    dir / "eac3_silence_51_decode.log") == 0);
    const auto decoded = ac3::io::read_wav(wav_path.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->frame_count() > 0);
    for (const auto& channel : decoded->channels) {
        CHECK(rms(channel, 0, channel.size()) == 0.0);
    }
}

// decode's own object-layer gap (PRs #168/#169 made Eac3Decoder actually
// populate DecodedAccessUnit::object_metadata/object_audio; nothing on the
// CLI side read either field until now): a plain decode should report the
// object count it found, and objects_dir should export each JOC-
// reconstructed object as its own genuinely non-silent mono WAV - distinct
// from the bed and, since every 'atmos' object gets its own tone, from each
// other too.
TEST_CASE("decode reports the object layer of an Atmos stream and exports it with objects_dir",
          "[cli][decode][atmos]") {
    const auto dir = scratch_dir();
    const auto ec3_path = dir / "decode_atmos_objects.ec3";
    REQUIRE(run_cli("atmos \"" + ec3_path.string() + "\" 1 448 3 4 objects",
                    dir / "decode_atmos_objects_encode.log") == 0);

    // Plain decode, no objects_dir: the summary line is there, no export happens.
    const auto wav_path = dir / "decode_atmos_objects.wav";
    const auto plain_log = dir / "decode_atmos_objects_plain.log";
    REQUIRE(
        run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_path.string() + "\"", plain_log) ==
        0);
    const auto plain_text = read_log(plain_log);
    INFO(plain_text);
    CHECK(plain_text.find("3 dynamic objects") != std::string::npos);
    CHECK(plain_text.find("4 objects") != std::string::npos);
    CHECK(plain_text.find("JOC audio reconstructed") != std::string::npos);
    CHECK(plain_text.find("wrote") == std::string::npos);

    // Same stream, with objects_dir: one object_NN.wav per dynamic object.
    const auto objects_dir = dir / "decode_atmos_objects_out";
    fs::remove_all(objects_dir);
    const auto export_log = dir / "decode_atmos_objects_export.log";
    REQUIRE(run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_path.string() + "\" \"" +
                        objects_dir.string() + "\"",
                    export_log) == 0);
    const auto export_text = read_log(export_log);
    INFO(export_text);
    CHECK(export_text.find("wrote 3 object WAV(s)") != std::string::npos);

    std::vector<std::vector<float>> object_channels;
    for (int i = 0; i < 3; ++i) {
        const auto object_path = objects_dir / ("object_0" + std::to_string(i) + ".wav");
        REQUIRE(fs::exists(object_path));
        const auto decoded = ac3::io::read_wav(object_path.string());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 1);
        REQUIRE(decoded->frame_count() > 0);
        CHECK(rms(decoded->channels[0], 0, decoded->channels[0].size()) > 0.0);
        object_channels.push_back(decoded->channels[0]);
    }
    // Three distinct orbiting tones in, so not every object should have
    // decoded to the same waveform - compared as a bool for the same reason
    // the atmos-encode motion-vs-static test above does (stringifying a
    // multi-thousand-sample diff on failure is slow and was seen to crash).
    CHECK(object_channels[0] != object_channels[1]);
    CHECK(object_channels[1] != object_channels[2]);
}

TEST_CASE("decode objects_dir warns instead of exporting when there is no object audio to export",
          "[cli][decode][atmos]") {
    const auto dir = scratch_dir();

    SECTION("plain AC-3 has no object layer at all") {
        const auto ac3_path = dir / "decode_objects_plain_ac3.ac3";
        REQUIRE(run_cli("sine \"" + ac3_path.string() + "\" 1 192 1000 50 stereo",
                        dir / "decode_objects_plain_ac3_encode.log") == 0);
        const auto wav_path = dir / "decode_objects_plain_ac3.wav";
        const auto objects_dir = dir / "decode_objects_plain_ac3_out";
        fs::remove_all(objects_dir);
        const auto log = dir / "decode_objects_plain_ac3.log";
        REQUIRE(run_cli("decode \"" + ac3_path.string() + "\" \"" + wav_path.string() + "\" \"" +
                            objects_dir.string() + "\"",
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("no object layer") != std::string::npos);
        CHECK_FALSE(fs::exists(objects_dir));
    }

    SECTION("bed51 mode carries no object container to reconstruct from") {
        const auto ec3_path = dir / "decode_objects_bed51.ec3";
        REQUIRE(run_cli("atmos \"" + ec3_path.string() + "\" 1 448 3 4 bed51",
                        dir / "decode_objects_bed51_encode.log") == 0);
        const auto wav_path = dir / "decode_objects_bed51.wav";
        const auto objects_dir = dir / "decode_objects_bed51_out";
        fs::remove_all(objects_dir);
        const auto log = dir / "decode_objects_bed51.log";
        REQUIRE(run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_path.string() + "\" \"" +
                            objects_dir.string() + "\"",
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("no reconstructed object audio to export") != std::string::npos);
        CHECK_FALSE(fs::exists(objects_dir));
    }
}

// Regression test for a latent bug in run_decode_eac3's Eac3Decoder::flush()
// tail loop: it used to append a flushed substream's channels at pcm[0..N)
// by CODED index, rather than at the Table E2.5 slot decode_access_unit's
// own §E3.8.2 assembly would have used. That is silently correct for a lone
// independent substream (its coded order already matches pcm's established
// index order), but wrong the moment flush() hands back a DEPENDENT on its
// own - its smaller channel set then lands in pcm[0]/pcm[1] (the bed's L/R)
// instead of its own height slots, leaving some pcm[] vectors longer than
// others once interleaved into the WAV.
//
// §3.7 transient pre-noise processing is what can make flush() return a raw
// per-substream result at all (decoder.hpp's own doc comment on
// Eac3Decoder::flush): a substream identity holds its last frame back
// exactly when ITS OWN channels carry a transient on the stream's very last
// frame, with no following frame to release it. tools=tpn turns the tool on
// for every substream uniformly (plan.cpp's apply_tools), but activation is
// decided per substream from its own real audio content - so a bed that ends
// on a sharp transient while its height dependent stays silent desyncs
// exactly like this at end of stream, the same asymmetry
// test_eac3_decoder.cpp's own "decode_access_unit queues a substream..."
// test already proves reliably triggers at the library level.
TEST_CASE(
    "decode assembles a flush()'d transient-pre-noise tail into the correct channel slots, not "
    "coded-order-into-pcm-index",
    "[cli][decode][eac3][transient_prenoise]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "flush_desync_in.wav";

    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrame = static_cast<std::size_t>(ac3::kSamplesPerFrame);
    constexpr std::size_t kSilentFrames = 4;
    // Within the final frame - the same offset the library's own
    // transient_prenoise unit test uses to trigger block switching.
    constexpr std::size_t kOnset = 960;
    constexpr std::size_t kTotalFrames = (kSilentFrames + 1) * kFrame;

    // Bed (L, R): silent until the very last frame, then a loud 1 kHz onset.
    std::vector<float> bed_channel(kTotalFrames, 0.0f);
    for (std::size_t n = kSilentFrames * kFrame + kOnset; n < kTotalFrames; ++n) {
        bed_channel[n] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) /
                          static_cast<double>(kRate)));
    }
    // The height dependent (Vhl, Vhr): silent for the whole file, so it never
    // sets transproce itself and keeps releasing every call right up to the
    // stream's last frame - the asymmetry the bug needs. WAV channel order
    // matches Table E2.5/kWavSpeakerOrder for this location set: L, R, then
    // Vhl, Vhr.
    const std::vector<float> silence(kTotalFrames, 0.0f);
    REQUIRE(write_wav(wav_path, {bed_channel, bed_channel, silence, silence}, kRate));

    const auto ec3_path = dir / "flush_desync.ec3";
    const auto encode_log = dir / "flush_desync_encode.log";
    const auto encode_rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                       ec3_path.string() + "\" 192 tpn L,R,Vhl,Vhr",
                                   encode_log);
    INFO(read_log(encode_log));
    REQUIRE(encode_rc == 0);

    const auto wav_out = dir / "flush_desync_out.wav";
    const auto decode_log = dir / "flush_desync_decode.log";
    const auto decode_rc =
        run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_out.string() + "\"", decode_log);
    INFO(read_log(decode_log));
    REQUIRE(decode_rc == 0);

    const auto decoded = ac3::io::read_wav(wav_out.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 4);

    // The core symptom this bug produced: some pcm[] vectors longer than
    // others once a flushed substream's channels landed at the wrong index.
    // Every channel must come out exactly as long as every other.
    const auto expected_length = decoded->channels[0].size();
    for (const auto& channel : decoded->channels) {
        CHECK(channel.size() == expected_length);
    }
    // Every real frame must have made it out, including the desynced tail -
    // a fix that merely made the lengths agree by truncating the flush()
    // tail would still pass the check above but fail this one.
    CHECK(expected_length >= kTotalFrames);

    // And the tail must have landed in the RIGHT channels: the bed's onset
    // in L/R (indices 0/1), silence still in the height pair (indices 2/3) -
    // not the reverse, and not smeared across both by a coded-order copy.
    const auto tail_from = kSilentFrames * kFrame + kOnset;
    for (const std::size_t ch : {std::size_t{0}, std::size_t{1}}) {
        CHECK(rms(decoded->channels[ch], tail_from, expected_length - tail_from) > 0.3);
    }
    for (const std::size_t ch : {std::size_t{2}, std::size_t{3}}) {
        CHECK(rms(decoded->channels[ch], tail_from, expected_length - tail_from) < 0.05);
    }
}
