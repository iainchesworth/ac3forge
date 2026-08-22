#include "audio_io.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "matroska/matroska.hpp"

namespace ac3cli::commands {

int run_devices() {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
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
                     d.kind == ac3::audio::DeviceKind::kInput ? "input" : "loopback",
                     d.sample_rate, d.channels, d.name, d.is_default ? "  [default]" : "");
    }
    return 0;
}

int run_record(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
               int device_index, const Options& meta) {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
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

    ac3::audio::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return 1;
    }
    const auto channels = capture.channels();
    std::println("recording from \"{}\" ({} Hz, {} ch) for {} s…", device.name,
                 capture.sample_rate(), channels, seconds);

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
        .sample_rate = sr, .bitrate_kbps = bitrate, .fast_mdct = meta.fast_mdct});
    // Meters what the encoder is fed, not what the endpoint delivers: a
    // needle that moves on a channel the stream never carries would be a lie.
    ac3::analysis::LevelMeter meter{ac3::Acmod::k2_0, false, capture.sample_rate()};
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
        meter.process(views);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
        // One frame is 32 ms at 48 kHz, so the meter redraws about 30 times a
        // second without any throttling of its own.
        print_live_meter(meter, static_cast<double>(frames.size() * ac3::kSamplesPerFrame) /
                                    capture.sample_rate());
    }
    std::println("");

    capture.stop();
    const auto stats = capture.stats();
    // record is always plain AC-3 stereo (see the deinterleave above, which
    // only ever fills `planar`'s two channels) - the same track shape 'mkv'
    // would derive by scanning this file back, just already known here.
    const matroska::AudioTrack track{.codec_id = std::string{matroska::kCodecAc3},
                                     .sample_rate = capture.sample_rate(),
                                     .channels = 2,
                                     .samples_per_frame = ac3::kSamplesPerFrame};
    if (!write_frames_or_mux(out_path, meta.matroska_container, track, frames)) {
        return 1;
    }
    std::println("wrote {} frames ({} kbps) to {}{}", frames.size(), bitrate, out_path,
                 meta.matroska_container ? " (Matroska)" : "");
    std::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
    print_channel_summary(meter);
    return 0;
}

int run_outputs() {
    const auto devices = ac3::audio::enumerate_render_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
        std::println("no active render endpoints found");
        return 0;
    }
    std::println("{:>3}  {:<9}  {:<9}  {:<9}  {}", "idx", "AC-3", "E-AC-3", "excl PCM", "name");
    for (std::size_t i = 0; i < devices->size(); ++i) {
        const auto& d = (*devices)[i];
        std::println("{:>3}  {:<9}  {:<9}  {:<9}  {}{}", i, d.supports_ac3_passthrough ? "yes" : "no",
                     d.supports_eac3_passthrough ? "yes" : "no",
                     d.supports_exclusive_pcm ? "yes" : "no", d.name,
                     d.is_default ? "  [default]" : "");
    }
    std::println("");
    std::println("AC-3     the endpoint accepted an IEC 61937 AC-3 format in exclusive mode.");
    std::println("E-AC-3   the same, for Dolby Digital Plus (and Atmos riding inside it - there");
    std::println("         is no separate passthrough format for Atmos).");
    std::println("excl PCM the same endpoint accepted ordinary 16-bit stereo PCM exclusively.");
    std::println("");
    std::println("PCM yes + AC-3/E-AC-3 no means the device simply cannot bitstream - analog");
    std::println("outputs cannot; only S/PDIF (TOSLINK/coax) and HDMI can. Enable Dolby Digital");
    std::println("under Sound > Playback > Properties > Supported Formats for such a device.");
    std::println("All no means exclusive mode itself is unavailable (disabled for the device,");
    std::println("or another application currently holds it).");
    return 0;
}

int run_play(std::string_view in_path, int device_index) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        std::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    const bool eac3 = *bsid > 8;

    std::vector<std::span<const std::byte>> units;
    std::uint32_t content_rate = 0;
    if (eac3) {
        const auto split = ac3::split_access_units(stream);
        if (!split || split->empty()) {
            std::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return 1;
        }
        units = *split;
        content_rate =
            sample_rate_hz(static_cast<ac3::SampleRate>(
                std::to_integer<std::uint32_t>(units[0][4]) >> 6));
    } else {
        const auto split = ac3::split_frames(stream);
        if (!split || split->empty()) {
            std::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return 1;
        }
        units = *split;
        content_rate =
            sample_rate_hz(static_cast<ac3::SampleRate>(
                std::to_integer<std::uint32_t>(units[0][4]) >> 6));
    }

    const auto devices = ac3::audio::enumerate_render_devices(content_rate);
    // Enumeration failing and enumeration finding nothing are different
    // answers: the first is the backend saying it could not look, the second
    // is it looking and seeing no endpoints. Reporting both as "none
    // available" sent people hunting for a missing sound device when the real
    // answer was a COM failure.
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (devices->empty()) {
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
        const bool supported =
            eac3 ? chosen.supports_eac3_passthrough : chosen.supports_ac3_passthrough;
        if (!supported) {
            std::println(stderr,
                         "error: \"{}\" does not accept {} over IEC 61937 (see 'ac3cli outputs')",
                         chosen.name, eac3 ? "E-AC-3" : "AC-3");
            return 1;
        }
    }

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(
        device_id, content_rate,
        eac3 ? ac3::audio::BitstreamFormat::kEac3 : ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return 1;
    }
    std::println("streaming {} {} to \"{}\" ({} Hz{})…", units.size(),
                 eac3 ? "access units" : "frames", device_name, content_rate,
                 eac3 ? ", carrier 4x that" : " carrier");

    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        std::vector<std::byte> burst;
        if (eac3) {
            auto result = eac3_packer.push(unit);
            if (!result) {
                std::println(stderr, "error: burst wrap failed");
                return 1;
            }
            if (!*result) {
                continue;  // accumulating; nothing to submit yet
            }
            burst = std::move(**result);
        } else {
            const auto wrapped = ac3::iec61937::wrap_frame(unit);
            if (!wrapped) {
                std::println(stderr, "error: burst wrap failed");
                return 1;
            }
            burst = *wrapped;
        }
        // Wait for room rather than racing ahead: the render thread consumes
        // in real time, one burst per burst period.
        while (!sink.submit(burst)) {
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

}  // namespace ac3cli::commands
