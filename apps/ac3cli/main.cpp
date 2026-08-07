#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
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
#include "sinks/iec61937.hpp"
#include "spatial/spatial.hpp"

namespace {

void print_usage() {
    std::println("ac3forge 0.1.0 — clean-room AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli silence <out.ac3> [seconds] [bitrate_kbps]");
    std::println("  ac3cli sine    <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]");
    std::println("  ac3cli encode  <in.wav> <out.ac3> [bitrate_kbps]");
    std::println("  ac3cli decode  <in.ac3> <out.wav>");
    std::println("  ac3cli orbit   <out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]");
    std::println("  ac3cli spdif   <in.ac3> <out.wav>   (IEC 61937 wrap as playable PCM16 WAV)");
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

// A 440 Hz tone orbiting the listener once per orbit_seconds, rendered
// through the spatial layer into 5.1 — the "move sound in space and hear it
// in the stream" demo.
int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t orbit_seconds) {
    ac3::spatial::BedRenderer renderer;
    const auto object =
        renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7, .lfe_send = 0.15});
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = bitrate, .acmod = ac3::Acmod::k3_2, .lfe = true}};

    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<float> mono(ac3::spatial::kBlockSamples);
    std::vector<std::vector<float>> frame_channels(6);
    std::vector<std::vector<float>> bed_block(
        6, std::vector<float>(ac3::spatial::kBlockSamples));
    std::vector<std::span<const float>> views(6);
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        for (auto& channel : frame_channels) {
            channel.clear();
        }
        for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
            const double seconds_now = static_cast<double>(n0) / 48000.0;
            renderer.set_target(
                object, {.azimuth_deg = 360.0 * seconds_now /
                                        std::max<std::uint32_t>(orbit_seconds, 1),
                         .gain = 0.7,
                         .lfe_send = 0.15});
            for (std::size_t n = 0; n < mono.size(); ++n) {
                mono[n] = static_cast<float>(
                    0.6 * std::sin(2.0 * std::numbers::pi * 440.0 *
                                   static_cast<double>(n0 + n) / 48000.0));
            }
            n0 += mono.size();
            const std::array<std::span<const float>, 1> audio = {mono};
            const std::array<std::span<float>, 6> bed_views = {
                bed_block[0], bed_block[1], bed_block[2],
                bed_block[3], bed_block[4], bed_block[5]};
            renderer.render_block(audio, bed_views);
            for (std::size_t ch = 0; ch < 6; ++ch) {
                frame_channels[ch].insert(frame_channels[ch].end(), bed_block[ch].begin(),
                                          bed_block[ch].end());
            }
        }
        for (std::size_t ch = 0; ch < 6; ++ch) {
            views[ch] = frame_channels[ch];
        }
        const auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
    }
    std::println("wrote {} 5.1 frames: 440 Hz tone orbiting every {} s -> {}", frames,
                 orbit_seconds, out_path);
    return 0;
}

// Minimal WAV reader: PCM16 or float32, any channel count.
bool read_wav(std::string_view path, std::vector<std::vector<float>>& channels,
              std::uint32_t& rate) {
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return false;
    }
    std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::string_view view{raw.data(), raw.size()};
    const auto fmt_pos = view.find("fmt ");
    const auto data_pos = view.find("data");
    if (fmt_pos == std::string_view::npos || data_pos == std::string_view::npos) {
        return false;
    }
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(raw[at]) |
                                          (static_cast<std::uint8_t>(raw[at + 1]) << 8));
    };
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(u16(at)) | (static_cast<std::uint32_t>(u16(at + 2)) << 16);
    };
    const auto format = u16(fmt_pos + 8);
    const auto nch = u16(fmt_pos + 10);
    rate = u32(fmt_pos + 12);
    const auto bits = u16(fmt_pos + 22);
    const auto data_bytes = u32(data_pos + 4);
    const std::size_t data_at = data_pos + 8;
    if (nch == 0 || data_at + data_bytes > raw.size()) {
        return false;
    }
    const std::size_t frame_bytes = static_cast<std::size_t>(nch) * bits / 8;
    const std::size_t frames = data_bytes / frame_bytes;
    channels.assign(nch, std::vector<float>(frames));
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::uint16_t c = 0; c < nch; ++c) {
            const std::size_t at = data_at + f * frame_bytes + c * bits / 8;
            if (format == 3 && bits == 32) {
                float v;
                std::memcpy(&v, raw.data() + at, 4);
                channels[c][f] = v;
            } else if (format == 1 && bits == 16) {
                const auto s = static_cast<std::int16_t>(u16(at));
                channels[c][f] = static_cast<float>(s) / 32768.0f;
            } else {
                return false;
            }
        }
    }
    return true;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate) {
    std::vector<std::vector<float>> channels;
    std::uint32_t rate = 0;
    if (!read_wav(in_path, channels, rate)) {
        std::println(stderr, "error: cannot read {} (PCM16/float32 WAV expected)", in_path);
        return 1;
    }
    if (channels.size() != 2) {
        std::println(stderr, "error: encode currently expects stereo input");
        return 1;
    }
    ac3::SampleRate sr;
    switch (rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            std::println(stderr, "error: sample rate {} not legal for AC-3", rate);
            return 1;
    }
    ac3::FrameEncoder encoder{{.sample_rate = sr, .bitrate_kbps = bitrate}};
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    const std::size_t total = channels[0].size();
    std::vector<std::vector<float>> block(2, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(2);
    std::size_t written = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        for (std::size_t c = 0; c < 2; ++c) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[c][static_cast<std::size_t>(i)] = at < total ? channels[c][at] : 0.0f;
            }
            views[c] = block[c];
        }
        const auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
        ++written;
    }
    std::println("encoded {} frames ({} kbps, {} Hz) to {}", written, bitrate, rate, out_path);
    return 0;
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

// Wrap a raw AC-3 stream into IEC 61937 bursts inside a PCM16 stereo WAV:
// played BIT-EXACTLY (volume 100%, no mixing) into an S/PDIF or HDMI output,
// a receiver locks onto the bursts and lights up "Dolby Digital".
int run_spdif(std::string_view in_path, std::string_view out_path) {
    std::ifstream in{std::string{in_path}, std::ios::binary};
    if (!in) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto frames = ac3::split_frames(std::as_bytes(std::span{raw}));
    if (!frames || frames->empty()) {
        std::println(stderr, "error: not a valid AC-3 stream");
        return 1;
    }
    const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
    const auto rate = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));

    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", out_path);
        return 1;
    }
    const auto data_bytes =
        static_cast<std::uint32_t>(frames->size() * ac3::iec61937::kBurstBytes);
    const auto put_u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put_u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    put_u32(36 + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(16);
    put_u16(1);  // PCM
    put_u16(2);  // stereo carrier
    put_u32(rate);
    put_u32(rate * 4);
    put_u16(4);
    put_u16(16);
    out.write("data", 4);
    put_u32(data_bytes);
    for (const auto& frame : *frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        if (!burst) {
            std::println(stderr, "error: burst wrap failed");
            return 1;
        }
        out.write(reinterpret_cast<const char*>(burst->data()),
                  static_cast<std::streamsize>(burst->size()));
    }
    std::println("wrapped {} frames into IEC 61937 bursts -> {} ({} Hz carrier)",
                 frames->size(), out_path, rate);
    std::println("play bit-exactly (100% volume, exclusive/passthrough output) to light up");
    std::println("a receiver's Dolby Digital indicator.");
    return 0;
}

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
    if (command == "orbit") {
        return run_orbit(args[2], args.size() > 3 ? parse_u32_or(args[3], 8) : 8,
                         args.size() > 4 ? parse_u32_or(args[4], 448) : 448,
                         args.size() > 5 ? parse_u32_or(args[5], 4) : 4);
    }
    if (command == "encode" && args.size() > 3) {
        return run_encode(args[2], args[3], args.size() > 4 ? parse_u32_or(args[4], 192) : 192);
    }
    if (command == "decode" && args.size() > 3) {
        return run_decode(args[2], args[3]);
    }
    if (command == "spdif" && args.size() > 3) {
        return run_spdif(args[2], args[3]);
    }
    print_usage();
    return 1;
}
