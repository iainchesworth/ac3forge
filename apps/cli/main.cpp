#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/resampler.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3/audio/audio_backend.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/spatial/spatial.hpp"
#include "ac3/version.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "matroska/matroska.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "mpegts/mpegts.hpp"
#include "platform/stdio_binary.hpp"
#include "adm/atmos_adm.hpp"
#include "multi_source.hpp"
#include "support.hpp"

namespace {

namespace plan = ac3::plan;

using namespace ac3cli;

void print_usage();


int run_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate) {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = bitrate});
    if (!frame) {
        std::println(stderr, "error: bitrate must be one of the 19 legal AC-3 rates");
        return 1;
    }
    const std::uint64_t count = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    if (!write_repeated_frame(out_path, *frame, count)) {
        return 1;
    }
    std::println("wrote {} silent frames to {}", count, out_path);
    return 0;
}

// One tone per CODED channel, for the synthetic generators. The frequencies
// are deliberately spread and deliberately not harmonics of one another, so a
// channel that lands in the wrong speaker is measurable rather than merely
// suspected. One tone per CODED channel (coded_channels().size(), not the
// smaller rendered count) - a bed channel a dependent replaces still needs
// its own frequency, or nothing here could tell §E3.8.2's overwrite
// happening apart from the dependent being ignored altogether. The specific
// numbers mean nothing beyond being far enough apart to tell channels apart
// by ear; test_eac3_decoder.cpp's per-layout round trips pick their own
// frequencies independently rather than mirroring these.
std::vector<double> layout_tones(const plan::ChannelPlan& cp) {
    const auto coded = plan::coded_channels(cp);
    std::vector<double> out(coded.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 200.0 + 137.0 * static_cast<double>(i);
    }
    return out;
}

// Fills one frame of tones and hands back views onto it.
void fill_tones(std::vector<std::vector<float>>& samples,
                std::vector<std::span<const float>>& views,
                std::span<const double> tone_hz, double amplitude, std::uint64_t n0) {
    for (std::size_t ch = 0; ch < samples.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            samples[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * tone_hz[ch] *
                                     static_cast<double>(n0 + static_cast<std::uint64_t>(i)) /
                                     48000.0));
        }
        views[ch] = samples[ch];
    }
}

// Frames that cover `seconds` of audio, rounded up.
std::uint64_t frame_count(std::uint32_t seconds) {
    return (static_cast<std::uint64_t>(seconds) * 48000 + ac3::kSamplesPerFrame - 1) /
           ac3::kSamplesPerFrame;
}


int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
             bool couple_flag, const Options& meta) {
    // A layout may be suffixed with "c" to turn channel coupling on (51c). A
    // bare 'couple' token does the same, so the flag that works for 'encode'
    // is not silently ignored here.
    const bool couple = couple_flag || (!layout.empty() && layout.back() == 'c');
    const std::string_view base = couple ? layout.substr(0, layout.size() - 1) : layout;

    plan::Plan p{.codec = plan::Codec::kAc3, .bitrate_kbps = bitrate, .meta = meta.p};
    std::string label;
    if (!resolve_layout(base, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    const auto config = plan::ac3_config(p);
    const auto cp = plan::resolve(p);

    // A one- or two-channel layout is the frequency-sweep case the freq_hz
    // argument exists for; anything wider gets a tone per speaker instead,
    // because one frequency in six channels cannot show where it ended up.
    auto tone_hz = layout_tones(cp);
    if (plan::rendered_channel_count(cp) <= 2) {
        std::ranges::fill(tone_hz, static_cast<double>(freq_hz));
    }

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    const auto nchans = static_cast<std::size_t>(encoder->channel_count());
    const double amplitude = amplitude_pct / 100.0;
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, 48000};

    const std::uint64_t count = frame_count(seconds);
    std::vector<std::vector<float>> samples(nchans,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(count));
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
        fill_tones(samples, views, tone_hz, amplitude, n0);
        n0 += ac3::kSamplesPerFrame;
        meter.process(views);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} {} frames ({} kbps) to {}", count, label, bitrate, out_path);
    print_channel_summary(meter);
    return 0;
}

// The same tone generator as run_sine, but through the E-AC-3 container.
// Real audio is the only input that can detect a frame-layout error at all:
// with silence every bap is zero, so a stray bit lands in zero-filled aux
// data and the frame still "decodes".
// Layouts wider than 5.1 need a dependent substream: the independent one
// always carries a self-sufficient 5.1 bed, and the extra channels ride along
// beside it with a chanmap saying where they belong.
// The layout table itself is ac3::plan's; what survives here is the note about
// which of its entries can be checked against anything.
//
// 7.1.4 is spec-correct and rejected by FFmpeg, which refuses any frame with
// substreamid != 0 in ff_ac3_parse_header - it implements the TS 102 366
// Annex J profile, where a stream "shall" hold at most one dependent, numbered
// 0. Every real delivery path caps it there and ships 7.1.4 as Atmos objects
// instead, so that layout's only oracle is the in-repo decoder.

int run_mkv(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    // Everything the container needs to declare comes out of the bitstream:
    // the format, the access-unit boundaries, the sample rate and the channel
    // count. This used to take a layout argument to learn the channel count,
    // which meant a wrong one silently produced a file that misdescribed
    // itself - and nothing could catch it.
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    std::vector<std::vector<std::byte>> units;
    units.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        units.emplace_back(unit.begin(), unit.end());
    }

    const matroska::AudioTrack track{
        .codec_id = std::string{eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame};
    const auto file = matroska::mux(track, units);
    if (!file) {
        std::println(stderr, "error: {}", matroska::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return 1;
    }
    // Name the layout only when one substream carries the whole thing. With
    // dependents the acmod describes the BED, so printing it beside a wider
    // rendered channel count would just contradict itself.
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    std::println("wrote {} {} access units ({}, {} channels, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path);
    return 0;
}

// Same shape as run_mkv above, wrapping mp4::mux() instead: ac3::io::scan()
// still supplies everything the container needs to declare, and additionally
// - via ac3::io::build_codec_config_box() - the exact dac3/dec3 sample-entry
// payload (fscod/bsid/bsmod/acmod/lfeon and, when the stream carries Dolby
// Atmos objects, the flag_ec3_extension_type_a/complexity_index_type_a
// extension) straight off the bitstream. mp4::mux() never sees any of that
// syntax itself - see src/mp4/include/mp4/mp4.hpp's own header comment.
int run_mp4(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    std::vector<std::vector<std::byte>> units;
    units.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        units.emplace_back(unit.begin(), unit.end());
    }

    const mp4::AudioTrack track{
        .codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
        .codec_config = ac3::io::build_codec_config_box(*scanned)};
    const auto file = mp4::mux(track, units);
    if (!file) {
        std::println(stderr, "error: {}", mp4::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return 1;
    }
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    const std::string atmos =
        scanned->oba_complexity_index
            ? std::format(", Atmos complexity {}", *scanned->oba_complexity_index)
            : std::string{};
    std::println("wrote {} {} access units ({}, {} channels{}, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels, atmos,
                 file->size(), out_path);
    return 0;
}

bool write_bytes_to_path(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", path.string());
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::println(stderr, "error: write failed for {}", path.string());
        return false;
    }
    return true;
}

bool write_text_to_path(const std::filesystem::path& path, std::string_view text) {
    return write_bytes_to_path(
        path, std::as_bytes(std::span{reinterpret_cast<const char*>(text.data()), text.size()}));
}

// A minimal but complete DASH MPD document wrapped around
// mp4::build_dash_adaptation_set()'s <AdaptationSet> snippet - the library
// stops at the snippet (mp4.hpp/dash.hpp's own scope: single-representation
// audio, no opinion on the surrounding document), the CLI front end supplies
// the rest, the same boundary mp4::mux() not doing file I/O already draws.
// profiles="isoff-live" is what a SegmentTemplate-based MPD declares
// regardless of static/live (ISO/IEC 23009-1 Annex A.3) - "isoff-on-demand"
// instead mandates a single SegmentBase/index-range layout this module does
// not produce.
std::string build_dash_mpd(const mp4::AudioTrack& track,
                           std::span<const mp4::MediaSegment> segments,
                           std::string_view adaptation_set) {
    std::uint64_t total_samples = 0;
    for (const auto& segment : segments) {
        total_samples += segment.duration_samples;
    }
    const double total_seconds =
        static_cast<double>(total_samples) / static_cast<double>(track.sample_rate);
    return std::format(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        "mediaPresentationDuration=\"PT{:.3f}S\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period>\n"
        "{}"
        "  </Period>\n"
        "</MPD>\n",
        total_seconds, adaptation_set);
}

// fMP4/CMAF segmenting plus HLS/DASH signaling helpers (ROADMAP.md's A2) -
// the streaming-delivery follow-up 'mp4' (run_mp4 above) deliberately left
// for later. Same source (ac3::io::scan) as run_mp4/run_mkv for everything
// the container needs to declare; the only new wrinkle is that this writes a
// DIRECTORY of files (an init segment, one media segment per fragment, an
// HLS media+master playlist pair, and a DASH MPD) rather than one file, so a
// packager or CDN origin can be pointed at out_dir directly.
int run_fmp4(std::string_view in_path, std::string_view out_dir,
             std::uint32_t frames_per_fragment) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    std::vector<std::vector<std::byte>> units;
    units.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        units.emplace_back(unit.begin(), unit.end());
    }

    const mp4::AudioTrack track{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                                .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                                .channels = scanned->channels,
                                .samples_per_frame = ac3::kSamplesPerFrame,
                                .codec_config = ac3::io::build_codec_config_box(*scanned)};

    const auto fragmented = mp4::fragment(
        track, units, mp4::FragmentOptions{.frames_per_fragment = frames_per_fragment});
    if (!fragmented) {
        std::println(stderr, "error: {}", mp4::describe(fragmented.error()));
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path dir{std::string{out_dir}};
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::println(stderr, "error: cannot create directory {} ({})", out_dir, ec.message());
        return 1;
    }

    if (!write_bytes_to_path(dir / "init.mp4", fragmented->init_segment)) {
        return 1;
    }
    for (const auto& segment : fragmented->media_segments) {
        const auto name = std::format("segment{}.m4s", segment.sequence_number);
        if (!write_bytes_to_path(dir / name, segment.bytes)) {
            return 1;
        }
    }

    // Dolby Digital Plus with Atmos objects needs CHANNELS="<N>/JOC" instead
    // of a plain channel count (see mp4/hls.hpp's own citations) - N is the
    // same decodable-object count ac3::io::scan already read off the
    // bitstream to build the dec3 box above (TS 103 420
    // §8.3.2's complexity_index_type_a). mp4:: itself never reads that
    // field; only this CLI front end, which already has it, does.
    const mp4::HlsOptions hls_options{
        .channels_attribute = scanned->oba_complexity_index
                                  ? std::format("{}/JOC", *scanned->oba_complexity_index)
                                  : std::string{}};
    const auto media_playlist =
        mp4::build_hls_media_playlist(track, fragmented->media_segments, hls_options);
    const auto master_playlist = mp4::build_hls_master_playlist(track, fragmented->media_segments,
                                                                "audio.m3u8", hls_options);
    if (!write_text_to_path(dir / "audio.m3u8", media_playlist) ||
        !write_text_to_path(dir / "master.m3u8", master_playlist)) {
        return 1;
    }

    const auto adaptation_set = mp4::build_dash_adaptation_set(track, fragmented->media_segments);
    const auto mpd = build_dash_mpd(track, fragmented->media_segments, adaptation_set);
    if (!write_text_to_path(dir / "manifest.mpd", mpd)) {
        return 1;
    }

    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    const std::string atmos =
        scanned->oba_complexity_index
            ? std::format(", Atmos complexity {}", *scanned->oba_complexity_index)
            : std::string{};
    std::println(
        "wrote {} {} access units ({}, {} channels{}) as {} fragment(s) to {} "
        "(init.mp4, segment*.m4s, audio.m3u8, master.m3u8, manifest.mpd)",
        units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels, atmos,
        fragmented->media_segments.size(), out_dir);
    return 0;
}

// Same shape as run_mkv above: everything mpegts::mux needs comes off the
// bitstream via ac3::io::scan, not from a caller-supplied layout that could
// disagree with what is actually in the file. See mpegts/mpegts.hpp's header
// comment for the broadcast profile this wraps as (DVB stream_type 0x06 plus
// the AC3_descriptor/Enhanced_AC3_descriptor ETSI EN 300 468 Annex D defines)
// and why.
int run_ts(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    std::vector<std::vector<std::byte>> units;
    units.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        units.emplace_back(unit.begin(), unit.end());
    }

    const mpegts::AudioTrack track{
        .codec = eac3 ? mpegts::AudioCodec::kEac3 : mpegts::AudioCodec::kAc3,
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame};
    const auto file = mpegts::mux(track, units);
    if (!file) {
        std::println(stderr, "error: {}", mpegts::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return 1;
    }
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    std::println("wrote {} {} access units ({}, {} channels, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path);
    return 0;
}

int run_eac3_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                  std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                  const Options& meta) {
    plan::Plan p{.codec = plan::Codec::kEac3, .bitrate_kbps = bitrate, .meta = meta.p};
    std::string label;
    if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }
    // No [tools] positional here (unlike eac3-encode), so fast-mdct=off is
    // this command's only way to reach it - same field, same meaning as
    // 'sine'/'encode's identical assignment.
    p.tools.fast_mdct = meta.fast_mdct;
    const auto config = plan::eac3_config(p);
    const auto cp = plan::resolve(p);

    auto tone_hz = layout_tones(cp);
    if (plan::rendered_channel_count(cp) <= 2) {
        std::ranges::fill(tone_hz, static_cast<double>(freq_hz));
    }
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    assert(nchans == tone_hz.size());
    const double amplitude = amplitude_pct / 100.0;

    const std::uint64_t count = frame_count(seconds);
    std::vector<std::vector<float>> samples(nchans,
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(static_cast<std::size_t>(count));
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < count; ++f) {
        fill_tones(samples, views, tone_hz, amplitude, n0);
        n0 += ac3::kSamplesPerFrame;
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: invalid E-AC-3 configuration");
            return 1;
        }
        frames.push_back(std::move(unit->bytes));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} E-AC-3 {} access units ({} coded channels, {} substreams, "
                 "bsid 16) to {}",
                 count, label, nchans, config.dependents.size() + 1, out_path);
    return 0;
}

// Reports a bad tool token against the syntax, the same way a bad layout is
// reported against the layout list.
bool tools_or_error(std::string_view text, plan::Tools& out) {
    if (plan::parse_tools(text, out)) {
        return true;
    }
    std::println(stderr, "error: unknown tool set '{}' ({})", text, plan::kToolsSyntax);
    return false;
}

// As above, for the vbr argument.
bool vbr_or_error(std::string_view text, std::optional<ac3::eac3::VbrConfig>& out) {
    if (plan::parse_vbr(text, out)) {
        return true;
    }
    std::println(stderr, "error: unrecognised vbr setting '{}' ({})", text, plan::kVbrSyntax);
    return false;
}

// Real program material through the E-AC-3 path. The tone generators above
// exercise field placement; only recorded-style material exercises the coding
// decisions, which is what the Annex E tools are judged on.
// The same encode as run_eac3_encode below, but for a possibly multi-source
// run (src=/map= given). dialnorm=auto/dialnorm2=auto measure the RENDERED
// bed/programme content (post map=/routing, in coded-channel order) rather
// than raw per-source channels, since which channel is "L"/"Ls" - or, for
// 1+1, which is Ch1/Ch2 - depends on the assignment rather than file order.
// So the whole programme is routed once as a measurement pre-pass before the
// loop below routes it again to actually encode it - see route_frame's own
// comment for why sharing that one lambda keeps the two passes from ever
// disagreeing about what "routed" means.
int run_eac3_encode_multi(std::string_view in_path, std::string_view out_path,
                          std::uint32_t bitrate, std::string_view tools,
                          std::string_view layout, std::string_view vbr, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources) {
        return 1;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kEac3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        std::size_t total_channels = 0;
        for (const auto& shape : sources->shapes) {
            total_channels += shape.channels;
        }
        const auto id = plan::layout_for_source(total_channels);
        if (!id) {
            std::println(stderr, "error: {} channels - {}", total_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return 1;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing) {
        return 1;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // Renders one frame's worth of every source's samples onto the coded
    // channels `out`/`views` alias - shared by the measurement pre-pass below
    // and the real encode loop after it, so the two can never render this
    // programme two different ways.
    auto route_frame = [&](std::size_t start) {
        gather_frame(*sources, start, source);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
    };

    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    const bool want_dialnorm = p.meta.measure_dialnorm;
    // dialnorm2 only means anything under 1+1 - silently inert otherwise,
    // exactly like run_eac3_encode's identical check for its one file.
    const bool want_dialnorm2 = dual_mono && p.meta.measure_dialnorm2;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (want_dialnorm || want_dialnorm2) {
        // §5.4.2.8's BS.1770 pass has to measure what the encoder actually
        // receives - the routed/rendered coded channels, not each source's
        // own raw layout, since map= can permute, trim or fold several
        // sources onto them - so this renders the entire programme once
        // purely to measure it. Dual mono gets one single-channel meter per
        // programme (Ch1/Ch2 are unrelated, §E1.3 - see
        // measured_dialnorm_channel's own comment); every other target gets
        // one whole-programme meter, the same BS.1770 channel weighting
        // measured_dialnorm uses for the single-file case.
        std::optional<ac3::meta::LoudnessMeter> whole;
        std::optional<ac3::meta::LoudnessMeter> ch1;
        std::optional<ac3::meta::LoudnessMeter> ch2;
        if (dual_mono) {
            if (want_dialnorm) {
                ch1.emplace(*sr, ac3::Acmod::k1_0, false);
            }
            if (want_dialnorm2) {
                ch2.emplace(*sr, ac3::Acmod::k1_0, false);
            }
        } else if (want_dialnorm) {
            whole.emplace(*sr, cp.bed_acmod, cp.bed_lfe);
        }
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole) {
                whole->push(views);
            }
            if (ch1) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
        }
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured) {
                std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2) {
                std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return 1;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    assert(static_cast<int>(nchans) == encoder.channel_count());
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        route_frame(start);
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: the encoder cannot express this configuration");
            write_partial_output(out_path, meta.keep_partial, frames);
            return 1;
        }
        frames.push_back(std::move(unit->bytes));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    if (p.vbr) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number.
        std::size_t min_bytes = frames.empty() ? 0 : frames.front().size();
        std::size_t max_bytes = 0;
        std::size_t total_bytes = 0;
        for (const auto& frame : frames) {
            min_bytes = std::min(min_bytes, frame.size());
            max_bytes = std::max(max_bytes, frame.size());
            total_bytes += frame.size();
        }
        const double mean_bytes = frames.empty() ? 0.0
                                                  : static_cast<double>(total_bytes) /
                                                        static_cast<double>(frames.size());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(sources->sample_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        std::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), plan::format_vbr(p.vbr), sources->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
        std::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     min_bytes, max_bytes, mean_bytes, mean_kbps);
    } else {
        std::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), bitrate, sources->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label, status);
    return 0;
}

int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools, std::string_view layout,
                    std::string_view vbr, const Options& meta,
                    std::string_view in2_path = {}) {
    if (!meta.sources.empty() || meta.map_spec) {
        if (!in2_path.empty()) {
            std::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return 1;
        }
        return run_eac3_encode_multi(in_path, out_path, bitrate, tools, layout, vbr, meta);
    }
    // The same streaming-vs-whole-file split as run_encode, for the same
    // reasons - see its comment. A failed open falls through so read_wav_arg
    // produces the error message it always has.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           !meta.p.measure_dialnorm2 && in2_path.empty() &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return 1;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        std::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return 1;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kEac3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        // An unnamed layout follows the source, which is what this command
        // did before it could be told otherwise.
        const auto id = plan::layout_for_source(src_channels);
        if (!id) {
            std::println(stderr, "error: {} channels - {}", src_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return 1;
    }
    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm", status)
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe, status);
        if (!measured) {
            std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2) {
            std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing) {
        return 1;
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    assert(static_cast<int>(nchans) == encoder.channel_count());
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    const std::size_t total = offset + frame_count;

    std::vector<std::vector<float>> source(src_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    std::vector<std::vector<std::byte>> frames;
    // See run_encode's identical streaming state: the loop consumes the
    // source strictly in order, so a rolling read position and the last
    // real sample per channel are all the streaming path needs.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // Hold the last real sample past end-of-file rather than dropping to
        // hard zero - see run_encode's identical padding for why: a sudden
        // drop to silence is itself a transient the encoder would (correctly)
        // spend a block-switch on, for a discontinuity that only exists
        // because this frame ends mid-buffer. Ahead of the source's own
        // samples, offset= silence is real silence, not padding.
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, ac3::kSamplesPerFrame)
                               : 0;
            std::size_t want = 0;
            if (lead < ac3::kSamplesPerFrame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(ac3::kSamplesPerFrame - lead, remaining);
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                std::fill_n(source[c].begin(), lead, 0.0f);
                stream_dst[c] = std::span{source[c]}.subspan(lead, want);
            }
            if (want > 0) {
                const auto got = stream_in.read_planar(stream_dst, want);
                if (!got || *got != want) {
                    std::println(stderr, "error: {}: {}", in_path,
                                 ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                       : got.error()));
                    write_partial_output(out_path, meta.keep_partial, frames);
                    return 1;
                }
                consumed += want;
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                if (want > 0) {
                    stream_hold[c] = source[c][lead + want - 1];
                }
                std::fill(source[c].begin() + static_cast<std::ptrdiff_t>(lead + want),
                          source[c].end(), stream_hold[c]);
                in[c] = source[c];
            }
        } else {
            for (std::size_t c = 0; c < source.size(); ++c) {
                const float hold = frame_count > 0 ? wav->channels[c][frame_count - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    if (at < offset) {
                        source[c][static_cast<std::size_t>(i)] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][static_cast<std::size_t>(i)] =
                        shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: the encoder cannot express this configuration");
            write_partial_output(out_path, meta.keep_partial, frames);
            return 1;
        }
        frames.push_back(std::move(unit->bytes));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    if (p.vbr) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number.
        std::size_t min_bytes = frames.empty() ? 0 : frames.front().size();
        std::size_t max_bytes = 0;
        std::size_t total_bytes = 0;
        for (const auto& frame : frames) {
            min_bytes = std::min(min_bytes, frame.size());
            max_bytes = std::max(max_bytes, frame.size());
            total_bytes += frame.size();
        }
        const double mean_bytes = frames.empty() ? 0.0
                                                  : static_cast<double>(total_bytes) /
                                                        static_cast<double>(frames.size());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(src_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        std::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), plan::format_vbr(p.vbr), src_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
        std::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     min_bytes, max_bytes, mean_bytes, mean_kbps);
    } else {
        std::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), bitrate, src_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label, status);
    return 0;
}

// Applies EMDF object signing to freshly-encoded Atmos units when the operator
// asked for it (sign-objects) and supplied a key. Returns the number of frames
// signed, or nullopt if signing was requested but the key could not be loaded
// (the message is already printed). Not requested -> 0, units untouched. The
// key comes from the operator at runtime (signing-key=<path> or the
// AC3FORGE_SIGNING_KEY[_FILE] env vars) and is never stored - see
// docs/concepts/object-signing.md.
std::optional<int> apply_object_signing(std::vector<std::vector<std::byte>>& units,
                                        const Options& meta) {
    if (!meta.sign_objects) {
        return 0;
    }
    const auto key = ac3::signing::load_signing_key(meta.signing_key.value_or(""));
    if (!key) {
        if (key.error().kind == ac3::signing::KeyErrorKind::kAbsent) {
            std::println(stderr,
                         "error: sign-objects needs a key — pass signing-key=<path>, or set "
                         "AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY");
        } else {
            std::println(stderr, "error: {}", key.error().message);
        }
        return std::nullopt;
    }
    int signed_count = 0;
    for (auto& unit : units) {
        signed_count += ac3::signing::sign_atmos_stream(unit, *key);
    }
    return signed_count;
}

// Checks EMDF object signatures on a stream about to be decoded/monitored,
// when the operator asked for it (verify-objects) and supplied a key. Reads
// the raw stream bytes the same way sign_atmos_stream does - independent of,
// and either before or alongside, whatever Eac3Decoder itself does with
// those same bytes; never routed through it, since that class's own stance
// is that the protection field is opaque per spec (see decoder.hpp). Returns
// the summary, or nullopt if verification was requested but the key could
// not be loaded, or if any signed frame's tag did not match (both cases
// already print their own message). Not requested -> an all-zero summary,
// nothing checked, stream untouched either way: this only reads bytes, it
// never signs. A signed stream is either fully verified or the command
// refuses - matching this project's own "graceful 5.1 fallback is
// either/or" stance - never a silent partial pass.
std::optional<ac3::signing::VerifySummary> apply_object_verification(
    std::span<const std::byte> stream, const Options& meta) {
    if (!meta.verify_objects) {
        return ac3::signing::VerifySummary{};
    }
    const auto key = ac3::signing::load_signing_key(meta.signing_key.value_or(""));
    if (!key) {
        if (key.error().kind == ac3::signing::KeyErrorKind::kAbsent) {
            std::println(stderr,
                         "error: verify-objects needs a key — pass signing-key=<path>, or set "
                         "AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY");
        } else {
            std::println(stderr, "error: {}", key.error().message);
        }
        return std::nullopt;
    }
    const auto summary = ac3::signing::verify_atmos_stream(stream, *key);
    std::println("  object signature: {} valid, {} mismatched, {} unsigned frame(s)",
                 summary.valid, summary.mismatch, summary.no_container);
    if (summary.mismatch > 0) {
        std::println(stderr,
                     "error: object signature verification failed ({} of {} signed frames did "
                     "not match the supplied key)",
                     summary.mismatch, summary.valid + summary.mismatch);
        return std::nullopt;
    }
    return summary;
}

// Objects moving in three dimensions, out as one 5.1 E-AC-3 stream carrying
// JOC and OAMD. Each object orbits at its own rate and sits at its own height,
// so no two of them share a direction for long - which is the condition under
// which JOC can actually pull them apart again. Heights are what makes this
// worth doing at all: a 5.1 bed cannot carry them, and the object metadata can.
int run_atmos(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t objects, std::uint32_t orbit_seconds, std::string_view mode,
              const Options& meta) {
    if (objects < 1 || objects > 15) {
        std::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return 1;
    }
    // "objects" emits the JOC + OAMD container; "bed51" omits it so the stream
    // degrades to a plain 5.1 bed on a decoder that refuses an unvalidated
    // object container instead of falling back (see AtmosConfig).
    if (mode != "objects" && mode != "bed51") {
        std::println(stderr, "error: mode is 'objects' (default) or 'bed51'");
        return 1;
    }
    const bool emit_objects = mode != "bed51";
    const auto count = static_cast<std::size_t>(objects);
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = bitrate,
                                    .dialnorm = meta.p.dialnorm,
                                    .num_bands_idx = 4,
                                    .emit_object_metadata = emit_objects,
                                    .fast_mdct = meta.fast_mdct},
                                   static_cast<int>(objects)};

    // Distinct tones so the objects are separable in the first place, and a
    // reader with an object renderer can tell which one ended up where.
    std::vector<double> tone_hz(count);
    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
        // Rates that are not simple ratios of each other, so the objects do
        // not lock into formation and stay separable.
        const double rate = 1.0 / (static_cast<double>(orbit_seconds) *
                                   (1.0 + 0.31 * static_cast<double>(i)));
        // Spread around the ring to begin with, or a short clip would show
        // them all bunched in the same quadrant - and objects that share a
        // direction are exactly the ones JOC cannot separate.
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(count);
        const double height = count == 1 ? 0.5
                                         : -1.0 + 2.0 * static_cast<double>(i) /
                                                      static_cast<double>(count - 1);
        paths.push_back(ac3::oba::make_orbit_path(
            rate, phase, height, 0.7 / std::sqrt(static_cast<double>(count)),
            // Only the lowest object feeds the LFE, and only a little: it is
            // the one channel JOC never touches.
            i == 0 ? 0.2 : 0.0));
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> essences(count,
                                             std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    std::vector<std::vector<std::byte>> out;
    out.reserve(static_cast<std::size_t>(frames));

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        // The placement is the object's position at the END of the frame,
        // because that is where both metadata layers interpolate to: OAMD's
        // ramp and the JOC matrix both finish there.
        const double t = static_cast<double>(n0 + ac3::kSamplesPerFrame) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, t);
        for (std::size_t i = 0; i < count; ++i) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                essences[i][static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * tone_hz[i] *
                             static_cast<double>(n0 + static_cast<std::uint64_t>(n)) / 48000.0));
            }
            views[i] = essences[i];
        }
        n0 += ac3::kSamplesPerFrame;

        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and "
                         "the mantissas share one frame, so try a higher bit rate",
                         objects, bitrate);
            return 1;
        }
        out.push_back(std::move(unit->bytes));
    }
    // Optional object signing: writes the keyed EMDF-protection tag so a
    // decoder that validates it accepts the JOC objects instead of falling
    // back to the 5.1 bed. Off unless the operator passes sign-objects with a
    // key; the algorithm is in-tree (clean-room), only the key is supplied.
    const auto signed_count = apply_object_signing(out, meta);
    if (!signed_count) {
        return 1;
    }
    if (*signed_count > 0) {
        std::println("  signed {} frames' EMDF object container with the supplied key",
                     *signed_count);
    }
    if (!write_frames(out_path, out)) {
        return 1;
    }
    std::println("wrote {} E-AC-3 access units to {}", frames, out_path);
    if (emit_objects) {
        std::println("  {} dynamic objects + the bed's LFE = {} objects, JOC over a 5.1 downmix",
                     objects, ac3::oba::object_count(encoder.program()));
    } else {
        std::println("  bed51: 5.1 bed only, no object container — plays as 5.1 on a decoder "
                     "that rejects an unvalidated one ({} objects were panned into the bed)",
                     objects);
    }
    return 0;
}

// Parses a hand-authored keyframe file: whitespace-separated columns
// "object_index time_s x y z gain lfe_send" per line, blank lines and '#'
// comments (to end of line) skipped. Returns each object's keyframes, indexed
// by object_index - an object index with no lines simply gets an empty entry.
std::optional<std::vector<std::vector<ac3::oba::Keyframe>>> parse_path_file(
    std::string_view path) {
    std::ifstream in{std::string{path}};
    if (!in) {
        std::println(stderr, "error: cannot open {}", path);
        return std::nullopt;
    }
    std::vector<std::vector<ac3::oba::Keyframe>> by_object;
    std::string line;
    for (std::size_t lineno = 1; std::getline(in, line); ++lineno) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream tokens{line};
        std::size_t object = 0;
        if (!(tokens >> object)) {
            continue;  // blank, or comment-only, line
        }
        ac3::oba::Keyframe kf;
        if (!(tokens >> kf.time_s >> kf.position.x >> kf.position.y >> kf.position.z >>
              kf.gain >> kf.lfe_send)) {
            std::println(stderr, "error: {}:{}: expected 'object time_s x y z gain lfe_send'",
                         path, lineno);
            return std::nullopt;
        }
        if (object >= by_object.size()) {
            by_object.resize(object + 1);
        }
        by_object[object].push_back(kf);
    }
    return by_object;
}

// Objects driven by a hand-authored keyframe file rather than the built-in
// orbit above - the CLI-side proof that ac3::oba's path primitive works end
// to end from genuinely authored motion, not just a closed-form generator.
// An object index the file never mentions holds still at room centre, the
// same fallback the GUI uses for an object with no authored path.
int run_atmos_path(std::string_view out_path, std::string_view paths_path, std::uint32_t seconds,
                   std::uint32_t bitrate, std::uint32_t objects_arg, const Options& meta) {
    const auto parsed = parse_path_file(paths_path);
    if (!parsed) {
        return 1;
    }
    const auto objects =
        objects_arg != 0 ? static_cast<std::size_t>(objects_arg) : parsed->size();
    if (objects < 1 || objects > 15) {
        std::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return 1;
    }
    if (parsed->size() > objects) {
        std::println(stderr,
                     "error: {} has keyframes up to object index {}, more than the {} objects "
                     "requested",
                     paths_path, parsed->size() - 1, objects);
        return 1;
    }

    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        if (i < parsed->size() && !(*parsed)[i].empty()) {
            auto created = ac3::oba::KeyframePath::create((*parsed)[i]);
            if (!created) {
                std::println(stderr, "error: object {} has two keyframes at the same time_s", i);
                return 1;
            }
            paths.emplace_back(std::move(*created));
            continue;
        }
        auto fallback = ac3::oba::KeyframePath::create(
            {{.time_s = 0.0,
              .position = {.x = 0.5, .y = 0.5, .z = 0.0},
              .gain = 0.7 / std::sqrt(static_cast<double>(objects)),
              .lfe_send = 0.0}});
        paths.emplace_back(std::move(*fallback));
    }

    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct},
        static_cast<int>(objects)};

    // Distinct tones purely for audibility, same as 'atmos'.
    std::vector<double> tone_hz(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> essences(objects,
                                             std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(objects);
    std::vector<std::vector<std::byte>> out;
    out.reserve(static_cast<std::size_t>(frames));

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(n0 + ac3::kSamplesPerFrame) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(paths, t);
        for (std::size_t i = 0; i < objects; ++i) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                essences[i][static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * tone_hz[i] *
                             static_cast<double>(n0 + static_cast<std::uint64_t>(n)) / 48000.0));
            }
            views[i] = essences[i];
        }
        n0 += ac3::kSamplesPerFrame;

        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and "
                         "the mantissas share one frame, so try a higher bit rate",
                         objects, bitrate);
            return 1;
        }
        out.push_back(std::move(unit->bytes));
    }
    // Optional object signing, same as 'atmos' - see apply_object_signing's
    // own comment.
    const auto signed_count = apply_object_signing(out, meta);
    if (!signed_count) {
        return 1;
    }
    if (*signed_count > 0) {
        std::println("  signed {} frames' EMDF object container with the supplied key",
                     *signed_count);
    }
    if (!write_frames(out_path, out)) {
        return 1;
    }
    std::println("wrote {} E-AC-3 access units to {} ({} objects from {})", frames, out_path,
                 objects, paths_path);
    return 0;
}

// Every channel of a real file as its own object, over a 5.1 bed with JOC and
// OAMD beside it. The synthetic 'atmos' above shows what the object layer can
// express; this is the one that answers what it does to material somebody
// actually recorded - and it is what the GUI's object mode runs, so the two
// front ends can be compared on the same file.
int run_atmos_encode(std::string_view in_path, std::string_view out_path,
                     std::uint32_t bitrate, std::uint32_t objects,
                     const Options& meta, std::string_view paths_path = {}) {
    // The same streaming-vs-whole-file split as run_encode - see its
    // comment. This command has no dual-mono merge, so only stdin and
    // dialnorm=auto (whole-programme BS.1770) force the whole-file read.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    // One object per source channel unless told otherwise; more objects than
    // the file has channels would leave some carrying nothing.
    const auto count = objects == 0 ? src_channels
                                    : std::min<std::size_t>(objects, src_channels);
    if (count < 1 || count > 15) {
        std::println(stderr,
                     "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 "
                     "§8.3.2.2 caps the total at 16); this file has {} channels",
                     src_channels);
        return 1;
    }

    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        const auto layout = ac3::io::ac3_layout_for(src_channels);
        const auto measured = layout
                                  ? measured_dialnorm(*wav, *sr, layout->acmod, layout->lfe, status)
                                  : std::nullopt;
        if (!measured) {
            std::println(stderr, "error: cannot measure loudness for this file; "
                                 "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        dialnorm = *measured;
    }

    ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct},
        static_cast<int>(count)};

    // Objects that reach the bed by the same route are exactly the ones JOC
    // cannot pull apart again, so the source's channels are spread across the
    // room rather than stacked at one point. A channel that already has a
    // direction keeps it; the rest fan out evenly.
    std::vector<ac3::oba::ObjectPlacement> placement(count);
    const auto layout = ac3::io::ac3_layout_for(src_channels);
    for (std::size_t i = 0; i < count; ++i) {
        double azimuth = 0.0;
        if (layout) {
            // wav_index maps a coded channel to a WAV one; this needs the
            // inverse, so the channel is found rather than indexed.
            for (std::size_t k = 0; k < layout->wav_index.size(); ++k) {
                if (layout->wav_index[k] != i) {
                    continue;
                }
                azimuth = ac3::analysis::channel_azimuth_deg(layout->acmod, layout->lfe,
                                                             static_cast<int>(k))
                              .value_or(0.0);
            }
        } else {
            azimuth = 360.0 * static_cast<double>(i) / static_cast<double>(count);
        }
        const double radians = azimuth * std::numbers::pi / 180.0;
        placement[i] = {.position = {.x = 0.5 - 0.5 * std::sin(radians),
                                     .y = 0.5 - 0.5 * std::cos(radians),
                                     .z = 0.0},
                        // Every object is panned into the same five channels,
                        // so their contributions add there. The same
                        // inverse-root law 'atmos' and the GUI use, so a file
                        // encoded either way comes out at the same level.
                        .gain = 0.7 / std::sqrt(static_cast<double>(count)),
                        .lfe_send = 0.0};
    }

    // An authored keyframe file (same format/addressing as atmos-path, object
    // index == this WAV channel index) drives motion instead of the static
    // placement above; empty (the default) leaves that placement reused
    // unchanged every frame, exactly as before this argument existed - see
    // the per-frame loop below.
    std::optional<std::vector<ac3::oba::ObjectPath>> paths;
    if (!paths_path.empty()) {
        const auto parsed = parse_path_file(paths_path);
        if (!parsed) {
            return 1;
        }
        paths.emplace();
        paths->reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i < parsed->size() && !(*parsed)[i].empty()) {
                auto created = ac3::oba::KeyframePath::create((*parsed)[i]);
                if (!created) {
                    std::println(stderr, "error: object {} has two keyframes at the same time_s",
                                 i);
                    return 1;
                }
                paths->emplace_back(std::move(*created));
                continue;
            }
            // Not mentioned in the file: keep exactly the placement this
            // object has today, just re-expressed as a (never-moving) path.
            auto fallback = ac3::oba::KeyframePath::create({{.time_s = 0.0,
                                                              .position = placement[i].position,
                                                              .gain = placement[i].gain,
                                                              .lfe_send = placement[i].lfe_send}});
            paths->emplace_back(std::move(*fallback));
        }
    }

    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, src_rate};
    const std::size_t total =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    std::vector<std::vector<float>> block(count, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    std::vector<std::vector<std::byte>> out;
    // Streaming reads every file channel (read_planar's contract), but only
    // the first `count` become objects - the extras land in one shared
    // discard buffer whose contents nothing reads.
    std::vector<float> stream_discard(streaming ? ac3::kSamplesPerFrame : 0);
    std::vector<std::span<float>> stream_dst(streaming ? src_channels : 0);

    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        if (streaming) {
            for (std::size_t ch = 0; ch < src_channels; ++ch) {
                stream_dst[ch] = ch < count ? std::span{block[ch]}.first(valid)
                                            : std::span{stream_discard}.first(valid);
            }
            const auto got = stream_in.read_planar(stream_dst, valid);
            if (!got || *got != valid) {
                std::println(stderr, "error: {}: {}", in_path,
                             ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                   : got.error()));
                write_partial_output(out_path, meta.keep_partial, out);
                return 1;
            }
            for (std::size_t ch = 0; ch < count; ++ch) {
                // The tail frame zero-pads past the file's end, exactly as
                // the whole-file loop below writes 0.0f there.
                std::fill(block[ch].begin() + static_cast<std::ptrdiff_t>(valid),
                          block[ch].end(), 0.0f);
                views[ch] = block[ch];
            }
        } else {
            for (std::size_t ch = 0; ch < count; ++ch) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    block[ch][static_cast<std::size_t>(i)] =
                        at < total ? wav->channels[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
            }
        }
        // With paths_path, the object placement moves - evaluated at the
        // frame's END time, the same convention run_atmos_path and the GUI's
        // encodeObjects use. Without it, every frame reuses the one static
        // placement computed above, byte-identical to before this argument
        // existed.
        auto unit = paths ? encoder.encode_frame(
                                views, ac3::oba::evaluate_placements(
                                           *paths, static_cast<double>(start + ac3::kSamplesPerFrame) /
                                                       static_cast<double>(src_rate)))
                          : encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            write_partial_output(out_path, meta.keep_partial, out);
            return 1;
        }
        // The bed exists only once the frame is encoded, so it is metered
        // afterwards - and it is the bed, not the source, that a legacy
        // decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        out.push_back(std::move(unit->bytes));
    }
    // Optional object signing, same as 'atmos' - see apply_object_signing's
    // own comment. Goes through status_stream() like the report below: with
    // out_path == "-" the E-AC-3 bytes just written own stdout.
    const auto signed_count = apply_object_signing(out, meta);
    if (!signed_count) {
        return 1;
    }
    if (*signed_count > 0) {
        std::println(status_stream(out_path),
                     "  signed {} frames' EMDF object container with the supplied key",
                     *signed_count);
    }
    if (!write_frames(out_path, out)) {
        return 1;
    }
    std::println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) to {}", out.size(),
                bitrate, src_rate, out_path);
    std::println(status,
                 "  {} objects from {} source channels + the bed's LFE = {} objects, "
                 "JOC over a 5.1 downmix",
                 count, src_channels, ac3::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return 0;
}

// Roadmap B1 phase 3 of 3 (see ROADMAP.md's "ADM BWF reader feeding the JOC encoder" entry - the
// last phase; phase 1 is src/ac3adm, phase 2 is src/admbridge, this command is the first place
// both are driven together). A real ADM BWF master (BS.2076-2 ADM XML embedded in a BS.2088-1
// BW64/RF64 container) straight to DD+ JOC E-AC-3 - no WAV plus a hand-authored keyframe file the
// way atmos-encode above needs, because the master already carries every bed speaker feed's and
// dynamic object's own position/gain automation (§10.3). Every resolved channel becomes one of
// AtmosEncoder's flat object slots (a bed channel pinned in place, or LFE-routed, exactly as
// ac3::admbridge::build()'s own header comment describes), driven frame by frame by
// ac3::oba::evaluate_placements the same way run_atmos_path/run_atmos_encode above already do.
//
// The parse+bridge step itself (ac3adm::parse_bw64, ac3::admbridge::build) is NOT called from
// here: ac3adm::ac3adm/ac3::admbridge are this project's one opt-in, non-default library
// (AC3FORGE_BUILD_ADM, default OFF - see the root CMakeLists.txt's own option() for why), and this
// file cannot name their types at all in a build where AC3FORGE_BUILD_ADM is off - not even behind
// a preprocessor guard, since this project's tools/checks/check_platform_macros.ps1 (CI-enforced, see
// .github/workflows/ci.yml's "Check for preprocessor conditionals in src/" job) refuses ANY
// #if/#ifdef/#ifndef anywhere under src/, deliberately stricter than "no OS macros" (that script's
// own comment: a feature-flag #ifdef is "just as unwelcome as a platform one"). So the same
// principle this file's own platform/stdio_binary.cpp split already uses for an OS difference
// applies here to a library-linked-or-not difference instead: adm/atmos_adm.hpp declares
// ac3cli::load_adm_atmos_source() and ac3cli::adm_capability() unconditionally, in terms of
// ac3::oba's own always-available types only, and apps/cli/CMakeLists.txt compiles exactly one of
// adm/enabled/atmos_adm.cpp (the real ac3adm/admbridge call) or adm/disabled/atmos_adm.cpp (a
// stub) into this same ac3cli binary - never both, never neither. This function, and its
// kCommands row below, are therefore unconditional too: 'atmos-adm' is always one of the 26 rows
// in the table (matching every command's own fixed shape) and is refused before this function is
// ever called by the SAME Needs::kAdm/unmet() capability gate the audio-hardware commands
// (Needs::kCapture/kPassthrough/kMonitor, ac3::audio::audio_backend()) already use for their
// own "is this available in this particular build?" question - reused rather than a second
// mechanism invented for what is structurally the identical problem. (An earlier version of this
// function used a scoped #ifdef instead, before tools/checks/check_platform_macros.ps1 was actually run
// against it and found to reject that outright - this design is what replaced it.)
//
// This function is deliberately thin beyond that seam: parse+bridge, per-frame evaluate+encode,
// write - the same library-API-is-the-shared-layer convention examples/encode_adm.cpp follows
// independently (see docs/library/adm-bridge.md), just reached through load_adm_atmos_source()
// instead of ac3adm::parse_bw64/ac3::admbridge::build directly.
int run_atmos_adm(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  const Options& meta, std::string_view programme_id) {
    // No fixed source layout to measure a pre-encode loudness figure against the way
    // atmos-encode's WAV input has (ac3::io::ac3_layout_for) - an ADM document's channels are an
    // arbitrary mix of bed speaker feeds and dynamic objects, not one of the handful of layouts
    // that function maps. Refusing clearly beats silently keeping the fixed default dialnorm:
    // "a silently ignored metadata flag looks exactly like metadata that did not work" (see
    // parse_options's own comment above).
    if (meta.p.measure_dialnorm) {
        std::println(stderr,
                     "error: dialnorm=auto is not supported by atmos-adm - an ADM document's bed/"
                     "object channels have no single fixed layout to measure loudness against the "
                     "way atmos-encode's WAV input does; pass dialnorm=<1..31> explicitly");
        return 1;
    }

    auto source = ac3cli::load_adm_atmos_source(in_path, programme_id);
    if (!source) {
        std::println(stderr, "error: {}: {}", in_path, source.error());
        return 1;
    }

    const auto sr = wav_sample_rate(source->sample_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }

    const auto count = source->channel_count();
    if (count < 1 || count > 15) {
        std::println(stderr,
                     "error: 1 to 15 bed/object channels (the bed's LFE is the 16th, and TS 103 "
                     "420 §8.3.2.2 caps the total at 16); {} resolved {} channel(s)",
                     in_path, count);
        return 1;
    }

    ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm,
         .num_bands_idx = 4, .fast_mdct = meta.fast_mdct},
        static_cast<int>(count)};

    // Metered the same way run_atmos_encode meters its own bed: 3/2 + LFE is AtmosEncoder's own
    // fixed bed layout regardless of how many dynamic objects/bed feeds fed it.
    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, source->sample_rate};
    const std::size_t total = source->pcm.empty() ? 0 : source->pcm.front().size();
    std::vector<std::vector<float>> block(count, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    std::vector<std::vector<std::byte>> out;

    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        for (std::size_t ch = 0; ch < count; ++ch) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[ch][static_cast<std::size_t>(i)] =
                    at < source->pcm[ch].size() ? source->pcm[ch][at] : 0.0f;
            }
            views[ch] = block[ch];
        }
        // Evaluated at the frame's END time, the same convention run_atmos_path/run_atmos_encode
        // use.
        const auto placement = ac3::oba::evaluate_placements(
            source->paths, static_cast<double>(start + ac3::kSamplesPerFrame) /
                                static_cast<double>(source->sample_rate));
        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} channels at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            write_partial_output(out_path, meta.keep_partial, out);
            return 1;
        }
        // The bed exists only once the frame is encoded, so it is metered afterwards - and it is
        // the bed, not the source, that a legacy decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        out.push_back(std::move(unit->bytes));
    }
    if (!write_frames(out_path, out)) {
        return 1;
    }

    std::size_t bed_count = 0;
    for (const bool is_bed : source->is_bed) {
        bed_count += is_bed ? 1 : 0;
    }
    // See run_encode's identical status_stream() comment: out_path == "-" means the E-AC-3 bytes
    // just written own stdout, so this report goes to stderr instead.
    const auto status = status_stream(out_path);
    std::println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) from {} to {}",
                out.size(), bitrate, source->sample_rate, in_path, out_path);
    std::println(status,
                 "  {} bed speaker feed(s) + {} dynamic object(s) + the bed's LFE = {} objects, "
                 "JOC over a 5.1 downmix",
                 bed_count, count - bed_count, ac3::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return 0;
}

int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t orbit_seconds, const Options& meta) {
    ac3::spatial::BedRenderer renderer;
    const auto object =
        renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7, .lfe_send = 0.15});
    const plan::Plan p{.codec = plan::Codec::kAc3,
                       .layout = plan::LayoutId::k51,
                       .bitrate_kbps = bitrate,
                       .tools = {.fast_mdct = meta.fast_mdct},
                       .meta = meta.p};
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(p));
    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, 48000};

    const std::uint64_t count = frame_count(seconds);
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
        meter.process(views);
        auto frame = encoder->encode_frame(views);
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
    // An orbit visits every speaker equally, so the summary's job here is to
    // show that no channel was left out and none dominates.
    print_channel_summary(meter);
    return 0;
}

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

// Capture live audio and encode it straight to AC-3. The capture thread fills
// a lock-free ring; this thread drains it a frame at a time.
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

// The same encode as run_encode below, but for a possibly multi-source run
// (src=/map= given) - see run_eac3_encode_multi for why this is a separate
// function rather than a shared path with the classic single-file one, and
// for the shape of its dialnorm=auto/dialnorm2=auto measurement pre-pass,
// which this mirrors exactly.
int run_encode_multi(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                     bool couple, std::string_view layout, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources) {
        return 1;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "AC-3", false);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kAc3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        std::size_t total_channels = 0;
        for (const auto& shape : sources->shapes) {
            total_channels += shape.channels;
        }
        const auto id = plan::layout_for_source(total_channels);
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            std::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         total_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        std::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing) {
        return 1;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // Renders one frame's worth of every source's samples onto the coded
    // channels `out`/`views` alias - shared by the measurement pre-pass below
    // and the real encode loop after it, so the two can never render this
    // programme two different ways.
    auto route_frame = [&](std::size_t start) {
        gather_frame(*sources, start, source);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
    };

    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    const bool want_dialnorm = p.meta.measure_dialnorm;
    // dialnorm2 only means anything under 1+1 - silently inert otherwise,
    // exactly like run_encode's identical check for its one file.
    const bool want_dialnorm2 = dual_mono && p.meta.measure_dialnorm2;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes this function writes below already own stdout in that
    // case, and no human-readable report (the dialnorm=auto measurement just
    // below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (want_dialnorm || want_dialnorm2) {
        // §5.4.2.8's BS.1770 pass has to measure what the encoder actually
        // receives - the routed/rendered coded channels, not each source's
        // own raw layout, since map= can permute, trim or fold several
        // sources onto them - so this renders the entire programme once
        // purely to measure it. Dual mono gets one single-channel meter per
        // programme (Ch1/Ch2 are unrelated, §E1.3 - see
        // measured_dialnorm_channel's own comment); every other target gets
        // one whole-programme meter, the same BS.1770 channel weighting
        // measured_dialnorm uses for the single-file case.
        std::optional<ac3::meta::LoudnessMeter> whole;
        std::optional<ac3::meta::LoudnessMeter> ch1;
        std::optional<ac3::meta::LoudnessMeter> ch2;
        if (dual_mono) {
            if (want_dialnorm) {
                ch1.emplace(*sr, ac3::Acmod::k1_0, false);
            }
            if (want_dialnorm2) {
                ch2.emplace(*sr, ac3::Acmod::k1_0, false);
            }
        } else if (want_dialnorm) {
            whole.emplace(*sr, cp.bed_acmod, cp.bed_lfe);
        }
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole) {
                whole->push(views);
            }
            if (ch1) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
        }
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured) {
                std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2) {
                std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return 1;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    const auto config = plan::ac3_config(p);
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, sources->sample_rate};
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        route_frame(start);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            write_partial_output(out_path, meta.keep_partial, frames);
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", frames.size(), bitrate,
                 sources->sample_rate, ac3::analysis::layout_name(config.acmod, config.lfe),
                 out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return 0;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple, std::string_view layout, const Options& meta,
               std::string_view in2_path = {}) {
    if (!meta.sources.empty() || meta.map_spec) {
        if (!in2_path.empty()) {
            std::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return 1;
        }
        return run_encode_multi(in_path, out_path, bitrate, couple, layout, meta);
    }
    // A seekable file whose whole-programme passes are not needed - no
    // dialnorm=auto BS.1770 measurement, no second dual-mono file to merge -
    // streams one frame-sized block at a time, holding ~40 KB of samples
    // resident instead of the whole file plus its planar float copy (the
    // measured peak was linear in duration before this: 152 MiB for a 60 s
    // 5.1 encode, 438 MiB for 180 s). Everything else takes the whole-file
    // read below, unchanged - including a failed open, which falls through
    // so read_wav_arg can produce the error message it always has.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           !meta.p.measure_dialnorm2 && in2_path.empty() &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return 1;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        // The same refusal prepare_dual_mono_source gives the one-file 1+1
        // case; the streaming path validates off the header instead.
        std::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return 1;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "AC-3", false);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kAc3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        // An unnamed layout follows the source, which is what this command
        // did before it could be told otherwise. Naming one is how a stereo
        // file reaches a 5.1 stream, or a 5.1 file gets folded down per §7.8.
        const auto id = plan::layout_for_source(src_channels);
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            std::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         src_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        std::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    // §5.4.2.8 says dialnorm "shall affect the sound reproduction level", so
    // getting it wrong is not a cosmetic error - a stream that claims 31 when
    // dialogue is really at -18 plays 13 dB too loud on a levelled system.
    // Measuring needs the whole programme (the BS.1770 relative gate does),
    // which is why it happens here rather than inside the frame encoder. It
    // gets the OUTPUT layout, because the BS.1770 channel weighting depends on
    // which coded positions are surrounds.
    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes this function writes below already own stdout in that
    // case, and no human-readable report (the dialnorm=auto measurement just
    // below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm", status)
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe, status);
        if (!measured) {
            std::println(stderr,
                         "error: {}no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2) {
            std::println(stderr,
                         "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing) {
        return 1;
    }

    const auto config = plan::ac3_config(p);
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, src_rate};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    const std::size_t total = offset + frame_count;

    std::vector<std::vector<float>> source(src_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    std::vector<std::vector<std::byte>> frames;
    // The streaming path's own state: the frame loop below asks for the
    // source's samples strictly in order (offset= only ever shifts where
    // they land inside a frame, never which come next), so a rolling read
    // position plus the last real sample per channel - for the same
    // hold-padding the whole-file path applies - is all it takes.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // The tail frame is padded to a full 1536 samples; the meter sees only
        // the real ones, so the padding cannot pull the RMS down. Padding
        // holds the last real sample rather than dropping to hard zero: a
        // sudden drop to silence is itself a transient, and the encoder's own
        // §8.2.2 detector would (correctly) spend a block-switch on it,
        // paying real side-info bits to preserve a discontinuity that exists
        // only because this frame ends mid-buffer, not in the source audio.
        // Ahead of the source's own samples, offset= silence is real
        // silence, not padding.
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, ac3::kSamplesPerFrame)
                               : 0;
            std::size_t want = 0;
            if (lead < ac3::kSamplesPerFrame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(ac3::kSamplesPerFrame - lead, remaining);
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                std::fill_n(source[c].begin(), lead, 0.0f);
                stream_dst[c] = std::span{source[c]}.subspan(lead, want);
            }
            if (want > 0) {
                const auto got = stream_in.read_planar(stream_dst, want);
                if (!got || *got != want) {
                    std::println(stderr, "error: {}: {}", in_path,
                                 ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                       : got.error()));
                    write_partial_output(out_path, meta.keep_partial, frames);
                    return 1;
                }
                consumed += want;
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                if (want > 0) {
                    stream_hold[c] = source[c][lead + want - 1];
                }
                std::fill(source[c].begin() + static_cast<std::ptrdiff_t>(lead + want),
                          source[c].end(), stream_hold[c]);
                in[c] = source[c];
            }
        } else {
            for (std::size_t c = 0; c < source.size(); ++c) {
                const float hold = frame_count > 0 ? wav->channels[c][frame_count - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    if (at < offset) {
                        source[c][static_cast<std::size_t>(i)] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][static_cast<std::size_t>(i)] =
                        shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: bitrate must be a legal AC-3 rate");
            write_partial_output(out_path, meta.keep_partial, frames);
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", frames.size(), bitrate,
                src_rate, ac3::analysis::layout_name(config.acmod, config.lfe), out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return 0;
}

// Reports the object layer (if any) an E-AC-3 decode found - the decode-side
// mirror of run_atmos_encode's own "{N} dynamic objects + the bed's LFE = {M}
// objects" line. Shared between run_decode_eac3's dual-mono and ordinary
// return paths, even though this project's own AtmosEncoder never emits dual
// mono alongside an object container. The object WAVs themselves are
// streamed out by per-object sinks as the decode runs (run_decode_eac3's
// append_objects) - by the time this prints, the files are already closed;
// this only says what happened.
int report_decoded_objects(FILE* status, const std::optional<ac3::oba::DecodedProgram>& metadata,
                           bool have_object_audio, std::size_t objects_written,
                           std::string_view objects_dir) {
    if (metadata) {
        std::println(status, "  {} dynamic objects + the bed's LFE = {} objects, OAMD present{}",
                     metadata->objects.size(), ac3::oba::object_count(metadata->program),
                     have_object_audio ? ", JOC audio reconstructed"
                                       : " (JOC audio not reconstructed)");
    }
    if (objects_dir.empty()) {
        return 0;
    }
    if (objects_written == 0) {
        std::println(stderr,
                     "warning: objects_dir given but there is no reconstructed object audio to "
                     "export");
        return 0;
    }
    std::println(status, "  wrote {} object WAV(s) to {}", objects_written, objects_dir);
    return 0;
}

// The dynrng/compr half of run_decode's own status report (main.cpp, further
// down), factored out so run_decode_eac3 can report the same two figures -
// range actually carried, and whether drc=/heavy asked for them to be
// applied - without duplicating run_decode's own dialnorm-anchored
// indentation, which this command's report has no dialnorm line to anchor to.
void print_drc_summary(FILE* status, double dynrng_min_db, double dynrng_max_db,
                       double compr_min_db, double compr_max_db, std::size_t compr_frames,
                       const Options& meta) {
    std::println(status, "  dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                 meta.drc_scale != 0.0 ? std::format(", applied at scale {}", meta.drc_scale)
                                       : ", not applied");
    if (compr_frames > 0) {
        std::println(status, "  compr  {:+.2f} .. {:+.2f} dB over {} access units{}",
                     compr_min_db, compr_max_db, compr_frames,
                     meta.p.heavy ? ", applied" : ", not applied");
    } else {
        std::println(status, "  compr  absent");
    }
}

int run_decode_eac3(std::span<const std::byte> stream, std::string_view out_path,
                     const Options& meta, std::string_view objects_dir) {
    // Access units, not syncframes: a dependent substream is only meaningful
    // alongside the independent one it extends, and the two are rendered
    // together into one set of speaker feeds.
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::println(stderr, "error: stream framing failed (code {})",
                     static_cast<int>(units.error()));
        return 1;
    }
    ac3::Eac3Decoder decoder{
        {.drc_scale = meta.drc_scale, .heavy_compression = meta.p.heavy.has_value()}};
    // The decoded programme goes out through the sink as units decode - the
    // sink's per-slot carry absorbs the one place slots advance unevenly
    // (the transient-pre-noise flush below).
    PlanarWavSink sink;
    std::size_t sink_slots = 0;
    const auto open_sink = [&](const ac3::DecodedAccessUnit& unit,
                               std::size_t slots) -> bool {
        sink_slots = slots;
        // Dual mono has no Table E2.5 location to order by, so Ch1 and Ch2
        // go out in coded order - the same identity the whole-buffer write
        // fell back to. Everyone else gets the WAV speaker order the encode
        // side reads a file in.
        std::vector<std::size_t> order;
        if (unit.acmod != ac3::Acmod::kDualMono) {
            order = plan::wav_order(std::span{unit.layout.items}.first(
                static_cast<std::size_t>(unit.layout.count)));
        }
        if (!sink.open(out_path, sample_rate_hz(unit.sample_rate), slots, order)) {
            std::println(stderr, "error: cannot open {} for writing", out_path);
            return false;
        }
        return true;
    };
    // JOC's reconstructed per-object audio - parallel to
    // first.object_metadata->objects (same index, same object). With no
    // objects_dir nothing keeps it: only the fact that some arrived matters
    // to the report. With one, each object streams to its own mono WAV. An
    // access unit whose object_audio size doesn't match the sinks is
    // skipped rather than resized into: DecodedSubstream's own comment
    // documents this as reachable (a program-shape mismatch JOC's ordering
    // can't be lined up against), not something worth failing the whole
    // decode over.
    bool have_object_audio = false;
    std::vector<PlanarWavSink> object_sinks;
    const auto abort_all = [&] {
        sink.abort();
        for (auto& object_sink : object_sinks) {
            object_sink.abort();
        }
    };
    const auto append_objects = [&](const std::vector<std::vector<float>>& object_audio,
                                    std::uint32_t sample_rate) -> bool {
        if (object_audio.empty()) {
            return true;
        }
        have_object_audio = true;
        if (objects_dir.empty()) {
            return true;
        }
        if (object_sinks.empty()) {
            std::error_code ec;
            const std::filesystem::path dir{std::string{objects_dir}};
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                std::println(stderr, "error: cannot create directory {} ({})", objects_dir,
                             ec.message());
                return false;
            }
            object_sinks.resize(object_audio.size());
            for (std::size_t i = 0; i < object_sinks.size(); ++i) {
                const auto object_path = dir / std::format("object_{:02}.wav", i);
                if (!object_sinks[i].open(object_path.string(), sample_rate, 1, {})) {
                    std::println(stderr, "error: cannot open {} for writing",
                                 object_path.string());
                    return false;
                }
            }
        }
        if (object_audio.size() != object_sinks.size()) {
            return true;  // shape mismatch: skipped, same as the old append
        }
        for (std::size_t i = 0; i < object_sinks.size(); ++i) {
            if (!object_sinks[i].append(0, object_audio[i])) {
                std::println(stderr, "error: cannot write object audio under {}", objects_dir);
                return false;
            }
        }
        return true;
    };
    ac3::DecodedAccessUnit first{};
    // What the independent (bed) substream actually carried, reported whether
    // or not it was applied - same convention as run_decode's own dynrng_min_db/
    // dynrng_max_db/compr_min_db/compr_max_db above, except both are seeded
    // from the first real word rather than from 0.0: a stream whose transmitted
    // dynrng/compr never happens to cross exactly unity would otherwise have
    // its true min or max silently clamped to 0 dB by the seed itself.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    // numblkscod bounds how many of `dynrng`'s kBlocksPerFrame entries are
    // real: E-AC-3 (unlike AC-3) can code as few as one block per syncframe,
    // and the rest of the fixed-size array is never written (DecodedSubstream::
    // dynrng's own comment) - folding those unwritten, always-unity entries in
    // here would understate the true range for any such stream.
    const auto track_metadata = [&](const std::array<std::uint8_t, ac3::kBlocksPerFrame>& dynrng,
                                    int numblkscod, std::optional<std::uint8_t> compr) {
        const auto nblks =
            static_cast<std::size_t>(ac3::eac3::blocks_per_syncframe(numblkscod));
        for (std::size_t i = 0; i < nblks; ++i) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(dynrng[i]));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
    };
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            std::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            abort_all();
            return 1;
        }
        if (!decoded->has_value()) {
            // §3.7: this access unit's frame(s) are being held back pending
            // transient pre-noise processing (Eac3Decoder::decode_access_unit's
            // own doc comment) - nothing new to append yet, not an error.
            continue;
        }
        const auto& out = **decoded;
        if (!sink.is_open()) {
            first = out;
            if (!open_sink(first, out.channels.size())) {
                return 1;
            }
        }
        track_metadata(out.dynrng, out.numblkscod, out.compr);
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            if (!sink.append(ch, out.channels[ch])) {
                std::println(stderr, "error: cannot write to {}", out_path);
                abort_all();
                return 1;
            }
        }
        if (!append_objects(out.object_audio, sample_rate_hz(first.sample_rate))) {
            abort_all();
            return 1;
        }
    }
    // Whatever transient pre-noise processing was still holding back at
    // end-of-stream. flush() returns raw per-substream results rather than
    // assembled access units (see its own doc comment) - placed at the SAME
    // pcm slot decode_access_unit's own §E3.8.2 assembly would have used
    // (via location_map()), not assumed to already sit at that slot: a lone
    // independent substream's coded order happens to agree with pcm's, but
    // a dependent carrying only its own smaller channel set does not, and
    // naively appending it by coded index corrupts already-established
    // channels (e.g. a bed's L/R) with a dependent's height audio instead.
    const auto flushed = decoder.flush();
    if (!flushed.empty()) {
        // §7.7 words are meaningful at this report's level only from the
        // independent (bed) substream - same convention as
        // DecodedAccessUnit::dynrng/compr above; a dependent flushed here
        // (only possible when transient pre-noise processing has left
        // substreams of one access unit desynchronised at end-of-stream) is
        // never the figure this report promises.
        for (const auto& substream : flushed) {
            if (substream.strmtyp == ac3::eac3::StreamType::kIndependent) {
                track_metadata(substream.dynrng, substream.numblkscod, substream.compr);
            }
        }
        const bool dual_mono = sink.is_open() ? first.acmod == ac3::Acmod::kDualMono
                                              : flushed.front().acmod == ac3::Acmod::kDualMono;
        if (dual_mono) {
            // No Table E2.5 location to place by - dual mono is always a
            // lone substream with no dependents and no spatial layout
            // (decode_access_unit's own comment) - so its channels go
            // straight out in coded order, same as decode_access_unit.
            for (const auto& substream : flushed) {
                if (!sink.is_open()) {
                    first.acmod = ac3::Acmod::kDualMono;
                    first.sample_rate = substream.sample_rate;
                    first.dialnorm = substream.dialnorm;
                    first.substream_count = 1;
                    first.object_metadata = substream.object_metadata;
                    if (!open_sink(first, substream.channels.size())) {
                        return 1;
                    }
                }
                for (std::size_t ch = 0; ch < substream.channels.size(); ++ch) {
                    if (!sink.append(ch, substream.channels[ch])) {
                        std::println(stderr, "error: cannot write to {}", out_path);
                        abort_all();
                        return 1;
                    }
                }
            }
        } else {
            if (!sink.is_open()) {
                // No access unit ever completed - synthesize the program's
                // layout by unioning every flushed substream's own
                // locations, exactly like decode_access_unit's own §E3.8.2
                // assembly.
                std::uint16_t occupied = 0;
                for (const auto& substream : flushed) {
                    occupied = static_cast<std::uint16_t>(occupied | substream.location_map());
                }
                ac3::DecodedAccessUnit synthesized;
                synthesized.sample_rate = flushed.front().sample_rate;
                synthesized.acmod = flushed.front().acmod;
                synthesized.dialnorm = flushed.front().dialnorm;
                synthesized.substream_count = static_cast<int>(flushed.size());
                synthesized.layout = ac3::eac3::chanmap::expand(occupied);
                // Object audio only ever rides in the bed (the independent
                // substream) - see DecodedAccessUnit::object_metadata's own
                // comment - so at most one flushed substream carries it.
                for (const auto& substream : flushed) {
                    if (substream.object_metadata) {
                        synthesized.object_metadata = substream.object_metadata;
                        break;
                    }
                }
                first = synthesized;
                if (!open_sink(first, static_cast<std::size_t>(first.layout.count))) {
                    return 1;
                }
            }
            // §E3.8.2 placement: each flushed substream's own channels land
            // at whichever slot their Table E2.5 location occupies in
            // `first.layout`, mirroring decode_access_unit's own assembly
            // loop. Different substreams may append different lengths to
            // different slots here; the sink's per-slot carry absorbs it.
            for (const auto& substream : flushed) {
                const auto locations = ac3::eac3::chanmap::expand(substream.location_map());
                for (int i = 0; i < locations.count; ++i) {
                    const int slot = first.layout.index_of(locations[i]);
                    if (slot < 0) {
                        continue;
                    }
                    if (!sink.append(static_cast<std::size_t>(slot),
                                     substream.channels[static_cast<std::size_t>(i)])) {
                        std::println(stderr, "error: cannot write to {}", out_path);
                        abort_all();
                        return 1;
                    }
                }
            }
        }
        // JOC's reconstructed per-object audio, streamed the same way the
        // main access-unit loop's is - see append_objects for why a size
        // mismatch is skipped rather than resized into.
        for (const auto& substream : flushed) {
            if (!append_objects(substream.object_audio,
                                sample_rate_hz(substream.sample_rate))) {
                abort_all();
                return 1;
            }
        }
    }
    if (!sink.is_open()) {
        std::println(stderr, "error: no access units");
        return 1;
    }
    // Dual mono has no Table E2.5 location to order by - decode_access_unit
    // leaves `layout` empty for exactly this case - so Ch1 and Ch2 go out in
    // coded order, the same identity write_wav_f32 falls back to itself.
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the WAV bytes the write below produces already own stdout in that
    // case, and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    const auto written = sink.close();
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::size_t objects_written = 0;
    for (auto& object_sink : object_sinks) {
        if (const auto closed = object_sink.close(); !closed) {
            std::println(stderr, "error: {}", ac3::io::describe(closed.error()));
            return 1;
        }
        ++objects_written;
    }
    if (first.acmod == ac3::Acmod::kDualMono) {
        std::println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                     units->size(), first.substream_count, out_path);
        std::println(status,
                     "  {} channels, {} Hz: Ch1 Ch2 (1+1 dual mono - two programmes, not a "
                     "soundfield)",
                     sink_slots, sample_rate_hz(first.sample_rate));
        print_drc_summary(status, dynrng_min_db, dynrng_max_db, compr_min_db, compr_max_db,
                          compr_frames, meta);
        return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                      objects_written, objects_dir);
    }
    // The same WAV speaker order the encode side reads a file in, so a stream
    // decoded here and re-encoded lands every channel back where it started -
    // recomputed here only for the speaker-name report; the sink applied it.
    const auto map = plan::wav_order(
        std::span{first.layout.items}.first(static_cast<std::size_t>(first.layout.count)));
    std::string speakers;
    for (const auto index : map) {
        speakers += ac3::eac3::chanmap::name(first.layout[static_cast<int>(index)]);
        speakers += ' ';
    }
    std::println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                 units->size(), first.substream_count, out_path);
    std::println(status, "  {} channels, {} Hz: {}", map.size(), sample_rate_hz(first.sample_rate),
                 speakers);
    print_drc_summary(status, dynrng_min_db, dynrng_max_db, compr_min_db, compr_max_db,
                      compr_frames, meta);
    return report_decoded_objects(status, first.object_metadata, have_object_audio,
                                  objects_written, objects_dir);
}

int run_decode(std::string_view in_path, std::string_view out_path, const Options& meta,
               std::string_view objects_dir) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    if (!apply_object_verification(stream, meta)) {
        return 1;
    }
    // bsid at bit 40 says which syntax this is, before either is assumed.
    // spdif and play branch on it the same way now that both packers handle
    // E-AC-3 (Eac3BurstPacker alongside AC-3's wrap_frame).
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        std::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return 1;
    }
    if (*bsid > 8) {
        return run_decode_eac3(stream, out_path, meta, objects_dir);
    }
    if (!objects_dir.empty()) {
        std::println(stderr,
                     "warning: objects_dir given but {} is plain AC-3 - it has no object layer",
                     in_path);
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::println(stderr, "error: {}: {}", in_path, ac3::describe(frames.error()));
        return 1;
    }
    ac3::FrameDecoder decoder{
        {.drc_scale = meta.drc_scale, .heavy_compression = meta.p.heavy.has_value()}};
    PlanarWavSink sink;
    std::optional<ac3::analysis::LevelMeter> meter;
    ac3::DecodedFrame first{};
    bool have_first = false;
    // What the stream actually carried, reported whether or not it was applied.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            std::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
            sink.abort();
            return 1;
        }
        for (const auto word : decoded->dynrng) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
        if (decoded->compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*decoded->compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
        if (!have_first) {
            first = *decoded;
            // The channel permutation the whole-buffer write used to apply
            // at the end is fixed from the first frame's layout - the same
            // values, just needed up front now that samples leave as they
            // decode.
            if (!sink.open(out_path, sample_rate_hz(decoded->sample_rate),
                           decoded->channels.size(),
                           ac3::io::wav_channel_order(decoded->acmod, decoded->lfe))) {
                std::println(stderr, "error: cannot open {} for writing", out_path);
                return 1;
            }
            meter.emplace(decoded->acmod, decoded->lfe, sample_rate_hz(decoded->sample_rate));
            have_first = true;
        }
        std::vector<std::span<const float>> views;
        views.reserve(decoded->channels.size());
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            if (!sink.append(ch, decoded->channels[ch])) {
                std::println(stderr, "error: cannot write to {}", out_path);
                sink.abort();
                return 1;
            }
            views.emplace_back(decoded->channels[ch]);
        }
        // have_first gates meter.emplace() a few lines up, in this same
        // iteration on the first pass and an earlier one on every pass
        // after, so meter is always engaged by the time this line runs.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        meter->process(views);
    }
    if (!have_first) {
        std::println(stderr, "error: no frames");
        return 1;
    }
    const auto written = sink.close();
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the WAV bytes just written above already own stdout in that case,
    // and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    std::println(status, "decoded {} frames -> {} ({}, {} Hz)", frames->size(), out_path,
                 ac3::analysis::layout_name(first.acmod, first.lfe),
                 sample_rate_hz(first.sample_rate));
    std::println(status, "metadata: dialnorm {} (dialogue at -{} dBFS)", first.dialnorm,
                 first.dialnorm);
    if (first.dialnorm2) {
        std::println(status, "          dialnorm2 {} (Ch2, dialogue at -{} dBFS){}",
                     *first.dialnorm2, *first.dialnorm2, first.compr2 ? ", compr2 present" : "");
    }
    std::println(status, "          dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                 meta.drc_scale != 0.0 ? std::format(", applied at scale {}", meta.drc_scale)
                                       : ", not applied");
    if (compr_frames > 0) {
        std::println(status, "          compr  {:+.2f} .. {:+.2f} dB over {} frames{}",
                     compr_min_db, compr_max_db, compr_frames,
                     meta.p.heavy ? ", applied" : ", not applied");
    } else {
        std::println(status, "          compr  absent");
    }
    // The have_first check above already returned if the frame loop never
    // ran, and it is that same loop's first iteration that emplaces meter.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    print_channel_summary(*meter, status);
    return 0;
}

// --- qc (roadmap C2) --------------------------------------------------------
// Bitstream-aware loudness QC: decode a whole stream, measure it with the
// real BS.1770-4/EBU Tech 3342 meter (the same ac3::meta::LoudnessMeter
// dialnorm=auto already uses), and compare the result against what the
// stream's own dialnorm/compr claim and, optionally, a named delivery-spec
// gate (ac3::meta::qc_preset - see ac3/meta/qc.hpp for the cited sources).

// One decoded programme this command measures and reports on - the whole
// soundfield for every layout except 1+1 dual mono, which is two of these
// (Ch1, Ch2): §E1.3 makes them unrelated, unmixed programmes sharing one
// syncframe rather than a single soundfield BS.1770 could measure as one, the
// same reason measured_dialnorm_channel exists alongside measured_dialnorm
// above.
struct QcProgrammeResult {
    std::string_view label = {};  // "" (whole programme) or "Ch1"/"Ch2" for 1+1
    // Every field below has an explicit default member initializer, even the
    // ones std::optional's own default constructor would already give -
    // every construction of this type in this file is a PARTIAL designated
    // initializer (only the fields relevant at that call site named), and
    // GCC's -Wmissing-field-initializers (on under -Wextra, and this project
    // builds -Werror) fires on any member without one, regardless of what
    // its type's own default constructor would produce.
    std::optional<double> integrated_lkfs = std::nullopt;
    std::optional<double> lra_lu = std::nullopt;
    std::optional<double> true_peak_dbtp = std::nullopt;
    int dialnorm = 31;
    std::optional<std::uint8_t> compr = std::nullopt;
};

struct QcResult {
    std::string_view codec_label;  // "AC-3" / "E-AC-3"
    std::string_view unit_label;   // "frame(s)" / "access unit(s)"
    std::string layout_label;
    std::uint32_t sample_rate_hz = 0;
    std::size_t unit_count = 0;
    double seconds = 0.0;
    std::vector<QcProgrammeResult> programmes;
};

// AC-3 (bsid <= 8): straightforward per-frame decode, same loop shape as
// run_decode above, feeding ac3::meta::LoudnessMeter instead of accumulating
// PCM - qc never writes audio out, so there is nothing to buffer.
std::optional<QcResult> measure_qc_ac3(std::span<const std::byte> stream) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::println(stderr, "error: not a valid AC-3 stream");
        return std::nullopt;
    }
    ac3::FrameDecoder decoder;
    QcResult result;
    result.codec_label = "AC-3";
    result.unit_label = "frame(s)";
    result.unit_count = frames->size();

    bool have_first = false;
    bool dual_mono = false;
    std::optional<ac3::meta::LoudnessMeter> meter;      // whole programme
    std::optional<ac3::meta::LoudnessMeter> meter_ch1;  // dual mono only
    std::optional<ac3::meta::LoudnessMeter> meter_ch2;

    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            std::println(stderr, "error: {}", ac3::describe(decoded.error()));
            return std::nullopt;
        }
        if (!have_first) {
            have_first = true;
            dual_mono = decoded->acmod == ac3::Acmod::kDualMono;
            result.sample_rate_hz = sample_rate_hz(decoded->sample_rate);
            if (dual_mono) {
                result.layout_label = "1+1 dual mono";
                meter_ch1.emplace(decoded->sample_rate, ac3::Acmod::k1_0, false);
                meter_ch2.emplace(decoded->sample_rate, ac3::Acmod::k1_0, false);
                result.programmes.push_back(
                    QcProgrammeResult{.label = "Ch1", .dialnorm = decoded->dialnorm,
                                      .compr = decoded->compr});
                result.programmes.push_back(QcProgrammeResult{
                    .label = "Ch2", .dialnorm = decoded->dialnorm2.value_or(31),
                    .compr = decoded->compr2});
            } else {
                result.layout_label =
                    std::string{ac3::analysis::layout_name(decoded->acmod, decoded->lfe)};
                meter.emplace(decoded->sample_rate, decoded->acmod, decoded->lfe);
                result.programmes.push_back(
                    QcProgrammeResult{.dialnorm = decoded->dialnorm, .compr = decoded->compr});
            }
        }
        if (dual_mono) {
            const std::array<std::span<const float>, 1> ch1{decoded->channels[0]};
            const std::array<std::span<const float>, 1> ch2{decoded->channels[1]};
            meter_ch1->push(ch1);
            meter_ch2->push(ch2);
        } else {
            std::vector<std::span<const float>> views;
            views.reserve(decoded->channels.size());
            for (const auto& channel : decoded->channels) {
                views.emplace_back(channel);
            }
            meter->push(views);
        }
    }
    if (dual_mono) {
        result.programmes[0].integrated_lkfs = meter_ch1->integrated_lkfs();
        result.programmes[0].lra_lu = meter_ch1->loudness_range();
        result.programmes[0].true_peak_dbtp = meter_ch1->true_peak_dbtp();
        result.programmes[1].integrated_lkfs = meter_ch2->integrated_lkfs();
        result.programmes[1].lra_lu = meter_ch2->loudness_range();
        result.programmes[1].true_peak_dbtp = meter_ch2->true_peak_dbtp();
    } else {
        result.programmes[0].integrated_lkfs = meter->integrated_lkfs();
        result.programmes[0].lra_lu = meter->loudness_range();
        result.programmes[0].true_peak_dbtp = meter->true_peak_dbtp();
    }
    result.seconds = static_cast<double>(result.unit_count) *
                     static_cast<double>(ac3::kSamplesPerFrame) /
                     static_cast<double>(result.sample_rate_hz);
    return result;
}

// E-AC-3 (bsid 11-16): measures the INDEPENDENT substream's own bed audio
// only, never a dependent's - the same "bed acmod/lfe, never the wider
// rendered layout" scope run_encode/run_eac3_encode's own pre-encode
// measured_dialnorm(cp.bed_acmod, cp.bed_lfe, ...) pass already uses (see
// above). BS.1770's channel weighting is defined over Table 5.8 acmod/lfe,
// which a dependent substream's own extension channels (height, wide, Ts,
// etc.) are not members of - the bed is always a Table 5.8 layout, so
// measuring it is what makes this comparable to the encoder's own dialnorm
// derivation for the identical programme. Dual mono (1+1) is always a lone
// independent substream with no dependents (decoder.hpp's own doc comment on
// DecodedAccessUnit), so the same independent-substream-only filtering
// naturally covers it too, exactly like the AC-3 path above.
//
// Walked at the raw-syncframe level (ac3::split_frames, NOT split_access_units
// - decoder.hpp's own doc comment on split_frames says it "handles both
// generations"), calling Eac3Decoder::decode_substream directly on every
// frame so dependent-substream frames are still decoded (consuming their own
// overlap-add state and catching any parse error) even though this only ever
// measures what comes back independent.
std::optional<QcResult> measure_qc_eac3(std::span<const std::byte> stream) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::println(stderr, "error: not a valid E-AC-3 stream");
        return std::nullopt;
    }
    ac3::Eac3Decoder decoder;
    QcResult result;
    result.codec_label = "E-AC-3";
    result.unit_label = "access unit(s)";

    bool have_first = false;
    bool dual_mono = false;
    std::optional<ac3::meta::LoudnessMeter> meter;
    std::optional<ac3::meta::LoudnessMeter> meter_ch1;
    std::optional<ac3::meta::LoudnessMeter> meter_ch2;

    // Shared by the main decode loop below and the end-of-stream flush() -
    // both hand this a released, independent-or-dependent DecodedSubstream;
    // only an independent one is ever measured (see this function's own
    // comment above).
    auto ingest = [&](const ac3::DecodedSubstream& sub) {
        if (sub.strmtyp == ac3::eac3::StreamType::kDependent) {
            return;
        }
        if (!have_first) {
            have_first = true;
            dual_mono = sub.acmod == ac3::Acmod::kDualMono;
            result.sample_rate_hz = sample_rate_hz(sub.sample_rate);
            if (dual_mono) {
                result.layout_label = "1+1 dual mono";
                meter_ch1.emplace(sub.sample_rate, ac3::Acmod::k1_0, false);
                meter_ch2.emplace(sub.sample_rate, ac3::Acmod::k1_0, false);
                result.programmes.push_back(QcProgrammeResult{
                    .label = "Ch1", .dialnorm = sub.dialnorm, .compr = sub.compr});
                result.programmes.push_back(QcProgrammeResult{
                    .label = "Ch2", .dialnorm = sub.dialnorm2.value_or(31), .compr = sub.compr2});
            } else {
                result.layout_label = std::string{ac3::analysis::layout_name(sub.acmod, sub.lfe)};
                meter.emplace(sub.sample_rate, sub.acmod, sub.lfe);
                result.programmes.push_back(
                    QcProgrammeResult{.dialnorm = sub.dialnorm, .compr = sub.compr});
            }
        }
        ++result.unit_count;
        if (dual_mono) {
            const std::array<std::span<const float>, 1> ch1{sub.channels[0]};
            const std::array<std::span<const float>, 1> ch2{sub.channels[1]};
            meter_ch1->push(ch1);
            meter_ch2->push(ch2);
        } else {
            std::vector<std::span<const float>> views;
            views.reserve(sub.channels.size());
            for (const auto& channel : sub.channels) {
                views.emplace_back(channel);
            }
            meter->push(views);
        }
    };

    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_substream(frame);
        if (!decoded) {
            std::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            return std::nullopt;
        }
        // §3.7: this substream's frame is being held back pending transient
        // pre-noise processing (Eac3Decoder::decode_substream's own doc
        // comment) - nothing new to ingest yet, not an error.
        if (decoded->has_value()) {
            ingest(**decoded);
        }
    }
    // Whatever transient pre-noise processing was still holding back at
    // end-of-stream, same convention run_decode_eac3 follows.
    for (const auto& sub : decoder.flush()) {
        ingest(sub);
    }

    if (!have_first) {
        std::println(stderr, "error: no independent substream frames");
        return std::nullopt;
    }
    if (dual_mono) {
        result.programmes[0].integrated_lkfs = meter_ch1->integrated_lkfs();
        result.programmes[0].lra_lu = meter_ch1->loudness_range();
        result.programmes[0].true_peak_dbtp = meter_ch1->true_peak_dbtp();
        result.programmes[1].integrated_lkfs = meter_ch2->integrated_lkfs();
        result.programmes[1].lra_lu = meter_ch2->loudness_range();
        result.programmes[1].true_peak_dbtp = meter_ch2->true_peak_dbtp();
    } else {
        result.programmes[0].integrated_lkfs = meter->integrated_lkfs();
        result.programmes[0].lra_lu = meter->loudness_range();
        result.programmes[0].true_peak_dbtp = meter->true_peak_dbtp();
    }
    result.seconds = static_cast<double>(result.unit_count) *
                     static_cast<double>(ac3::kSamplesPerFrame) /
                     static_cast<double>(result.sample_rate_hz);
    return result;
}

// Prints one programme's measurement (the empty-label whole-programme case,
// or "Ch1"/"Ch2" for 1+1 dual mono) and, if `preset_arg` names one (or
// "all"), checks it against the requested preset(s). Returns true iff every
// requested gate passed (or none was requested at all) - run_qc's own exit
// code is exactly this, ANDed across every programme it reports.
bool report_qc_programme(const QcProgrammeResult& p, const std::optional<std::string>& preset_arg) {
    const std::string heading = p.label.empty() ? std::string{} : std::format("{}: ", p.label);
    std::println("{}measured (BS.1770-4 gated / EBU Tech 3342 / BS.1770-4 Annex 2):", heading);
    if (p.integrated_lkfs) {
        std::println("  integrated loudness  {:>+8.2f} LKFS", *p.integrated_lkfs);
        std::println("  loudness range       {}", p.lra_lu ? std::format("{:>7.2f} LU", *p.lra_lu)
                                                             : std::string{"n/a"});
    } else {
        std::println("  integrated loudness  no audio above the -70 LKFS absolute gate");
        std::println("  loudness range       n/a");
    }
    std::println("  true peak            {}",
                 p.true_peak_dbtp ? std::format("{:>+8.2f} dBTP", *p.true_peak_dbtp)
                                   : std::string{"n/a"});
    std::println("{}embedded metadata:", heading);
    std::println("  dialnorm             {:>3}  (claims dialogue at {:.2f} LKFS)", p.dialnorm,
                 -static_cast<double>(p.dialnorm));
    if (p.compr) {
        std::println("  compr                present, {:+.2f} dB",
                     ac3::meta::to_db(ac3::meta::compr_gain(*p.compr)));
    } else {
        std::println("  compr                absent");
    }
    if (p.integrated_lkfs) {
        // §5.4.2.8: dialnorm states how far dialogue sits below digital
        // 100%, so the stream's own claimed programme level is simply its
        // negation - delta is measured minus that claim, positive meaning
        // the real programme is louder than dialnorm says.
        const double claimed_lkfs = -static_cast<double>(p.dialnorm);
        const double delta = *p.integrated_lkfs - claimed_lkfs;
        const int implied = ac3::meta::dialnorm_from_lkfs(*p.integrated_lkfs);
        std::println("{}dialnorm check:", heading);
        std::println("  claimed              {:>+8.2f} LKFS  (from dialnorm {})", claimed_lkfs,
                     p.dialnorm);
        std::println("  delta                {:>+8.2f} dB    (measured - claimed; positive = "
                     "measured is louder)",
                     delta);
        std::println("  measurement-derived dialnorm would be {}{}", implied,
                     implied == p.dialnorm ? " (matches)" : std::format(", not {}", p.dialnorm));
    }

    if (!preset_arg) {
        return true;
    }
    bool all_pass = true;
    std::println("{}gates:", heading);
    const auto check_one = [&](ac3::meta::QcPresetId id) {
        const auto preset = ac3::meta::qc_preset(id);
        const auto name = ac3::meta::qc_preset_name(id);
        const auto verdict = ac3::meta::evaluate_qc_gate(preset, p.integrated_lkfs, p.true_peak_dbtp);
        std::println("  {}:", name);
        if (p.integrated_lkfs) {
            std::println("    loudness   target {:+.1f} +/-{:.1f} LKFS   measured {:+.2f} LKFS   "
                         "delta {:+.2f} LU   {}",
                         preset.target_lkfs, preset.tolerance_lu, *p.integrated_lkfs,
                         *verdict.loudness_delta_lu, verdict.loudness_pass ? "PASS" : "FAIL");
        } else {
            std::println("    loudness   target {:+.1f} +/-{:.1f} LKFS   measured n/a   FAIL",
                         preset.target_lkfs, preset.tolerance_lu);
        }
        if (p.true_peak_dbtp) {
            std::println("    true peak  limit  <= {:+.1f} dBTP        measured {:+.2f} dBTP        "
                         "{}",
                         preset.max_true_peak_dbtp, *p.true_peak_dbtp,
                         verdict.true_peak_pass ? "PASS" : "FAIL");
        } else {
            std::println("    true peak  limit  <= {:+.1f} dBTP        measured n/a   FAIL",
                         preset.max_true_peak_dbtp);
        }
        std::println("    verdict: {}", verdict.pass() ? "PASS" : "FAIL");
        if (!verdict.pass()) {
            all_pass = false;
        }
    };
    if (*preset_arg == "all") {
        for (const auto id : ac3::meta::kQcPresetIds) {
            check_one(id);
        }
    } else {
        ac3::meta::QcPresetId id{};
        if (ac3::meta::parse_qc_preset(*preset_arg, id)) {
            check_one(id);
        } else {
            // parse_options already validates preset= against
            // kQcPresetNames/"all" before dispatch ever reaches here (see
            // its own "preset" handling) - kept as a defensive fallback
            // rather than an assert, since main.cpp has no
            // exception-based unreachable() convention of its own.
            std::println(stderr, "error: unknown qc preset '{}'", *preset_arg);
            all_pass = false;
        }
    }
    return all_pass;
}

int run_qc(std::string_view in_path, const std::optional<std::string>& preset_arg) {
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
    const auto result = *bsid > 8 ? measure_qc_eac3(stream) : measure_qc_ac3(stream);
    if (!result) {
        return 1;
    }
    std::println("qc: {} ({}, {}, {} Hz, {} {}, {:.2f} s)", in_path, result->codec_label,
                 result->layout_label, result->sample_rate_hz, result->unit_count,
                 result->unit_label, result->seconds);
    bool all_pass = true;
    for (const auto& programme : result->programmes) {
        if (!report_qc_programme(programme, preset_arg)) {
            all_pass = false;
        }
    }
    return all_pass ? 0 : 1;
}

// E-AC-3's own level report. The rendered layout is a chanmap rather than an
// acmod, so it cannot go through LevelMeter's Table 5.8 naming; the figures
// still come from ac3::analysis, so a level reads the same here as anywhere.
int run_levels_eac3(std::span<const std::byte> stream, std::string_view in_path) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        std::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
        return 1;
    }
    ac3::Eac3Decoder decoder;
    std::vector<ac3::analysis::ChannelSummary> totals;
    ac3::DecodedAccessUnit first{};
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            std::println(stderr, "error: {}: decode failed (code {})", in_path,
                         static_cast<int>(decoded.error()));
            return 1;
        }
        if (!decoded->has_value()) {
            // §3.7: held back pending transient pre-noise processing
            // (Eac3Decoder::decode_access_unit's own doc comment) - this
            // report accepts losing the very last frame's stats to that
            // rather than draining decoder.flush() for a metering tool.
            continue;
        }
        const auto& out = **decoded;
        if (totals.empty()) {
            first = out;
            totals.resize(out.channels.size());
            std::println("{}: {} access units, {} substreams each, {} channels, {} Hz",
                         in_path, units->size(), out.substream_count,
                         out.channels.size(), sample_rate_hz(out.sample_rate));
        }
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            auto& stats = totals[ch];
            for (const float sample : out.channels[ch]) {
                const double magnitude = std::abs(static_cast<double>(sample));
                stats.peak = std::max(stats.peak, magnitude);
                stats.sum_squares += magnitude * magnitude;
                ++stats.samples;
                if (magnitude >= static_cast<double>(ac3::analysis::kFullScale)) {
                    ++stats.clipped_samples;
                }
            }
        }
    }
    std::println("");
    std::println("per-channel levels:");
    std::println("  {:<6} {:>8} {:>8}  {:<20} {}", "ch", "peak", "rms", "peak (-60..0 dBFS)",
                 "clipped");
    // Dual mono has no Table E2.5 location - `layout` is left empty for
    // exactly that case (see decode_access_unit) - so Ch1/Ch2 name themselves
    // by coded position instead of a speaker name that would not apply.
    const bool dual_mono = first.acmod == ac3::Acmod::kDualMono;
    for (std::size_t ch = 0; ch < totals.size(); ++ch) {
        const auto& stats = totals[ch];
        const std::string name = dual_mono ? std::format("Ch{}", ch + 1)
                                           : std::string{ac3::eac3::chanmap::name(
                                                 first.layout[static_cast<int>(ch)])};
        std::println("  {:<6} {:>8.2f} {:>8.2f}  [{}] {}", name, stats.peak_db(),
                     stats.rms_db(), meter_bar(stats.peak_db(), 18),
                     stats.clipped_samples > 0 ? std::to_string(stats.clipped_samples) : "-");
    }
    return 0;
}

// What is actually in a file, channel by channel — the answer both front ends
// are built to show, without having to encode anything to get it.
int run_levels(std::string_view in_path) {
    const auto bytes = read_all(in_path);
    if (bytes.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    // A syncframe opens with 0x0B77 (§5.4.1.1); anything else is treated as a
    // WAV, whose reader reports its own diagnosis if it is neither.
    const bool syncword = bytes.size() >= 6 && std::to_integer<int>(bytes[0]) == 0x0B &&
                          std::to_integer<int>(bytes[1]) == 0x77;

    if (syncword) {
        // E-AC-3 has its own decoder here now, so this is no longer a wall to
        // turn a wider syntax away at - bsid only decides which reader runs.
        const auto bsid = ac3::stream_bsid(bytes);
        if (bsid && *bsid > 8) {
            return run_levels_eac3(bytes, in_path);
        }
        const auto frames = ac3::split_frames(bytes);
        if (!frames || frames->empty()) {
            std::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return 1;
        }
        ac3::FrameDecoder decoder;
        std::optional<ac3::analysis::LevelMeter> meter;
        for (const auto& frame : *frames) {
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded) {
                std::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                return 1;
            }
            if (!meter) {
                meter.emplace(decoded->acmod, decoded->lfe,
                              sample_rate_hz(decoded->sample_rate));
                std::println("{}: {} frames, {}, {} kbps, {} Hz", in_path, frames->size(),
                             ac3::analysis::layout_name(decoded->acmod, decoded->lfe),
                             decoded->bitrate_kbps, sample_rate_hz(decoded->sample_rate));
            }
            std::vector<std::span<const float>> views;
            views.reserve(decoded->channels.size());
            for (const auto& channel : decoded->channels) {
                views.emplace_back(channel);
            }
            // meter is engaged by the !meter check a few lines up, in this
            // same iteration on the first pass and an earlier one thereafter.
            // clang-tidy's bugprone-unchecked-optional-access and MSVC
            // /analyze's C26829 both flag it anyway: neither does the
            // cross-iteration reasoning needed to see it's always engaged
            // by the time this runs. #pragma warning(suppress: 26829) would
            // silence /analyze too, but it is not a portable pragma - GCC/
            // clang both treat an unrecognized #pragma as -Wunknown-pragmas,
            // and this project builds with -Werror, so emitting it here
            // would fail every non-MSVC leg. The C26829 alert on both this
            // line and the one below is dismissed separately with this same
            // justification instead.
            meter->process(views); // NOLINT(bugprone-unchecked-optional-access)
        }
        // The `!frames || frames->empty()` check above guarantees the loop
        // ran at least once, and its first iteration always emplaces meter.
        print_channel_summary(*meter); // NOLINT(bugprone-unchecked-optional-access)
        return 0;
    }

    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
    if (!layout) {
        std::println(stderr, "error: levels handles 1 to 6 channels ({} given)",
                     wav->channels.size());
        return 1;
    }
    const double seconds = wav->sample_rate > 0
                               ? static_cast<double>(wav->frame_count()) / wav->sample_rate
                               : 0.0;
    std::println("{}: {} Hz, {:.2f} s, shown in A/52 order as {}", in_path, wav->sample_rate,
                 seconds, ac3::analysis::layout_name(layout->acmod, layout->lfe));

    ac3::analysis::LevelMeter meter{layout->acmod, layout->lfe, wav->sample_rate};
    std::vector<std::span<const float>> views(layout->wav_index.size());
    for (std::size_t ch = 0; ch < layout->wav_index.size(); ++ch) {
        views[ch] = wav->channels[layout->wav_index[ch]];
    }
    meter.process(views);
    print_channel_summary(meter);
    return 0;
}

// Measure a WAV and report what dialnorm it implies. §5.4.2.8 wants dialogue
// level below full scale and A/52 predates any standard way to measure it;
// BS.1770 gated loudness is the modern answer, so this is the number the
// encoder would put on the stream for dialnorm=auto.
int run_loudness(std::string_view in_path) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    ac3::SampleRate sr{};
    switch (wav->sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            std::println(stderr, "error: sample rate {} is not legal for AC-3", wav->sample_rate);
            return 1;
    }
    // The BS.1770 channel weighting depends on which coded positions are
    // surrounds, so the layout has to be inferred from the channel count
    // (Table 5.8) rather than assumed.
    const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
    if (!layout) {
        std::println(stderr, "error: {} channels is not an AC-3 layout",
                     wav->channels.size());
        return 1;
    }
    const auto dialnorm = measured_dialnorm(*wav, sr, layout->acmod, layout->lfe);
    if (!dialnorm) {
        std::println("no audio above the -70 LKFS absolute gate: loudness undefined");
        return 1;
    }
    // Reporting the answer was missing where this came from, so the command
    // measured the programme and then said nothing about it.
    std::println("{}: {} Hz, {}", in_path, wav->sample_rate,
                 ac3::analysis::layout_name(layout->acmod, layout->lfe));
    std::println("  dialogue level -{} LKFS -> dialnorm {}", *dialnorm, *dialnorm);
    return 0;
}

// Wrap a raw AC-3 stream into IEC 61937 bursts inside a PCM16 stereo WAV:
// played BIT-EXACTLY (volume 100%, no mixing) into an S/PDIF or HDMI output,
// a receiver locks onto the bursts and lights up "Dolby Digital".
// AC-3 frames wrap one-to-one; an E-AC-3 access unit may need several
// consecutive ones to fill a burst (Eac3BurstPacker accumulates internally).
// Shared by run_spdif and run_play so the two cannot disagree about how a
// stream becomes bursts.
std::optional<std::vector<std::byte>> wrap_ac3_stream(std::span<const std::byte> stream,
                                                       std::uint32_t& rate_out) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        return std::nullopt;
    }
    const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
    rate_out = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));

    std::vector<std::byte> payload;
    payload.reserve(frames->size() * ac3::iec61937::kBurstBytes);
    for (const auto& frame : *frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        if (!burst) {
            return std::nullopt;
        }
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    return payload;
}

std::optional<std::vector<std::byte>> wrap_eac3_stream(std::span<const std::byte> stream,
                                                        std::uint32_t& rate_out) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        return std::nullopt;
    }
    const auto byte4 = std::to_integer<std::uint32_t>((*units)[0][4]);
    rate_out = sample_rate_hz(static_cast<ac3::SampleRate>(byte4 >> 6));

    std::vector<std::byte> payload;
    ac3::iec61937::Eac3BurstPacker packer;
    for (const auto& unit : *units) {
        const auto burst = packer.push(unit);
        if (!burst) {
            return std::nullopt;
        }
        if (*burst) {
            payload.insert(payload.end(), (**burst).begin(), (**burst).end());
        }
    }
    return payload;
}

int run_spdif(std::string_view in_path, std::string_view out_path) {
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

    std::uint32_t content_rate = 0;
    const auto payload = eac3 ? wrap_eac3_stream(stream, content_rate)
                              : wrap_ac3_stream(stream, content_rate);
    if (!payload) {
        std::println(stderr, "error: {} is not a valid {} stream", in_path,
                     eac3 ? "E-AC-3" : "AC-3");
        return 1;
    }
    // The WAV carrier itself runs at 4x the content rate for E-AC-3 (Dolby
    // Digital Plus over IEC 60958/61937 - Microsoft's "Representing Formats
    // for IEC 61937 Transmissions"), matching WASAPI's make_eac3_format.
    const auto carrier_rate = eac3 ? content_rate * 4 : content_rate;
    const auto written =
        ac3::io::write_wav_pcm16_raw(std::string{out_path}, *payload, carrier_rate, 2);
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::println("wrapped {} into IEC 61937 bursts -> {} ({} Hz carrier)",
                 eac3 ? "E-AC-3 access units" : "AC-3 frames", out_path, carrier_rate);
    std::println("play bit-exactly (100% volume, exclusive/passthrough output) to light up");
    std::println("a receiver's Dolby Digital{} indicator.", eac3 ? " Plus" : "");
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

// Stream an AC-3 or E-AC-3 file to a receiver in real time via exclusive-mode
// IEC 61937. The sink's render thread pulls bursts; this loop keeps it fed.
// bsid picks the branch: AC-3 wraps one frame per burst, E-AC-3 wraps one
// access unit at a time through a persistent Eac3BurstPacker, which may hold
// bytes back until enough have accumulated to fill a burst (see
// Eac3BurstPacker's own comment on why - Annex E frames can cover as few as
// one of the six blocks a burst period spans).
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


int run_eac3_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                     std::string_view layout, const Options& meta) {
    plan::Plan p{.codec = plan::Codec::kEac3, .bitrate_kbps = bitrate, .meta = meta.p};
    std::string label;
    if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }
    const auto unit = ac3::eac3::build_silent_access_unit(plan::eac3_config(p));
    if (!unit) {
        std::println(stderr, "error: invalid E-AC-3 configuration");
        return 1;
    }
    const std::uint64_t count = frame_count(seconds);
    if (!write_repeated_frame(out_path, unit->bytes, count)) {
        return 1;
    }
    std::println("wrote {} silent E-AC-3 {} access units ({} substreams, "
                 "{} bytes each, bsid 16) to {}",
                 count, label, unit->substream_count(), unit->bytes.size(), out_path);
    return 0;
}

// Decode a file back to PCM and play it on an ordinary (shared-mode, not
// bitstreamed) output - a sanity-check/preview path, and the offline half of
// live monitoring ('live's --monitor equivalent works the same way, one
// access unit at a time as it is produced instead of read from a file).
// Object metadata (JOC/OAMD), when present, is reported (object count) but
// not played or exported here: Eac3Decoder reads TS 103 420's object layer
// (DecodedSubstream::object_metadata/object_audio), but this path only plays
// the 5.1 bed - exactly what a legacy decoder hears, which is the thing most
// worth confirming actually sounds right. 'ac3cli decode' with objects_dir
// is where the reconstructed object audio itself comes out.
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

// Live capture -> live encode -> optionally live monitor and/or live IEC
// 61937 passthrough, running continuously and also writing the encoded
// access units to a file (so a live session leaves an artifact the way
// 'record' always has). This is the command 'record' is not: 'record' only
// ever reaches a file.
//
// mode "atmos" additionally moves each object's placement every frame from
// elapsed wall-clock time, using the same orbiting math run_atmos's
// synthetic demo uses - the concrete shape a real per-frame live position
// source (a separate, parallel piece of work) drops into once it lands: swap
// the orbit-angle expression below for a read of wherever that source keeps
// its current position, still evaluated fresh every frame inside this same loop.
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

// ---------------------------------------------------------------------------
// The command table. Every command is one row: its name, how many positional
// arguments it needs, the argument spec the usage text prints, and the code
// that runs it.
//
// It replaces a chain of `if (command == ...)` comparisons, each of which
// repeated `args.size() > N ? parse(args[N]) : default` for every parameter.
// That repetition is where this file kept going wrong: consolidating six
// parallel branches turned up SIX argv faults of exactly one shape - an entry
// counting from the wrong base, or reading a slot it had not checked. Two
// would have written output to a file named after the duration. None was
// visible in a build or a unit test, because the indices are only wrong
// relative to a convention nothing states in one place.
//
// Here the convention is stated once: args[0] is the command, so args[1] is
// the first parameter, and min_args is checked before any handler runs.
// print_usage() is generated from the same rows, so the help cannot drift
// from what dispatch accepts - it already had, with eac3-silence and
// eac3-sine missing from it entirely.
// ---------------------------------------------------------------------------

struct Args {
    std::span<char* const> a;
    const Options& meta;
    bool couple;

    [[nodiscard]] std::string_view str(std::size_t i, std::string_view fallback = {}) const {
        return i < a.size() ? std::string_view{a[i]} : fallback;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t i, std::uint32_t fallback) const {
        return i < a.size() ? parse_u32_or(a[i], fallback) : fallback;
    }
    // Signed, unlike u32: routing a negative token through parse_u32_or (which
    // parses unsigned) always fails and silently returns 0 rather than
    // `fallback` - the wrong answer for the sentinel values several commands
    // read as "unset" or "default" (e.g. play's device index). from_chars
    // for a signed int accepts the leading '-' directly, so this parses
    // the token itself instead of bouncing through the unsigned path.
    [[nodiscard]] int i32(std::size_t i, int fallback) const {
        if (i >= a.size()) {
            return fallback;
        }
        const std::string_view text{a[i]};
        int value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
    }
};

// What a command needs beyond plain file I/O to run at all in THIS build. Most commands need
// nothing. Several need the machine's audio hardware; one (atmos-adm) needs a library that is not
// part of every build - either way, unmet() below answers with the same {available, reason} shape
// (ac3::audio::Capability), so dispatch and the usage listing treat both kinds of "not here"
// identically.
//
// This is a column in the table rather than a check inside each handler for
// the same reason min_args is: stated once, beside the command it describes,
// and read by both dispatch and the usage text so the two cannot disagree
// about which commands exist here.
//
// 'live' needs kCapture, not a new combined category: capture is the one
// hard requirement (no capture endpoint, no session at all), while its
// monitor/passthrough legs are soft - unavailable or refused there degrades
// to a warning and a file-only session, exactly like plugging into a
// receiver that says no. Only 'monitor' (which does nothing BUT play back)
// needs kMonitor as a hard gate, the same way 'play'/'outputs' need
// kPassthrough.
//
// kAdm ('atmos-adm', roadmap B1 phase 3): unlike the three audio ones, this is not a hardware
// question - it is whether ac3adm::ac3adm/ac3::admbridge were linked into this build at all
// (AC3FORGE_BUILD_ADM, default OFF - see the root CMakeLists.txt's own option()). Answered the
// same way regardless: adm/atmos_adm.hpp's ac3cli::adm_capability(), backed by exactly one of
// adm/enabled/atmos_adm.cpp or adm/disabled/atmos_adm.cpp (see run_atmos_adm's own comment for
// why a CMake-selected file, not a preprocessor conditional, decides this).
enum class Needs : std::uint8_t { kNothing, kCapture, kPassthrough, kMonitor, kAdm };

// The unmet requirement, or nullptr when this build/platform can satisfy it.
//
// Note what kCapture/kPassthrough/kMonitor are not: an OS test. main.cpp never asks whether it is
// on Windows - it asks the one translation unit CMake compiled from
// src/audio/src/backend/<os>/ what that backend can do, and prints the answer
// that unit supplied. The day a Unix capture backend lands, capture flips to
// available in that file alone and 'devices' and 'record' start working here
// with no change to this file. kAdm asks the analogous question of
// adm/{enabled,disabled}/atmos_adm.cpp instead - a library-linked-or-not fact rather than an
// OS one, answered by the identical "ask the compiled-in file" shape.
const ac3::audio::Capability* unmet(Needs needs) {
    const auto& backend = ac3::audio::audio_backend();
    switch (needs) {
        case Needs::kNothing: return nullptr;
        case Needs::kCapture: return backend.capture.available ? nullptr : &backend.capture;
        case Needs::kPassthrough:
            return backend.passthrough.available ? nullptr : &backend.passthrough;
        case Needs::kMonitor: return backend.monitor.available ? nullptr : &backend.monitor;
        case Needs::kAdm: {
            const auto& adm = ac3cli::adm_capability();
            return adm.available ? nullptr : &adm;
        }
    }
    return nullptr;
}

struct Command {
    std::string_view name;
    std::size_t min_args;  // positional count INCLUDING the command itself
    std::string_view spec;
    std::string_view note;
    Needs needs;
    int (*run)(const Args&);
};

// 26 commands, always - including atmos-adm, whether or not AC3FORGE_BUILD_ADM linked
// ac3adm::ac3adm/ac3::admbridge into this particular build (see Needs::kAdm/unmet() above and
// run_atmos_adm's own comment): a command this build cannot run is listed with Needs gating it,
// never sized out of the table entirely - the identical "listed, not hidden" treatment
// kCapture/kPassthrough/kMonitor commands already get (see print_usage()'s own comment below on
// why hiding would be a lie about a command that exists and would work elsewhere).
constexpr std::array<Command, 26> kCommands{{
    {"silence", 2, "<out.ac3> [seconds] [bitrate_kbps]", "", Needs::kNothing,
     [](const Args& x) { return run_silence(x.str(1), x.u32(2, 5), x.u32(3, 192)); }},
    {"sine", 2, "<out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "",
     Needs::kNothing,
     [](const Args& x) {
         return run_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000), x.u32(5, 50),
                         x.str(6, "stereo"), x.couple, x.meta);
     }},
    {"orbit", 2, "<out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]", "", Needs::kNothing,
     [](const Args& x) {
         return run_orbit(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.meta);
     }},
    {"atmos", 2, "<out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds] [mode]", "",
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.u32(5, 6),
                          x.str(6, "objects"), x.meta);
     }},
    {"atmos-path", 3, "<out.ec3> <paths.txt> [seconds] [bitrate_kbps] [objects]",
     "objects driven by an authored keyframe file instead of the built-in orbit",
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos_path(x.str(1), x.str(2), x.u32(3, 8), x.u32(4, 448), x.u32(5, 0),
                               x.meta);
     }},
    {"atmos-encode", 3, "<in.wav> <out.ec3> [bitrate_kbps] [objects] [paths.txt]",
     "every source channel as an object; optional: authored per-object motion from a keyframe "
     "file (same format as atmos-path), objects it doesn't mention keep their default placement",
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos_encode(x.str(1), x.str(2), x.u32(3, 448), x.u32(4, 0), x.meta,
                                 x.str(5));
     }},
    {"atmos-adm", 3, "<in.adm.wav> <out.ec3> [bitrate_kbps] [programme_id]",
     "a real ADM BWF master (BS.2076-2 ADM XML + BW64/RF64, roadmap B1) straight to DD+ JOC "
     "E-AC-3; every bed/object channel the resolved audioProgramme names becomes an AtmosEncoder "
     "object, driven by the file's own authored automation - no keyframe file needed. Only in "
     "builds with -DAC3FORGE_BUILD_ADM=ON",
     Needs::kAdm,
     [](const Args& x) {
         return run_atmos_adm(x.str(1), x.str(2), x.u32(3, 448), x.meta, x.str(4));
     }},
    {"record", 2, "<out.ac3> [seconds] [bitrate_kbps] [device_index]", "", Needs::kCapture,
     [](const Args& x) {
         return run_record(x.str(1), x.u32(2, 5), x.u32(3, 192), x.i32(4, 0), x.meta);
     }},
    {"live", 3,
     "<out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] "
     "[passthrough_device] [mode]",
     "capture -> encode -> live monitor and/or passthrough", Needs::kCapture,
     [](const Args& x) {
         return run_live(x.str(1), x.i32(2, 0), x.u32(3, 10), x.u32(4, 192), x.i32(5, -2),
                         x.i32(6, -2), x.str(7, "channels"), x.meta);
     }},
    {"encode", 3, "<in.wav> <out.ac3> [bitrate_kbps] [layout] [in2.wav]",
     "in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= "
     "for more than one source",
     Needs::kNothing,
     [](const Args& x) {
         return run_encode(x.str(1), x.str(2), x.u32(3, 192), x.couple, x.str(4), x.meta,
                           x.str(5));
     }},
    {"eac3-silence", 2, "<out.ec3> [seconds] [bitrate_kbps] [layout]", "", Needs::kNothing,
     [](const Args& x) {
         return run_eac3_silence(x.str(1), x.u32(2, 5), x.u32(3, 192), x.str(4, "stereo"),
                                 x.meta);
     }},
    {"eac3-sine", 2,
     "<out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "", Needs::kNothing,
     [](const Args& x) {
         return run_eac3_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000),
                              x.u32(5, 50), x.str(6, "stereo"), x.meta);
     }},
    {"eac3-encode", 3,
     "<in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr] [in2.wav]",
     "in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= "
     "for more than one source",
     Needs::kNothing,
     [](const Args& x) {
         return run_eac3_encode(x.str(1), x.str(2), x.u32(3, 192), x.str(4, "none"), x.str(5),
                                x.str(6, "off"), x.meta, x.str(7));
     }},
    {"decode", 3, "<in.ac3|in.ec3> <out.wav> [objects_dir]",
     "AC-3 or E-AC-3; bsid decides. objects_dir (E-AC-3 Atmos only): export each "
     "JOC-reconstructed object as its own object_NN.wav there",
     Needs::kNothing,
     [](const Args& x) { return run_decode(x.str(1), x.str(2), x.meta, x.str(3)); }},
    {"levels", 2, "<in.wav|in.ac3|in.ec3>", "per-channel peak/RMS report", Needs::kNothing,
     [](const Args& x) { return run_levels(x.str(1)); }},
    {"loudness", 2, "<in.wav>", "BS.1770-4 loudness -> dialnorm", Needs::kNothing,
     [](const Args& x) { return run_loudness(x.str(1)); }},
    {"qc", 2, "<in.ac3|in.ec3> [preset=<name>|all]",
     "bitstream-aware loudness QC: measured loudness vs. embedded dialnorm/compr, optional "
     "preset gate",
     Needs::kNothing, [](const Args& x) { return run_qc(x.str(1), x.meta.qc_preset); }},
    {"spdif", 3, "<in.ac3> <out.wav>", "IEC 61937 wrap as playable PCM16 WAV", Needs::kNothing,
     [](const Args& x) { return run_spdif(x.str(1), x.str(2)); }},
    {"mkv", 3, "<in.ac3|in.ec3> <out.mkv>", "wrap as a playable Matroska file", Needs::kNothing,
     [](const Args& x) { return run_mkv(x.str(1), x.str(2)); }},
    {"mp4", 3, "<in.ac3|in.ec3> <out.mp4>",
     "wrap as a playable MP4 with a spec-correct dac3/dec3 box", Needs::kNothing,
     [](const Args& x) { return run_mp4(x.str(1), x.str(2)); }},
    {"fmp4", 3, "<in.ac3|in.ec3> <out_dir> [frames_per_fragment]",
     "fragmented MP4/CMAF + HLS/DASH manifests, ready for a packager", Needs::kNothing,
     [](const Args& x) { return run_fmp4(x.str(1), x.str(2), x.u32(3, 48)); }},
    {"ts", 3, "<in.ac3|in.ec3> <out.ts>", "wrap as an MPEG-2 Transport Stream (DVB profile)",
     Needs::kNothing, [](const Args& x) { return run_ts(x.str(1), x.str(2)); }},
    {"devices", 1, "", "input and loopback capture endpoints", Needs::kCapture,
     [](const Args&) { return run_devices(); }},
    {"outputs", 1, "", "render endpoints + AC-3/E-AC-3 passthrough support", Needs::kPassthrough,
     [](const Args&) { return run_outputs(); }},
    {"play", 2, "<in.ac3|in.ec3> [device_index]",
     "exclusive-mode IEC 61937 passthrough; bsid decides AC-3 vs E-AC-3", Needs::kPassthrough,
     // -1, not 0: run_play reads a negative index as "the default endpoint",
     // where 0 names the first one 'outputs' lists and demands passthrough of it.
     [](const Args& x) { return run_play(x.str(1), x.i32(2, -1)); }},
    {"monitor", 2, "<in.ac3|in.ec3> [device_index]",
     "decode and play on an ordinary (non-bitstreamed) output", Needs::kMonitor,
     [](const Args& x) { return run_monitor(x.str(1), x.i32(2, -1), x.meta); }},
}};

void print_usage() {
    std::println("ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
    std::println("  ac3cli --version    print version and git provenance, then exit");
    for (const auto& c : kCommands) {
        std::string line = std::format("  ac3cli {:<13}{}", c.name, c.spec);
        // A command the platform cannot run is listed, not hidden: hiding it
        // makes 'ac3cli play' answer "unknown command", which is a lie about
        // a command that exists and would work elsewhere. The note slot says
        // so instead, and the reasons follow once below rather than being
        // repeated on every affected row.
        const std::string_view note = unmet(c.needs) != nullptr ? "UNAVAILABLE HERE" : c.note;
        if (!note.empty()) {
            // The note column starts at 62, but 'record' has a spec longer
            // than that and no padding is applied to a line already past the
            // stop - so guarantee the separating space by hand rather than
            // letting the note run into the last argument.
            if (line.size() >= 62) {
                line += ' ';
            }
            line = std::format("{:<62}({})", line, note);
        }
        std::println("{}", line);
    }
    const auto& backend = ac3::audio::audio_backend();
    if (!backend.capture.available || !backend.passthrough.available ||
        !backend.monitor.available) {
        std::println("");
        if (!backend.capture.available) {
            std::println("UNAVAILABLE HERE — {}.", backend.capture.reason);
        }
        if (!backend.passthrough.available) {
            std::println("UNAVAILABLE HERE — {}.", backend.passthrough.reason);
        }
        if (!backend.monitor.available) {
            std::println("UNAVAILABLE HERE — {}.", backend.monitor.reason);
        }
        std::println("Everything else is file I/O and behaves identically on every platform;");
        std::println("'spdif' in particular reaches a receiver without any audio backend at all.");
    }
    std::println("");
    std::println("'-' in place of <in.wav>, <out.ac3>, <out.ec3>, <in.ac3|in.ec3> or <out.wav>");
    std::println("       means stdin (an input path) or stdout (an output path) - encode,");
    std::println("       eac3-encode, atmos-encode and decode only. e.g.:");
    std::println("       ac3cli encode - - 448 couple < in.wav > out.ac3");
    std::println("");
    std::println("live monitor_device/passthrough_device: -2 (default) leaves that leg off,");
    std::println("       -1 is the default render endpoint, N picks one from 'outputs'.");
    std::println("       Either or both may run alongside the file this always writes.");
    std::println("live mode: 'channels' (default) carries stereo straight through; 'atmos'");
    std::println("       pans every captured channel into a 5.1 bed as its own object, moving");
    std::println("       it every frame the same way 'atmos' orbits its synthetic ones — the");
    std::println("       hook a real live position source drops into once one exists.");
    std::println("live capture2=<index>: the capture_device positional stays the session's");
    std::println("       clock master, paced exactly as it always has been; capture2= adds a");
    std::println("       second, independently-clocked device whose stream is resampled to");
    std::println("       track the master, with the measured drift printed at session end.");
    std::println("record/live container=mkv: write straight to Matroska (a single command)");
    std::println("       instead of the bare elementary stream both write by default; 'mkv'");
    std::println("       remains the way to wrap an ALREADY-encoded file after the fact.");
    std::println("monitor/live --monitor play the 5.1 BED of an Atmos-mode stream: the decoder");
    std::println("       reads TS 103 420's object layer (OAMD/JOC) and reports an object count,");
    std::println("       but this path does not render or export objects, so this is what a");
    std::println("       legacy decoder hears, not unmixed objects.");
    std::println("decode objects_dir (E-AC-3 Atmos only): exports each JOC-reconstructed object");
    std::println("       as its own object_NN.wav, alongside the usual 5.1 bed WAV.");
    std::println("");
    std::println("tools:  Annex E coding tools, '+'-joined — {}", plan::kToolsSyntax);
    std::println("        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);");
    std::println("        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;");
    std::println("        atten:N pins the SPX notch depth, noatten removes it");
    std::println("");
    std::println("vbr (eac3-encode only): {}", plan::kVbrSyntax);
    std::println("        quality is encoder-relative, not a fixed target — bit cost rises");
    std::println("        steeply above roughly half the range, so a high quality with no");
    std::println("        max bound will often refuse real programme material outright;");
    std::println("        bitrate_kbps still matters in vbr mode — it feeds the same");
    std::println("        coupling/spx frequency defaults it always has, not a target rate");
    std::println("atmos: objects orbit the room at different heights and rates,");
    std::println("       encoded as a 5.1 E-AC-3 bed with JOC + OAMD side data");
    std::println("       (TS 103 420). FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("atmos mode: objects (default) writes the JOC+OAMD container; bed51 omits");
    std::println("       it so the 5.1 bed still plays on a decoder that refuses an object");
    std::println("       container it cannot validate instead of falling back to the bed.");
    std::println("");
    std::println("layout: {}", plan::layout_names(plan::Codec::kEac3));
    std::println("        AC-3 carries only {} — everything wider needs the dependent",
                 plan::layout_names(plan::Codec::kAc3));
    std::println("        substreams that only E-AC-3 has.");
    for (const auto& info : plan::kLayouts) {
        if (info.transmitted == info.rendered) {
            continue;
        }
        // Where the two differ, say so: a dependent that REPLACES a bed
        // channel spends coded channels a listener never counts.
        std::println("        {} renders {} speakers from {} coded channels", info.name,
                     info.rendered, info.transmitted);
    }
    std::println("        For 'sine' and 'eac3-sine' each speaker gets its own tone; append");
    std::println("        'c' to a 'sine' layout (stereoc, 51c) to enable channel coupling.");
    std::println("        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a");
    std::println("        source narrower than it leaves the channels it lacks silent, and");
    std::println("        a wider one folds down per §7.8 using cmixlev/surmixlev.");
    std::println("");
    std::println("        [layout] also takes a comma-separated Table E2.5 location list");
    std::println("        instead of one of the names above, for anything Annex E allows");
    std::println("        that has no preset: e.g. L,C,R,LFE,Vhl,Vhr or L,C,R,LFE,LFE2,Vhc.");
    std::println("        AC-3 accepts one too, as long as it needs no dependent substream");
    std::println("        (e.g. L,R,Cs or L,C,R,Cs - Table 5.8 modes no preset names).");
    std::println("        Locations: L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr");
    std::println("        Vhc Lts Rts LFE2 LFE - a paired location (Lc/Rc, Lrs/Rrs, Lsd/Rsd,");
    std::println("        Lw/Rw, Vhl/Vhr, Lts/Rts) must be given both halves.");
    std::println("");
    std::println("atmos: objects orbit the room at different heights and rates;");
    std::println("       atmos-encode makes each channel of a real file an object instead.");
    std::println("       Both emit a 5.1 E-AC-3 bed with JOC + OAMD side data (TS 103 420).");
    std::println("       FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("       atmos-encode's [paths.txt] takes authored per-object motion the same");
    std::println("       way atmos-path does, keyed by WAV channel index; an object it doesn't");
    std::println("       mention keeps atmos-encode's own default (fanned-out) placement.");
    std::println("");
    std::println("mkv wraps an AC-3 or E-AC-3 elementary stream in Matroska, taking the");
    std::println("format, packet boundaries, sample rate and channel count from the bitstream");
    std::println("itself — so it cannot be told the wrong ones. E-AC-3 dependent substreams");
    std::println("are grouped into their access unit and counted as the channels they render.");
    std::println("");
    std::println("fmp4 writes a fragmented MP4/CMAF init segment plus one media segment per");
    std::println("fragment (frames_per_fragment access units each, default 48 - about 1.5s at");
    std::println("48 kHz), alongside an HLS media+master playlist pair and a DASH MPD, all");
    std::println("pointing at the same segments (CMAF's whole point) — ready for a real HLS/");
    std::println("DASH origin or packager. Dolby Atmos content signals CHANNELS=\"<N>/JOC\" in");
    std::println("the HLS playlists automatically, per Apple's HLS Authoring Specification.");
    std::println("");
    std::println("ts wraps the same elementary stream as an MPEG-2 Transport Stream (PAT + PMT");
    std::println("+ one PES-wrapped audio PID), identified per the DVB profile — stream_type");
    std::println("0x06 plus the AC3_descriptor or Enhanced_AC3_descriptor ETSI EN 300 468 Annex D");
    std::println("defines, not ATSC's — with PCR stamped on the audio PID every access unit.");
    std::println("");
    std::println("Without a layout, encode and eac3-encode both follow the source: 1 -> mono,");
    std::println("2 -> stereo, 3 to 6 -> 5.1; eac3-encode alone extends that to 8 -> 7.1,");
    std::println("10 -> 5.1.4, 12 -> 7.1.4 (encode refuses anything wider than 3/2 + LFE).");
    std::println("Commands that carry PCM report per-channel levels when they finish; 'record'");
    std::println("meters live. 'couple' turns on channel coupling wherever a command encodes.");
    print_meta_usage();
    std::println("");
    std::println("For decode, drc=<scale> applies §7.7.1 partial compression (0 = ignore,");
    std::println("1 = as encoded) and 'heavy' prefers compr where the stream carries it.");
    std::println("");
    std::println("qc measures a stream's real BS.1770-4/EBU Tech 3342 loudness and compares it");
    std::println("       against the dialnorm/compr it embeds - preset=<name> also gates that");
    std::println("       measurement against a named delivery spec ({}),", ac3::meta::kQcPresetNames);
    std::println("       or preset=all checks every one; omitting preset= just measures and");
    std::println("       reports, with no pass/fail verdict. Exit code is 0 only when every");
    std::println("       requested gate passes (or none was requested and decode succeeded).");
}

}  // namespace

int run_main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    if (raw.size() > 1 &&
        (std::string_view{raw[1]} == "--version" || std::string_view{raw[1]} == "-v")) {
        std::println("{}", ac3::version_details());
        return 0;
    }
    // Split the command line into positional arguments and metadata options. An
    // option is a key=value token or one of the bare flags, so the positional
    // arguments keep their places whether options are present or not, and
    // options may appear in any order.
    std::vector<char*> args{};      // args[0] is the command
    std::vector<char*> options{};
    bool couple_flag = false;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const std::string_view token{raw[i]};
        const bool is_option = token.find('=') != std::string_view::npos ||
                               token == "couple" || token == "heavy" || token == "heavy2" ||
                               token == "mixmeta" || token == "sign-objects" ||
                               token == "verify-objects" || token == "keep-partial" ||
                               token == "fast-mdct";
        if (token == "couple") {
            couple_flag = true;
        }
        (is_option ? options : args).push_back(raw[i]);
    }
    Options meta;
    if (!parse_options(options, meta)) {
        return 1;
    }
    if (args.empty()) {
        print_usage();
        return 0;
    }

    const std::string_view command{args[0]};
    for (const auto& c : kCommands) {
        if (c.name != command) {
            continue;
        }
        if (args.size() < c.min_args) {
            std::println(stderr, "error: {} needs {}", c.name, c.spec);
            return 1;
        }
        // Refuse before the handler runs, so a command that cannot work here
        // says why once, in the platform's own words, instead of failing
        // partway through with whatever error code the no-backend stub
        // happened to return. Nothing silently does nothing.
        if (const auto* missing = unmet(c.needs)) {
            std::println(stderr, "error: '{}' is unavailable on this platform: {}", c.name,
                         missing->reason);
            if (c.needs == Needs::kPassthrough) {
                // The one live-audio capability with a portable substitute:
                // same bursts, written to a file instead of an endpoint.
                std::println(stderr,
                             "  'ac3cli spdif <in.ac3> <out.wav>' wraps the same IEC 61937 "
                             "bursts into a WAV that any player will pass through untouched.");
            }
            return 1;
        }
        return c.run(Args{args, meta, couple_flag});
    }
    std::println(stderr, "error: unknown command '{}'", command);
    print_usage();
    return 1;
}

// run_main is std::expected-clean throughout; the one realistic exception
// source left is std::format/std::println itself (std::format_error), which
// nothing here catches internally. Left uncaught, that unwinds out of main
// and terminates - a crash with no exit code a script could act on rather
// than the ordinary "error: ..." this CLI otherwise always prints on
// failure. This is the one place that catches it. clang-tidy still flags
// main() itself: it cannot see past this try/catch to know the escape is
// caught, and reports the one path it cannot fully close by construction -
// the catch block's own std::println, whose fixed one-argument format string
// has no realistic way to throw. NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    try {
        return run_main(argc, argv);
    } catch (const std::exception& e) {
        std::println(stderr, "error: unhandled exception: {}", e.what());
        return 1;
    } catch (...) {
        std::println(stderr, "error: unhandled exception of unknown type");
        return 1;
    }
}
