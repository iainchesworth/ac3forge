#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <print>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"

namespace {

void print_usage() {
    std::println("ac3forge 0.1.0 — clean-room AC-3 (ATSC A/52) encoder, work in progress");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli silence <out.ac3> [seconds] [bitrate_kbps]");
    std::println("  ac3cli sine    <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amplitude_pct]");
    std::println("");
    std::println("2/0 (stereo) at 48 kHz. Defaults: 5 s, 192 kbps, 1000 Hz, 50%.");
    std::println("Verify: ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 -f null -");
}

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

}  // namespace

int run_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate) {
    ac3::SilentFrameConfig config{.bitrate_kbps = bitrate};
    const auto frame = ac3::build_silent_stereo_frame(config);
    if (!frame) {
        std::println(stderr, "error: invalid config (bitrate must be one of the 19 legal AC-3 rates)");
        return 1;
    }
    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    for (std::uint64_t i = 0; i < frames; ++i) {
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
    }
    std::println("wrote {} silent frames ({} bytes each, {} kbps) to {}", frames, frame->size(),
                 bitrate, out_path);
    return 0;
}

int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t freq_hz, std::uint32_t amplitude_pct) {
    ac3::StereoEncoder encoder{{.bitrate_kbps = bitrate}};
    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    const double amplitude = amplitude_pct / 100.0;

    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    std::vector<float> samples(ac3::kSamplesPerFrame);
    std::uint64_t n = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        for (auto& s : samples) {
            s = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * freq_hz * n / 48000.0));
            ++n;
        }
        const auto frame = encoder.encode_frame(samples, samples);
        if (!frame) {
            std::println(stderr, "error: invalid config (bitrate must be a legal AC-3 rate)");
            return 1;
        }
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
    }
    std::println("wrote {} sine frames ({} Hz, {}% amplitude, {} kbps) to {}", frames, freq_hz,
                 amplitude_pct, bitrate, out_path);
    return 0;
}

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 3) {
        print_usage();
        return args.size() < 2 ? 0 : 1;
    }
    const std::string_view command{args[1]};
    const std::string_view out_path{args[2]};
    const std::uint32_t seconds = args.size() > 3 ? parse_u32_or(args[3], 5) : 5;
    const std::uint32_t bitrate = args.size() > 4 ? parse_u32_or(args[4], 192) : 192;

    if (command == "silence") {
        return run_silence(out_path, seconds, bitrate);
    }
    if (command == "sine") {
        const std::uint32_t freq = args.size() > 5 ? parse_u32_or(args[5], 1000) : 1000;
        const std::uint32_t amplitude = args.size() > 6 ? parse_u32_or(args[6], 50) : 50;
        return run_sine(out_path, seconds, bitrate, freq, amplitude);
    }
    print_usage();
    return 1;
}
