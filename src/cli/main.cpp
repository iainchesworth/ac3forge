#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/capture/capture.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/passthrough.hpp"
#include "ac3/spatial/spatial.hpp"

namespace {

void print_usage() {
    std::println("ac3forge — clean-room AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli silence <out.ac3> [seconds] [bitrate_kbps]");
    std::println("  ac3cli sine    <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]");
    std::println("  ac3cli orbit   <out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]");
    std::println("  ac3cli devices");
    std::println("  ac3cli record  <out.ac3> [seconds] [bitrate_kbps] [device_index]");
    std::println("  ac3cli encode  <in.wav> <out.ac3> [bitrate_kbps] [couple]");
    std::println("  ac3cli decode  <in.ac3> <out.wav>");
    std::println("  ac3cli spdif   <in.ac3> <out.wav>   (IEC 61937 wrap as playable PCM16 WAV)");
    std::println("  ac3cli outputs                      (render endpoints + AC-3 passthrough support)");
    std::println("  ac3cli play    <in.ac3> [device_index]  (exclusive-mode IEC 61937 passthrough)");
    std::println("");
    std::println("layout: stereo (default) | 51 — 5.1 uses per-channel tones;");
    std::println("        append 'c' (stereoc, 51c) to enable channel coupling");
    std::println("        (L 1000, C 800, R 1200, SL 600, SR 1400, LFE 60 Hz)");
}

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

bool write_frames(std::string_view path, std::span<const std::vector<std::byte>> frames) {
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", path);
        return false;
    }
    for (const auto& frame : frames) {
        out.write(reinterpret_cast<const char*>(frame.data()),
                  static_cast<std::streamsize>(frame.size()));
    }
    return true;
}

std::vector<std::byte> read_all(std::string_view path) {
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return {};
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

int run_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate) {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = bitrate});
    if (!frame) {
        std::println(stderr, "error: bitrate must be one of the 19 legal AC-3 rates");
        return 1;
    }
    const std::uint64_t count = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    const std::vector<std::vector<std::byte>> frames(static_cast<std::size_t>(count), *frame);
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} silent frames to {}", count, out_path);
    return 0;
}

int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout) {
    // Layouts: stereo | 51, each optionally suffixed with "c" to turn
    // channel coupling on (e.g. 51c).
    const bool couple = !layout.empty() && layout.back() == 'c';
    const std::string_view base = couple ? layout.substr(0, layout.size() - 1) : layout;
    const bool surround = base == "51";
    ac3::EncoderConfig config{.bitrate_kbps = bitrate, .coupling = couple};
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

    const std::uint64_t count = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> samples(nchans,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(count));
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
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
        auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} {} frames ({} kbps) to {}", count, surround ? "5.1" : "stereo",
                 bitrate, out_path);
    return 0;
}

int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t orbit_seconds) {
    ac3::spatial::BedRenderer renderer;
    const auto object =
        renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7, .lfe_send = 0.15});
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = bitrate, .acmod = ac3::Acmod::k3_2, .lfe = true}};

    const std::uint64_t count = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<float> mono(ac3::spatial::kBlockSamples);
    std::vector<std::vector<float>> frame_channels(6);
    std::vector<std::vector<float>> bed_block(
        6, std::vector<float>(ac3::spatial::kBlockSamples));
    std::vector<std::span<const float>> views(6);
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(count));
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
        for (auto& channel : frame_channels) {
            channel.clear();
        }
        for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
            const double seconds_now = static_cast<double>(n0) / 48000.0;
            renderer.set_target(object,
                                {.azimuth_deg = 360.0 * seconds_now /
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
        auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} 5.1 frames: 440 Hz tone orbiting every {} s -> {}", count,
                 orbit_seconds, out_path);
    return 0;
}

int run_devices() {
    const auto devices = ac3::capture::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::capture::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        std::println("no active capture endpoints found");
        return 0;
    }
    std::println("{:>3}  {:<9} {:>7}  {:>3}  {}", "idx", "kind", "rate", "ch", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        std::println("{:>3}  {:<9} {:>7}  {:>3}  {}{}", i,
                     d.kind == ac3::capture::DeviceKind::kInput ? "input" : "loopback",
                     d.sample_rate, d.channels, d.name, d.is_default ? "  [default]" : "");
    }
    return 0;
}

// Capture live audio and encode it straight to AC-3. The capture thread fills
// a lock-free ring; this thread drains it a frame at a time.
int run_record(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
               int device_index) {
    const auto devices = ac3::capture::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::capture::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        std::println(stderr, "error: no capture endpoints available");
        return 1;
    }
    if (device_index < 0 || static_cast<std::size_t>(device_index) >= devices->size()) {
        std::println(stderr, "error: device index {} out of range (see 'ac3cli devices')",
                     device_index);
        return 1;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(device_index)];

    ac3::SampleRate sr{};
    switch (device.sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            std::println(stderr,
                         "error: device runs at {} Hz; AC-3 needs 32, 44.1 or 48 kHz "
                         "(change the endpoint's shared-mode format in Windows sound settings)",
                         device.sample_rate);
            return 1;
    }

    ac3::capture::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        std::println(stderr, "error: {}", ac3::capture::describe(started.error()));
        return 1;
    }
    const auto channels = capture.channels();
    std::println("recording from \"{}\" ({} Hz, {} ch) for {} s…", device.name,
                 capture.sample_rate(), channels, seconds);

    ac3::FrameEncoder encoder{{.sample_rate = sr, .bitrate_kbps = bitrate}};
    const std::uint64_t target_frames =
        (static_cast<std::uint64_t>(seconds) * capture.sample_rate() + ac3::kSamplesPerFrame - 1) /
        ac3::kSamplesPerFrame;

    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    std::vector<std::vector<float>> planar(2,
                                           std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
    std::vector<std::span<const float>> views{planar[0], planar[1]};
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(target_frames));

    while (frames.size() < target_frames) {
        // Block until a whole AC-3 frame of interleaved samples is available.
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(
                std::span{interleaved}.subspan(filled, interleaved.size() - filled));
            filled += got;
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        // Deinterleave to stereo: take the first two channels, or duplicate a
        // mono source across both.
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::size_t base = static_cast<std::size_t>(i) * channels;
            planar[0][static_cast<std::size_t>(i)] = interleaved[base];
            planar[1][static_cast<std::size_t>(i)] =
                channels > 1 ? interleaved[base + 1] : interleaved[base];
        }
        auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
    }

    capture.stop();
    const auto stats = capture.stats();
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} frames ({} kbps) to {}", frames.size(), bitrate, out_path);
    std::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
    return 0;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    if (wav->channels.size() != 2) {
        std::println(stderr, "error: encode currently expects stereo input ({} channels given)",
                     wav->channels.size());
        return 1;
    }
    ac3::SampleRate sr{};
    switch (wav->sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            std::println(stderr, "error: sample rate {} is not legal for AC-3 (need 32/44.1/48 kHz)",
                         wav->sample_rate);
            return 1;
    }

    ac3::FrameEncoder encoder{
        {.sample_rate = sr, .bitrate_kbps = bitrate, .coupling = couple}};
    const std::size_t total = wav->frame_count();
    std::vector<std::vector<float>> block(2, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(2);
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        for (std::size_t c = 0; c < 2; ++c) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[c][static_cast<std::size_t>(i)] =
                    at < total ? wav->channels[c][at] : 0.0f;
            }
            views[c] = block[c];
        }
        auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("encoded {} frames ({} kbps, {} Hz) to {}", frames.size(), bitrate,
                 wav->sample_rate, out_path);
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

int run_decode(std::string_view in_path, std::string_view out_path) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::println(stderr, "error: stream framing failed (code {})",
                     static_cast<int>(frames.error()));
        return 1;
    }
    ac3::FrameDecoder decoder;
    std::vector<std::vector<float>> pcm;
    ac3::DecodedFrame first{};
    bool have_first = false;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
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
    const auto map = wav_channel_map(first.acmod, first.lfe);
    const auto written = ac3::io::write_wav_f32(std::string{out_path}, pcm,
                                                sample_rate_hz(first.sample_rate), map);
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::println("decoded {} frames -> {} ({} channels, {} Hz)", frames->size(), out_path,
                 map.size(), sample_rate_hz(first.sample_rate));
    return 0;
}

// Wrap a raw AC-3 stream into IEC 61937 bursts inside a PCM16 stereo WAV:
// played BIT-EXACTLY (volume 100%, no mixing) into an S/PDIF or HDMI output,
// a receiver locks onto the bursts and lights up "Dolby Digital".
int run_spdif(std::string_view in_path, std::string_view out_path) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::println(stderr, "error: not a valid AC-3 stream");
        return 1;
    }
    const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
    const auto rate = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));

    std::vector<std::byte> payload;
    payload.reserve(frames->size() * ac3::iec61937::kBurstBytes);
    for (const auto& frame : *frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        if (!burst) {
            std::println(stderr, "error: burst wrap failed");
            return 1;
        }
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    const auto written = ac3::io::write_wav_pcm16_raw(std::string{out_path}, payload, rate, 2);
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::println("wrapped {} frames into IEC 61937 bursts -> {} ({} Hz carrier)",
                 frames->size(), out_path, rate);
    std::println("play bit-exactly (100% volume, exclusive/passthrough output) to light up");
    std::println("a receiver's Dolby Digital indicator.");
    return 0;
}

int run_outputs() {
    const auto devices = ac3::sinks::enumerate_render_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::sinks::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        std::println("no active render endpoints found");
        return 0;
    }
    std::println("{:>3}  {:<9}  {:<9}  {}", "idx", "AC-3", "excl PCM", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        std::println("{:>3}  {:<9}  {:<9}  {}{}", i, d.supports_ac3_passthrough ? "yes" : "no",
                     d.supports_exclusive_pcm ? "yes" : "no", d.name,
                     d.is_default ? "  [default]" : "");
    }
    std::println("");
    std::println("AC-3     the endpoint accepted an IEC 61937 AC-3 format in exclusive mode.");
    std::println("excl PCM the same endpoint accepted ordinary 16-bit stereo PCM exclusively.");
    std::println("");
    std::println("PCM yes + AC-3 no means the device simply cannot bitstream - analog outputs");
    std::println("cannot; only S/PDIF (TOSLINK/coax) and HDMI can. Enable Dolby Digital under");
    std::println("Sound > Playback > Properties > Supported Formats for such a device.");
    std::println("Both no means exclusive mode itself is unavailable (disabled for the device,");
    std::println("or another application currently holds it).");
    return 0;
}

// Stream an AC-3 file to a receiver in real time via exclusive-mode
// IEC 61937. The sink's render thread pulls bursts; this loop keeps it fed.
int run_play(std::string_view in_path, int device_index) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::println(stderr, "error: not a valid AC-3 stream");
        return 1;
    }
    const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
    const auto rate = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));

    const auto devices = ac3::sinks::enumerate_render_devices(rate);
    if (!devices || devices->empty()) {
        std::println(stderr, "error: no render endpoints available");
        return 1;
    }
    std::string device_id;
    std::string device_name = "default endpoint";
    if (device_index >= 0) {
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            std::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return 1;
        }
        const auto& chosen = (*devices)[static_cast<std::size_t>(device_index)];
        device_id = chosen.id;
        device_name = chosen.name;
        if (!chosen.supports_ac3_passthrough) {
            std::println(stderr,
                         "error: \"{}\" does not accept AC-3 over IEC 61937 (see 'ac3cli outputs')",
                         chosen.name);
            return 1;
        }
    }

    ac3::sinks::PassthroughSink sink;
    const auto started = sink.start(device_id, rate);
    if (!started) {
        std::println(stderr, "error: {}", ac3::sinks::describe(started.error()));
        return 1;
    }
    std::println("streaming {} frames to \"{}\" ({} Hz carrier)…", frames->size(), device_name,
                 rate);

    for (const auto& frame : *frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        if (!burst) {
            std::println(stderr, "error: burst wrap failed");
            return 1;
        }
        // Wait for room rather than racing ahead: the render thread consumes
        // in real time, one burst per AC-3 frame duration.
        while (!sink.submit(*burst)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    // Let the queue drain before tearing the endpoint down.
    while (sink.stats().bursts_rendered < sink.stats().bursts_submitted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    std::println("submitted {} bursts, rendered {}, {} underruns", stats.bursts_submitted,
                 stats.bursts_rendered, stats.underruns);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() >= 2 && std::string_view{args[1]} == "devices") {
        return run_devices();
    }
    if (args.size() >= 2 && std::string_view{args[1]} == "outputs") {
        return run_outputs();
    }
    if (args.size() < 3) {
        print_usage();
        return args.size() < 2 ? 0 : 1;
    }
    const std::string_view command{args[1]};
    if (command == "record") {
        return run_record(args[2], args.size() > 3 ? parse_u32_or(args[3], 5) : 5,
                          args.size() > 4 ? parse_u32_or(args[4], 192) : 192,
                          args.size() > 5 ? static_cast<int>(parse_u32_or(args[5], 0)) : 0);
    }
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
        return run_encode(args[2], args[3], args.size() > 4 ? parse_u32_or(args[4], 192) : 192,
                          args.size() > 5 && std::string_view{args[5]} == "couple");
    }
    if (command == "decode" && args.size() > 3) {
        return run_decode(args[2], args[3]);
    }
    if (command == "spdif" && args.size() > 3) {
        return run_spdif(args[2], args[3]);
    }
    if (command == "play") {
        return run_play(args[2],
                        args.size() > 3 ? static_cast<int>(parse_u32_or(args[3], 0)) : -1);
    }
    print_usage();
    return 1;
}
