#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"

namespace {

void print_usage() {
    std::println("ac3forge 0.1.0 — clean-room AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli silence <out.ac3> [seconds] [bitrate_kbps]");
    std::println("  ac3cli sine    <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]");
    std::println("  ac3cli decode  <in.ac3> <out.wav>");
    std::println("");
    std::println("layout: stereo (default) | 51 — 5.1 uses per-channel tones");
    std::println("        (L 1000, C 800, R 1200, SL 600, SR 1400, LFE 60 Hz)");
    std::println("decode writes float32 WAV in WAV channel order for direct comparison");
    std::println("with: ffmpeg -i in.ac3 -c:a pcm_f32le out.wav");
}

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

int run_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate) {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = bitrate});
    if (!frame) {
        std::println(stderr, "error: bitrate must be one of the 19 legal AC-3 rates");
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
    std::println("wrote {} silent frames to {}", frames, out_path);
    return 0;
}

int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout) {
    const bool surround = layout == "51";
    ac3::EncoderConfig config{.bitrate_kbps = bitrate};
    std::vector<double> tone_hz;
    if (surround) {
        config.acmod = ac3::Acmod::k3_2;
        config.lfe = true;
        tone_hz = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};  // L C R SL SR LFE
    } else {
        tone_hz = {static_cast<double>(freq_hz), static_cast<double>(freq_hz)};
    }
    ac3::FrameEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const double amplitude = amplitude_pct / 100.0;

    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> samples(nchans,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                samples[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                    amplitude * std::sin(2.0 * std::numbers::pi * tone_hz[ch] *
                                         static_cast<double>(n0 + static_cast<std::uint64_t>(i)) /
                                         48000.0));
            }
            views[ch] = samples[ch];
        }
        n0 += ac3::kSamplesPerFrame;
        const auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
    }
    std::println("wrote {} {} frames ({} kbps) to {}", frames, surround ? "5.1" : "stereo",
                 bitrate, out_path);
    return 0;
}

// AC-3 channel order -> standard WAV order for the supported layouts.
std::vector<std::size_t> wav_channel_map(ac3::Acmod acmod, bool lfe) {
    if (acmod == ac3::Acmod::k3_2 && lfe) {
        return {0, 2, 1, 5, 3, 4};  // FL FR FC LFE BL BR <- L C R SL SR LFE
    }
    std::vector<std::size_t> identity(
        static_cast<std::size_t>(ac3::fullbw_channel_count(acmod)) + (lfe ? 1 : 0));
    for (std::size_t i = 0; i < identity.size(); ++i) {
        identity[i] = i;
    }
    return identity;
}

void write_wav_f32(std::ofstream& out, const std::vector<std::vector<float>>& interleave_src,
                   const std::vector<std::size_t>& map, std::uint32_t sample_rate_hz,
                   std::uint64_t total_samples) {
    const auto channels = static_cast<std::uint32_t>(map.size());
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(total_samples) * channels * 4;
    const auto put_u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put_u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    put_u32(36 + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(16);
    put_u16(3);  // IEEE float
    put_u16(static_cast<std::uint16_t>(channels));
    put_u32(sample_rate_hz);
    put_u32(sample_rate_hz * channels * 4);
    put_u16(static_cast<std::uint16_t>(channels * 4));
    put_u16(32);
    out.write("data", 4);
    put_u32(data_bytes);
    for (std::uint64_t n = 0; n < total_samples; ++n) {
        for (const auto src : map) {
            const float v = interleave_src[src][static_cast<std::size_t>(n)];
            out.write(reinterpret_cast<const char*>(&v), 4);
        }
    }
}

int run_decode(std::string_view in_path, std::string_view out_path) {
    std::ifstream in{std::string{in_path}, std::ios::binary};
    if (!in) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto stream = std::as_bytes(std::span{raw});

    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::println(stderr, "error: stream framing failed (code {})",
                     static_cast<int>(frames.error()));
        return 1;
    }
    ac3::FrameDecoder decoder;
    std::vector<std::vector<float>> pcm;  // AC-3 order accumulation
    ac3::DecodedFrame first{};
    bool have_first = false;
    for (const auto& frame : *frames) {
        auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            std::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            return 1;
        }
        if (!have_first) {
            first = *decoded;
            pcm.resize(decoded->channels.size());
            have_first = true;
        }
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), decoded->channels[ch].begin(),
                           decoded->channels[ch].end());
        }
    }
    if (!have_first) {
        std::println(stderr, "error: no frames");
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    const auto map = wav_channel_map(first.acmod, first.lfe);
    write_wav_f32(out, pcm, map, sample_rate_hz(first.sample_rate), pcm[0].size());
    std::println("decoded {} frames -> {} ({} channels, {} Hz)", frames->size(), out_path,
                 map.size(), sample_rate_hz(first.sample_rate));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 3) {
        print_usage();
        return args.size() < 2 ? 0 : 1;
    }
    const std::string_view command{args[1]};
    if (command == "silence") {
        return run_silence(args[2], args.size() > 3 ? parse_u32_or(args[3], 5) : 5,
                           args.size() > 4 ? parse_u32_or(args[4], 192) : 192);
    }
    if (command == "sine") {
        return run_sine(args[2], args.size() > 3 ? parse_u32_or(args[3], 5) : 5,
                        args.size() > 4 ? parse_u32_or(args[4], 192) : 192,
                        args.size() > 5 ? parse_u32_or(args[5], 1000) : 1000,
                        args.size() > 6 ? parse_u32_or(args[6], 50) : 50,
                        args.size() > 7 ? std::string_view{args[7]} : "stereo");
    }
    if (command == "decode" && args.size() > 3) {
        return run_decode(args[2], args[3]);
    }
    print_usage();
    return 1;
}
