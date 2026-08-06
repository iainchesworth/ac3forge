#include <charconv>
#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string_view>

#include "ac3/encoder/silent_frame.hpp"

namespace {

void print_usage() {
    std::println("ac3forge 0.1.0 — clean-room AC-3 (ATSC A/52) encoder, work in progress");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli silence <out.ac3> [seconds] [bitrate_kbps]");
    std::println("");
    std::println("Writes 2/0 (stereo) digital silence at 48 kHz. Defaults: 5 s, 192 kbps.");
    std::println("Roadmap: docs/RESEARCH.md (next: MDCT + real audio).");
}

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 3 || std::string_view{args[1]} != "silence") {
        print_usage();
        return args.size() < 2 ? 0 : 1;
    }

    const std::string_view out_path{args[2]};
    const std::uint32_t seconds = args.size() > 3 ? parse_u32_or(args[3], 5) : 5;
    const std::uint32_t bitrate = args.size() > 4 ? parse_u32_or(args[4], 192) : 192;

    ac3::SilentFrameConfig config{.bitrate_kbps = bitrate};
    const auto frame = ac3::build_silent_stereo_frame(config);
    if (!frame) {
        std::println(stderr, "error: invalid config (bitrate must be one of the 19 legal AC-3 rates)");
        return 1;
    }

    // 48 kHz: 1536 samples per frame -> 31.25 frames per second.
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
    out.close();

    std::println("wrote {} frames ({} bytes each, {} kbps, 48 kHz stereo) to {}", frames,
                 frame->size(), bitrate, out_path);
    std::println("verify: ffprobe {}", out_path);
    std::println("        ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i {} -f null -",
                 out_path);
    return 0;
}
