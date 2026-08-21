#include "live_audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/audio/audio_backend.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/resampler.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "matroska/matroska.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

int run_monitor(std::string_view in_path, int device_index, const Options& meta) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    if (!apply_object_verification(stream, meta)) {
        return 1;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        std::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    const bool eac3 = *bsid > 8;

    std::string device_id;
    std::string device_name = "default endpoint";
    if (device_index >= 0) {
        const auto devices = ac3::audio::enumerate_render_devices();
        if (!devices) {
            std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
            return 1;
        }
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            std::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return 1;
        }
        device_id = (*devices)[static_cast<std::size_t>(device_index)].id;
        device_name = (*devices)[static_cast<std::size_t>(device_index)].name;
    }

    ac3::audio::MonitorSink sink;
    std::uint64_t units_played = 0;
    auto play = [&](std::span<const float> interleaved) {
        while (!sink.submit(interleaved)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    };

    if (eac3) {
        const auto units = ac3::split_access_units(stream);
        if (!units || units->empty()) {
            std::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return 1;
        }
        // Heap-allocated (PREfast's C6262, alert #9): Eac3Decoder grew
        // several KB of per-block scratch members (alert #63's fix), which
        // pushed this one-shot stack declaration over the threshold - same
        // pattern as PR #50.
        auto decoder = std::make_unique<ac3::Eac3Decoder>();
        std::vector<std::size_t> order;
        for (const auto& unit : *units) {
            const auto decoded = decoder->decode_access_unit(unit);
            if (!decoded) {
                std::println(stderr, "error: decode failed (code {})",
                             static_cast<int>(decoded.error()));
                return 1;
            }
            if (!decoded->has_value()) {
                // §3.7: held back pending transient pre-noise processing
                // (Eac3Decoder::decode_access_unit's own doc comment) - live
                // monitoring just waits for the next unit to catch up rather
                // than draining decoder.flush() mid-stream.
                continue;
            }
            const auto& out = **decoded;
            if (order.empty()) {
                // Dual mono has no Table E2.5 location to order by - `layout`
                // is left empty for exactly that case - so Ch1/Ch2 monitor in
                // coded order, same as everywhere else this comes up.
                if (out.acmod == ac3::Acmod::kDualMono) {
                    order.resize(out.channels.size());
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        order[i] = i;
                    }
                } else {
                    order = plan::wav_order(std::span{out.layout.items}.first(
                        static_cast<std::size_t>(out.layout.count)));
                }
                const auto started = sink.start(device_id, sample_rate_hz(out.sample_rate),
                                                static_cast<std::uint16_t>(order.size()));
                if (!started) {
                    std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
                    return 1;
                }
                std::println("monitoring {} ({} channels, {} Hz) on \"{}\"…", in_path,
                             order.size(), sample_rate_hz(out.sample_rate), device_name);
                // Same object-count line run_decode_eac3 reports (see
                // report_decoded_objects) - this path still only plays the
                // 5.1 bed (this function's own header comment), so it says
                // so rather than implying object playback is coming.
                if (out.object_metadata) {
                    // Guarded by the if above; clang-tidy's
                    // bugprone-unchecked-optional-access doesn't trace the
                    // guard through into a multi-argument std::println call,
                    // the same false positive print_channel_summary(*meter)
                    // elsewhere in this file works around - binding once here
                    // instead of repeating out.object_metadata-> twice sidesteps it.
                    const auto& metadata = *out.object_metadata;  // NOLINT(bugprone-unchecked-optional-access)
                    std::println(
                        "  {} dynamic objects + the bed's LFE = {} objects, OAMD present{}",
                        metadata.objects.size(), ac3::oba::object_count(metadata.program),
                        out.object_audio.empty()
                            ? " (JOC audio not reconstructed)"
                            : ", JOC audio reconstructed (bed-only playback here; see "
                              "'ac3cli decode' with objects_dir to export it)");
                }
            }
            play(interleave_reordered(out.channels, order));
            ++units_played;
        }
    } else {
        const auto frames = ac3::split_frames(stream);
        if (!frames || frames->empty()) {
            std::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return 1;
        }
        ac3::FrameDecoder decoder;
        std::vector<std::size_t> order;
        for (const auto& frame : *frames) {
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded) {
                std::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                return 1;
            }
            if (order.empty()) {
                order = ac3::io::wav_channel_order(decoded->acmod, decoded->lfe);
                const auto started = sink.start(device_id, sample_rate_hz(decoded->sample_rate),
                                                static_cast<std::uint16_t>(order.size()));
                if (!started) {
                    std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
                    return 1;
                }
                std::println("monitoring {} ({} channels, {} Hz) on \"{}\"…", in_path,
                             order.size(), sample_rate_hz(decoded->sample_rate), device_name);
            }
            play(interleave_reordered(decoded->channels, order));
            ++units_played;
        }
    }

    while (sink.stats().frames_rendered < sink.stats().frames_submitted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    std::println("played {} {}, {} underruns", units_played, eac3 ? "access units" : "frames",
                 stats.underruns);
    return 0;
}

int run_live(std::string_view out_path, int capture_device, std::uint32_t seconds,
            std::uint32_t bitrate, int monitor_device, int passthrough_device,
            std::string_view mode, const Options& meta) {
    if (mode != "channels" && mode != "atmos") {
        std::println(stderr, "error: mode is 'channels' (default) or 'atmos'");
        return 1;
    }
    const bool atmos = mode == "atmos";

    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return 1;
    }
    if (capture_device < 0 || static_cast<std::size_t>(capture_device) >= devices->size()) {
        std::println(stderr, "error: capture device index {} out of range (see 'ac3cli devices')",
                     capture_device);
        return 1;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(capture_device)];

    // capture2=: a second, independently-clocked device. Range-checked the
    // same way as the master above - a bad index refuses the whole command
    // rather than silently falling back to a single-device session.
    if (meta.capture2 && (*meta.capture2 < 0 ||
                          static_cast<std::size_t>(*meta.capture2) >= devices->size())) {
        std::println(stderr,
                     "error: capture2 device index {} out of range (see 'ac3cli devices')",
                     *meta.capture2);
        return 1;
    }
    const ac3::audio::DeviceInfo* device2 =
        meta.capture2 ? &(*devices)[static_cast<std::size_t>(*meta.capture2)] : nullptr;

    ac3::SampleRate sr{};
    switch (device.sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            std::println(stderr,
                         "error: \"{}\" runs at {} Hz; AC-3/E-AC-3 need 32, 44.1 or 48 kHz",
                         device.name, device.sample_rate);
            return 1;
    }

    // capture2's own rate only has to be a legal AC-3 rate itself - it does
    // NOT need to match the master's, since the resampler's nominal-
    // conversion side is exactly what absorbs a 44.1/48 kHz mismatch between
    // the two devices.
    double nominal_ratio = 1.0;
    if (device2) {
        switch (device2->sample_rate) {
            case 48000:
            case 44100:
            case 32000: break;
            default:
                std::println(stderr,
                             "error: capture2 \"{}\" runs at {} Hz; AC-3/E-AC-3 need 32, 44.1 "
                             "or 48 kHz",
                             device2->name, device2->sample_rate);
                return 1;
        }
        nominal_ratio =
            static_cast<double>(device.sample_rate) / static_cast<double>(device2->sample_rate);
    }

    ac3::audio::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        std::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return 1;
    }
    const auto channels = capture.channels();
    const auto rate_hz = capture.sample_rate();

    // Clock-master model: capture paces the session exactly as before -
    // nothing about its own timing changes below. capture2, when present, is
    // a second, independently-clocked device whose stream gets resampled
    // into lockstep with capture's pacing every frame, then appended after
    // capture's own channels.
    ac3::audio::Capture capture2;
    std::size_t capture2_channels = 0;
    std::optional<ac3::audio::DriftResampler> slave_resampler;
    std::optional<ac3::audio::ClockDriftEstimator> slave_drift;
    std::vector<float> slave_scratch;
    std::size_t slave_scratch_valid_frames = 0;
    std::vector<float> slave_out;
    if (device2) {
        const auto started2 = capture2.start(device2->id, device2->kind);
        if (!started2) {
            std::println(stderr, "error: {}", ac3::audio::describe(started2.error()));
            return 1;
        }
        capture2_channels = capture2.channels();
        slave_resampler.emplace(capture2_channels);
        slave_drift.emplace(nominal_ratio, static_cast<std::size_t>(ac3::kSamplesPerFrame));
        slave_scratch.resize(4 * static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                             capture2_channels);
        slave_out.resize(static_cast<std::size_t>(ac3::kSamplesPerFrame) * capture2_channels);
        slave_resampler->reset();
        std::println("capture2: \"{}\", {} ch @ {} Hz (nominal ratio {:.6f})", device2->name,
                     device2->channels, device2->sample_rate, nominal_ratio);
    }

    // Object mode pans every captured channel into the 5.1 bed as its own
    // object (mirrors encodeObjects/run_atmos_encode); channel mode carries
    // the first two channels straight through as AC-3 stereo (mirrors
    // run_record, which this supersedes for anything wanting monitor or
    // passthrough alongside the file). capture2's channels, once resampled
    // into lockstep, widen this the same way an extra source channel would -
    // capture's own channels keep their existing indices, the slave's land
    // at the new, higher ones.
    const std::size_t total_channels = static_cast<std::size_t>(channels) + capture2_channels;
    const std::size_t nobjects = atmos ? std::min<std::size_t>(total_channels, 15) : 2;

    auto resolve_render_device = [&](int index) -> std::optional<ac3::audio::RenderDeviceInfo> {
        if (index < 0) {
            return ac3::audio::RenderDeviceInfo{};  // empty id: default endpoint
        }
        const auto render_devices = ac3::audio::enumerate_render_devices(rate_hz);
        if (!render_devices || static_cast<std::size_t>(index) >= render_devices->size()) {
            return std::nullopt;
        }
        return (*render_devices)[static_cast<std::size_t>(index)];
    };

    ac3::audio::MonitorSink monitor_sink;
    bool monitoring = false;
    if (monitor_device != -2) {
        const auto target = resolve_render_device(monitor_device);
        if (!target) {
            std::println(stderr, "warning: monitor device index {} out of range; monitoring off",
                         monitor_device);
        } else {
            const auto mstarted = monitor_sink.start(
                target->id, rate_hz, static_cast<std::uint16_t>(atmos ? 6 : 2));
            if (!mstarted) {
                std::println(stderr, "warning: monitor unavailable: {}",
                             ac3::audio::describe(mstarted.error()));
            } else {
                monitoring = true;
                std::println("monitoring on \"{}\"", target->name.empty() ? "default endpoint"
                                                                          : target->name);
            }
        }
    }

    ac3::audio::PassthroughSink passthrough_sink;
    bool passing_through = false;
    if (passthrough_device != -2) {
        const auto target = resolve_render_device(passthrough_device);
        const auto format =
            atmos ? ac3::audio::BitstreamFormat::kEac3 : ac3::audio::BitstreamFormat::kAc3;
        if (!target) {
            std::println(stderr,
                         "warning: passthrough device index {} out of range; passthrough off",
                         passthrough_device);
        } else if (target->id.empty() ? false
                                      : (atmos ? !target->supports_eac3_passthrough
                                              : !target->supports_ac3_passthrough)) {
            std::println(stderr, "warning: \"{}\" does not accept {} over IEC 61937; "
                                 "passthrough off",
                         target->name, atmos ? "E-AC-3" : "AC-3");
        } else {
            const auto pstarted = passthrough_sink.start(target->id, rate_hz, format);
            if (!pstarted) {
                std::println(stderr, "warning: passthrough unavailable: {}",
                             ac3::audio::describe(pstarted.error()));
            } else {
                passing_through = true;
                std::println("passthrough ({}) on \"{}\"", atmos ? "E-AC-3" : "AC-3",
                             target->name.empty() ? "default endpoint" : target->name);
            }
        }
    }

    // Heap-allocated: each carries several KB of MDCT/delay history state,
    // and this function only constructs them once, at session start, not per
    // audio frame (PREfast's C6262) - same pattern as EncoderController's
    // runLiveSession, the GUI's equivalent of this function.
    auto ac3_encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
        .sample_rate = sr, .bitrate_kbps = bitrate, .fast_mdct = meta.fast_mdct});
    std::unique_ptr<ac3::oba::AtmosEncoder> atmos_encoder;
    if (atmos) {
        atmos_encoder = std::make_unique<ac3::oba::AtmosEncoder>(
            ac3::oba::AtmosConfig{.sample_rate = sr, .bitrate_kbps = bitrate,
                                  .num_bands_idx = 4, .fast_mdct = meta.fast_mdct},
            static_cast<int>(nobjects));
    }
    auto ac3_monitor_decoder = std::make_unique<ac3::FrameDecoder>();
    ac3::Eac3Decoder eac3_monitor_decoder;
    ac3::iec61937::Eac3BurstPacker eac3_packer;

    // Object mode meters the 5.1 bed (matching encodeObjects/run_atmos_encode
    // - what a legacy decoder hears); channel mode meters plain stereo
    // (matching run_record). Getting this wrong doesn't just mislabel a
    // column - the wrong acmod also changes how many channels the meter
    // reports.
    ac3::analysis::LevelMeter meter = atmos
                                          ? ac3::analysis::LevelMeter{ac3::Acmod::k3_2, true, rate_hz}
                                          : ac3::analysis::LevelMeter{ac3::Acmod::k2_0, false, rate_hz};
    const std::uint64_t target_frames =
        (static_cast<std::uint64_t>(seconds) * rate_hz + ac3::kSamplesPerFrame - 1) /
        ac3::kSamplesPerFrame;

    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    std::vector<std::vector<float>> block(nobjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nobjects);
    // Separate from `views`: that vector holds nobjects per-object essence
    // spans for the encoder, which in channel mode is 2 and in object mode
    // can be as few as 1 - either can be narrower than the bed's fixed 6
    // channels metered below, so reusing `views` for both risked (and in an
    // earlier version of this loop, did) an out-of-bounds write past a
    // 2-element vector.
    std::vector<std::span<const float>> bed_views(6);
    std::vector<ac3::oba::ObjectPlacement> placement(nobjects);
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(target_frames));

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < target_frames; ++f) {
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(
                std::span{interleaved}.subspan(filled, interleaved.size() - filled));
            filled += got;
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (slave_resampler.has_value() && slave_drift.has_value()) {
            // Opportunistic, non-blocking drain: whatever capture2 has ready
            // right now joins the scratch FIFO's tail. Unlike the master's
            // read above, this never waits - a slave that is momentarily
            // behind just leaves the resampler's next render() with less to
            // work from, which is exactly the drift the estimator is
            // steering against, not a stall to block the session on.
            //
            // Guarded on slave_resampler/slave_drift's own has_value() rather
            // than device2 (always in lockstep with it by construction, both
            // populated together right after capture2 opens) so clang-tidy's
            // bugprone-unchecked-optional-access can actually see the
            // invariant instead of having to trust a same-lockstep but
            // type-unrelated raw pointer.
            const std::size_t capacity_frames = slave_scratch.size() / capture2_channels;
            const std::size_t free_frames = capacity_frames - slave_scratch_valid_frames;
            if (free_frames > 0) {
                const auto got = capture2.buffer()->read(std::span{slave_scratch}.subspan(
                    slave_scratch_valid_frames * capture2_channels,
                    free_frames * capture2_channels));
                slave_scratch_valid_frames += got / capture2_channels;
            }
            slave_drift->update(slave_scratch_valid_frames);
            slave_resampler->set_ratio(slave_drift->ratio());
            const auto consumed = slave_resampler->render(
                std::span{slave_scratch}.first(slave_scratch_valid_frames * capture2_channels),
                slave_scratch_valid_frames, std::span{slave_out},
                static_cast<std::size_t>(ac3::kSamplesPerFrame));
            const std::size_t remaining_frames = slave_scratch_valid_frames - consumed;
            std::copy(slave_scratch.begin() + static_cast<std::ptrdiff_t>(
                                                   consumed * capture2_channels),
                     slave_scratch.begin() + static_cast<std::ptrdiff_t>(
                                                  slave_scratch_valid_frames * capture2_channels),
                     slave_scratch.begin());
            slave_scratch_valid_frames = remaining_frames;
        }
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::size_t base = static_cast<std::size_t>(i) * channels;
            const std::size_t base2 = static_cast<std::size_t>(i) * capture2_channels;
            for (std::size_t ch = 0; ch < nobjects; ++ch) {
                if (ch < channels) {
                    block[ch][static_cast<std::size_t>(i)] = interleaved[base + ch];
                } else if (ch < total_channels) {
                    block[ch][static_cast<std::size_t>(i)] = slave_out[base2 + (ch - channels)];
                } else {
                    block[ch][static_cast<std::size_t>(i)] = 0.0f;
                }
            }
        }
        for (std::size_t ch = 0; ch < nobjects; ++ch) {
            views[ch] = block[ch];
        }
        n0 += ac3::kSamplesPerFrame;

        std::vector<std::byte> unit_bytes;
        if (atmos) {
            // Objects orbit at their own rate and start spread around the
            // ring, matching run_atmos exactly - the position is recomputed
            // from elapsed time every frame rather than fixed once, which is
            // the whole point: a real live source reads the same way.
            const double t = static_cast<double>(n0) / static_cast<double>(rate_hz);
            for (std::size_t i = 0; i < nobjects; ++i) {
                const double rate =
                    1.0 / (6.0 * (1.0 + 0.31 * static_cast<double>(i)));
                const double phase =
                    2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(nobjects);
                const double angle = 2.0 * std::numbers::pi * rate * t + phase;
                const double height =
                    nobjects == 1 ? 0.5
                                 : -1.0 + 2.0 * static_cast<double>(i) /
                                              static_cast<double>(nobjects - 1);
                placement[i] = {.position = {.x = 0.5 + 0.5 * std::sin(angle),
                                             .y = 0.5 - 0.5 * std::cos(angle),
                                             .z = height},
                                .gain = 0.7 / std::sqrt(static_cast<double>(nobjects)),
                                .lfe_send = i == 0 ? 0.2 : 0.0};
            }
            const auto unit = atmos_encoder->encode_frame(views, placement);
            if (!unit) {
                std::println(stderr, "error: cannot encode {} objects at {} kbps",
                             nobjects, bitrate);
                break;
            }
            for (std::size_t ch = 0; ch < 6; ++ch) {
                bed_views[ch] = std::span{atmos_encoder->bed()[ch]};
            }
            meter.process(bed_views);
            unit_bytes = unit->bytes;
        } else {
            const auto frame = ac3_encoder->encode_frame(std::span{views}.first(2));
            if (!frame) {
                std::println(stderr, "error: bitrate must be a legal AC-3 rate");
                break;
            }
            meter.process(std::span{views}.first(2));
            unit_bytes = *frame;
        }

        if (monitoring) {
            std::optional<std::vector<float>> to_play;
            if (atmos) {
                const auto decoded = eac3_monitor_decoder.decode_access_unit(unit_bytes);
                // §3.7: decoded->has_value() is false exactly when this
                // access unit is being held back pending transient
                // pre-noise processing (decode_access_unit's own doc
                // comment) - live monitoring just waits for the next one.
                if (decoded && decoded->has_value()) {
                    const auto order = plan::wav_order(
                        std::span{(*decoded)->layout.items}.first(
                        static_cast<std::size_t>((*decoded)->layout.count)));
                    to_play = interleave_reordered((*decoded)->channels, order);
                }
            } else {
                const auto decoded = ac3_monitor_decoder->decode_frame(unit_bytes);
                if (decoded) {
                    const auto order = ac3::io::wav_channel_order(decoded->acmod, decoded->lfe);
                    to_play = interleave_reordered(decoded->channels, order);
                }
            }
            if (to_play) {
                while (!monitor_sink.submit(*to_play)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
        }

        if (passing_through) {
            if (atmos) {
                const auto burst = eac3_packer.push(unit_bytes);
                if (burst && *burst) {
                    while (!passthrough_sink.submit(**burst)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
            } else {
                const auto burst = ac3::iec61937::wrap_frame(unit_bytes);
                if (burst) {
                    while (!passthrough_sink.submit(*burst)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
            }
        }

        frames.push_back(std::move(unit_bytes));
        print_live_meter(meter, static_cast<double>(frames.size() * ac3::kSamplesPerFrame) /
                                    rate_hz);
    }
    std::println("");

    capture.stop();
    if (device2) {
        capture2.stop();
    }
    if (monitoring) {
        while (monitor_sink.stats().frames_rendered < monitor_sink.stats().frames_submitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        monitor_sink.stop();
    }
    if (passing_through) {
        while (passthrough_sink.stats().bursts_rendered <
               passthrough_sink.stats().bursts_submitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto pstats = passthrough_sink.stats();
        passthrough_sink.stop();
        std::println("passthrough: {} bursts submitted, {} rendered, {} underruns",
                     pstats.bursts_submitted, pstats.bursts_rendered, pstats.underruns);
    }
    const auto stats = capture.stats();
    // Object mode's unit_bytes are the 5.1-bed access unit (matching what a
    // legacy decoder hears, same as the meter above); channel mode is always
    // plain 2-channel AC-3 (encode_frame above only ever sees
    // views.first(2)) - the same track shape 'mkv' would derive by scanning
    // this file back, just already known here.
    const matroska::AudioTrack track{
        .codec_id = std::string{atmos ? matroska::kCodecEac3 : matroska::kCodecAc3},
        .sample_rate = rate_hz,
        .channels = atmos ? 6 : 2,
        .samples_per_frame = ac3::kSamplesPerFrame};
    if (!write_frames_or_mux(out_path, meta.matroska_container, track, frames)) {
        return 1;
    }
    std::println("wrote {} {} ({} kbps) to {}{}", frames.size(),
                 atmos ? "E-AC-3 access units" : "AC-3 frames", bitrate, out_path,
                 meta.matroska_container ? " (Matroska)" : "");
    std::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
    if (slave_drift.has_value()) {
        std::println("capture2 drift: {:+.1f} ppm", slave_drift->drift_ppm());
    }
    print_channel_summary(meter);
    return 0;
}

}  // namespace ac3cli::commands
