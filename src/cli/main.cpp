#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/capture/capture.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/platform/audio_backend.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/monitor.hpp"
#include "ac3/sinks/passthrough.hpp"
#include "ac3/spatial/spatial.hpp"
#include "matroska/matroska.hpp"

namespace {

namespace plan = ac3::plan;

// Everything about layouts, coding tools and metadata now lives in
// ac3::plan, so the GUI cannot mean something different by "514" or by "all"
// than this does. What is left here is argument shape and printing.

void print_meta_usage();

void print_usage();

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

// --- metadata options -------------------------------------------------------
// Bare words and key=value tokens, appended after the positional arguments in
// any order, the same way 'couple' already works. Everything defaults off, so
// a command line that says nothing about metadata produces exactly the stream
// it produced before this layer existed.

void print_meta_usage() {
    std::println("metadata options (any order, after the positional arguments):");
    std::println("  drc=<profile>     §7.7.1 dynamic range control per block");
    std::println("                    {}", ac3::meta::kProfileNames);
    std::println("  heavy             §7.7.2 heavy compression: a peak ceiling in the");
    std::println("                    mono downmix, at syncframe resolution");
    std::println("  ceiling=<dBFS>    that ceiling (default -0.5)");
    std::println("  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)");
    std::println("  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)");
    std::println("  dialnorm=<1..31>  set it directly (default 31)");
    std::println("  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)");
    std::println("  surmixlev=-3|-6|off     surround downmix level (Table 5.10)");
    std::println("  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)");
    std::println("  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)");
    std::println("  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)");
}

// The encode-side group is ac3::plan::Metadata verbatim; only the decode-side
// scale is local, because nothing an encoder is configured with corresponds
// to it.
struct MetaOptions {
    plan::Metadata p{};
    // Decoder side, for 'decode'.
    double drc_scale = 0.0;
};

bool parse_double(std::string_view text, double& out) {
    // from_chars for floating point needs the locale-independent form, which
    // is what a command line gives.
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

// Returns false and prints the offending token on anything unrecognised: a
// silently ignored metadata flag looks exactly like metadata that did not work.
bool parse_meta_options(std::span<char*> tokens, MetaOptions& out) {
    for (char* raw : tokens) {
        const std::string_view token{raw};
        const auto eq = token.find('=');
        const std::string_view key = token.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (token == "couple" || token == "heavy" || token == "mixmeta") {
            if (token == "heavy") {
                out.p.heavy.emplace();
            } else if (token == "mixmeta") {
                out.p.mixmeta = true;
            }
            continue;
        }
        if (key == "drc") {
            // On the decode side drc= is a scale factor (§7.7.1 partial
            // compression); on the encode side it names a profile. A numeric
            // value is unambiguous, so one spelling serves both.
            double scale = 0.0;
            if (parse_double(value, scale)) {
                out.drc_scale = scale;
                continue;
            }
            ac3::meta::ProfileId id{};
            if (!ac3::meta::parse_profile(value, id)) {
                std::println(stderr, "error: unknown DRC profile '{}' ({})", value,
                             ac3::meta::kProfileNames);
                return false;
            }
            out.p.drc = ac3::meta::profile(id);
            continue;
        }
        if (key == "ceiling" || key == "dialogue") {
            double db = 0.0;
            if (!parse_double(value, db)) {
                std::println(stderr, "error: {} needs a level in dBFS", key);
                return false;
            }
            if (!out.p.heavy) {
                out.p.heavy.emplace();
            }
            if (key == "ceiling") {
                out.p.heavy->peak_ceiling_dbfs = db;
            } else {
                out.p.heavy->dialogue_target_dbfs = db;
            }
            continue;
        }
        if (key == "dialnorm") {
            if (value == "auto") {
                out.p.measure_dialnorm = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                std::println(stderr, "error: dialnorm must be auto or 1..31 (§5.4.2.8)");
                return false;
            }
            out.p.dialnorm = static_cast<int>(n);
            continue;
        }
        if (key == "cmixlev") {
            if (value == "-3") {
                out.p.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
            } else if (value == "-4.5") {
                out.p.cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB;
            } else if (value == "-6") {
                out.p.cmixlev = ac3::meta::CentreMixLevel::kMinus6dB;
            } else {
                std::println(stderr, "error: cmixlev must be -3, -4.5 or -6 (Table 5.9)");
                return false;
            }
            continue;
        }
        if (key == "surmixlev") {
            if (value == "-3") {
                out.p.surmixlev = ac3::meta::SurroundMixLevel::kMinus3dB;
            } else if (value == "-6") {
                out.p.surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB;
            } else if (value == "off") {
                out.p.surmixlev = ac3::meta::SurroundMixLevel::kSilent;
            } else {
                std::println(stderr, "error: surmixlev must be -3, -6 or off (Table 5.10)");
                return false;
            }
            continue;
        }
        if (key == "lfemix") {
            out.p.mixmeta = true;
            if (value == "off") {
                out.p.lfemix = std::nullopt;
                continue;
            }
            const auto n = parse_u32_or(value, 99);
            if (n > 31) {
                std::println(stderr, "error: lfemix must be off or 0..31 (§E2.3.1.11)");
                return false;
            }
            out.p.lfemix = static_cast<int>(n);
            continue;
        }
        if (key == "dmixmod") {
            out.p.mixmeta = true;
            if (value == "ltrt") {
                out.p.dmixmod = ac3::meta::DownmixMode::kLtRt;
            } else if (value == "loro") {
                out.p.dmixmod = ac3::meta::DownmixMode::kLoRo;
            } else if (value == "none") {
                out.p.dmixmod = ac3::meta::DownmixMode::kNotIndicated;
            } else {
                std::println(stderr, "error: dmixmod must be ltrt, loro or none (Table D2.2)");
                return false;
            }
            continue;
        }
        std::println(stderr, "error: unknown option '{}'", token);
        print_meta_usage();
        return false;
    }
    return true;
}

// BS.1770 integrated loudness of a whole WAV, and the dialnorm it implies.
std::optional<int> measured_dialnorm(const ac3::io::WavData& wav, ac3::SampleRate rate,
                                     ac3::Acmod acmod, bool lfe) {
    ac3::meta::LoudnessMeter meter{rate, acmod, lfe};
    std::vector<std::span<const float>> views;
    for (const auto& channel : wav.channels) {
        views.emplace_back(channel);
    }
    meter.push(views);
    const auto lkfs = meter.integrated_lkfs();
    if (!lkfs) {
        return std::nullopt;
    }
    const int dialnorm = ac3::meta::dialnorm_from_lkfs(*lkfs);
    std::println("measured {:.2f} LKFS (BS.1770-4, gated) -> dialnorm {}", *lkfs, dialnorm);
    return dialnorm;
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

// Interleaves `channels` (one vector per decoded channel, AC-3/E-AC-3 coded
// order) into WAV/Windows speaker order for playback, reading order[i] as
// which channels[] entry belongs at interleaved position i - the same
// permutation ac3::io::write_wav_f32 and plan::wav_order/wav_channel_order
// already produce for exactly this AC-3-order-vs-WAV-order reconciliation
// (see ac3/io/wav.hpp).
std::vector<float> interleave_reordered(std::span<const std::vector<float>> channels,
                                        std::span<const std::size_t> order) {
    const auto frame_count = channels.empty() ? std::size_t{0} : channels.front().size();
    std::vector<float> out(frame_count * order.size());
    for (std::size_t i = 0; i < frame_count; ++i) {
        for (std::size_t ch = 0; ch < order.size(); ++ch) {
            out[i * order.size() + ch] = channels[order[ch]][i];
        }
    }
    return out;
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

// ---------------------------------------------------------------------------
// Level reporting. Every number comes from ac3::analysis, so a level reads
// the same here as on the GUI's meters; only the drawing is local.
// ---------------------------------------------------------------------------

// A bar on the same -60..0 dBFS scale the GUI's meters use. ASCII rather than
// block glyphs: this has to stay legible in a bare console whatever code page
// it happens to be running.
std::string meter_bar(double db, int width) {
    std::string bar(static_cast<std::size_t>(width), '-');
    const auto filled = static_cast<int>(std::lround(ac3::analysis::meter_fraction(db) * width));
    for (int i = 0; i < filled; ++i) {
        bar[static_cast<std::size_t>(i)] = '#';
    }
    return bar;
}

// The exact figures for a finished run. Peak and RMS here are unweighted over
// the whole signal — ballistics exist to make a moving display readable, and
// would only blur a question that has a right answer.
void print_channel_summary(const ac3::analysis::LevelMeter& meter) {
    const auto acmod = meter.acmod();
    const bool lfe = meter.lfe();
    std::println("");
    std::println("per-channel levels ({}):", ac3::analysis::layout_name(acmod, lfe));
    std::println("  {:<4} {:>8} {:>8}  {:<20} {}", "ch", "peak", "rms", "peak (-60..0 dBFS)",
                 "clipped");
    for (int ch = 0; ch < meter.channel_count(); ++ch) {
        const auto& stats = meter.summary()[static_cast<std::size_t>(ch)];
        std::println("  {:<4} {:>8.2f} {:>8.2f}  [{}] {}",
                     ac3::analysis::channel_name(acmod, lfe, ch), stats.peak_db(),
                     stats.rms_db(), meter_bar(stats.peak_db(), 18),
                     stats.clipped_samples > 0 ? std::to_string(stats.clipped_samples) : "-");
    }
    // The energy vector over the whole run, not the last few hundred
    // milliseconds levels() remembers: a summary line has to describe the
    // same span of audio as the table above it.
    std::vector<ac3::analysis::ChannelLevel> whole(
        static_cast<std::size_t>(meter.channel_count()));
    for (std::size_t ch = 0; ch < whole.size(); ++ch) {
        whole[ch].rms_db = meter.summary()[ch].rms_db();
    }
    const auto field = ac3::analysis::energy_vector(whole, acmod);
    if (ac3::fullbw_channel_count(acmod) >= 2 && field.magnitude > 0.0) {
        // A perfectly centred image leaves a vanishing negative y, which
        // rounds to a correct but ridiculous "-0°".
        const double azimuth = std::round(field.azimuth_deg);
        std::println("  soundfield: {:.0f}° azimuth, focus {:.2f} (1.0 = a single speaker)",
                     azimuth == 0.0 ? 0.0 : azimuth, field.magnitude);
    }
}

// One line, rewritten in place. A carriage return rather than ANSI cursor
// moves, so it behaves the same in a bare console as in a terminal that
// speaks escape sequences. Every field is fixed width, so the line never
// leaves fragments of a longer previous line behind.
void print_live_meter(const ac3::analysis::LevelMeter& meter, double seconds) {
    const bool narrow = meter.channel_count() > 2;
    const int width = narrow ? 8 : 14;
    std::string line = std::format("{:6.1f} s", seconds);
    for (int ch = 0; ch < meter.channel_count(); ++ch) {
        const auto& level = meter.levels()[static_cast<std::size_t>(ch)];
        line += std::format(
            "  {:>3} [{}]", ac3::analysis::channel_name(meter.acmod(), meter.lfe(), ch),
            meter_bar(level.peak_db, width));
        if (!narrow) {
            line += std::format(" {:>6.1f} {:<4}", level.peak_db, level.clipped ? "CLIP" : "");
        }
    }
    std::print("\r{}", line);
    // Without a newline nothing reaches the console on its own: stdout is
    // block-buffered the moment it is redirected, and a meter nobody sees
    // until the run ends is not a meter.
    std::fflush(stdout);
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

// One tone per CODED channel, for the synthetic generators. The frequencies
// are deliberately spread and deliberately not harmonics of one another, so a
// channel that lands in the wrong speaker is measurable rather than merely
// suspected. The bed's values are mirrored in tests/test_eac3_decoder.cpp and
// must not drift from them; a dependent's are chosen apart from the bed's for
// the same reason - identical ones could not tell §E3.8.2's overwrite
// happening apart from the dependent being ignored altogether.
std::vector<double> layout_tones(plan::LayoutId id) {
    const std::vector<double> bed = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    const std::vector<double> rear = {500.0, 1600.0, 400.0, 1800.0};
    const std::vector<double> top = {2000.0, 2400.0, 2800.0, 3200.0};
    std::vector<double> out;
    switch (id) {
        case plan::LayoutId::kMono: return {1000.0};
        case plan::LayoutId::kStereo: return {1000.0, 1000.0};
        case plan::LayoutId::k51: return bed;
        case plan::LayoutId::k71:
            out = bed;
            out.insert(out.end(), rear.begin(), rear.end());
            return out;
        case plan::LayoutId::k512:
            out = bed;
            out.insert(out.end(), top.begin(), top.begin() + 2);
            return out;
        case plan::LayoutId::k514:
            out = bed;
            out.insert(out.end(), top.begin(), top.end());
            return out;
        case plan::LayoutId::k714:
            out = bed;
            out.insert(out.end(), rear.begin(), rear.end());
            out.insert(out.end(), top.begin(), top.end());
            return out;
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

// Reports a bad layout token against the set the codec can actually carry, so
// asking AC-3 for 7.1.4 says which of the two things is wrong.
std::optional<plan::LayoutId> layout_or_error(std::string_view name, plan::Codec codec) {
    const auto id = plan::parse_layout(name);
    if (!id) {
        std::println(stderr, "error: unknown layout '{}' ({})", name,
                     plan::layout_names(codec));
        return std::nullopt;
    }
    if (!plan::carries(codec, *id)) {
        std::println(stderr, "error: {} cannot carry {} - {}", plan::codec_label(codec),
                     plan::layout(*id).label,
                     plan::describe(plan::PlanError::kLayoutNeedsEac3));
        return std::nullopt;
    }
    return id;
}

int run_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
             std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
             bool couple_flag, const MetaOptions& meta) {
    // A layout may be suffixed with "c" to turn channel coupling on (51c). A
    // bare 'couple' token does the same, so the flag that works for 'encode'
    // is not silently ignored here.
    const bool couple = couple_flag || (!layout.empty() && layout.back() == 'c');
    const std::string_view base = couple ? layout.substr(0, layout.size() - 1) : layout;
    const auto id = layout_or_error(base, plan::Codec::kAc3);
    if (!id) {
        return 1;
    }

    plan::Plan p{.codec = plan::Codec::kAc3,
                 .layout = *id,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    p.tools.coupling = couple;
    const auto config = plan::ac3_config(p);

    // A one- or two-channel layout is the frequency-sweep case the freq_hz
    // argument exists for; anything wider gets a tone per speaker instead,
    // because one frequency in six channels cannot show where it ended up.
    auto tone_hz = layout_tones(*id);
    if (plan::layout(*id).rendered <= 2) {
        std::ranges::fill(tone_hz, static_cast<double>(freq_hz));
    }

    ac3::FrameEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
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
    std::println("wrote {} {} frames ({} kbps) to {}", count, plan::layout(*id).label,
                 bitrate, out_path);
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

int run_eac3_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                  std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                  const MetaOptions& meta) {
    const auto id = layout_or_error(layout, plan::Codec::kEac3);
    if (!id) {
        return 1;
    }
    const plan::Plan p{.codec = plan::Codec::kEac3,
                       .layout = *id,
                       .bitrate_kbps = bitrate,
                       .meta = meta.p};
    const auto config = plan::eac3_config(p);

    auto tone_hz = layout_tones(*id);
    if (plan::layout(*id).rendered <= 2) {
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
                 count, plan::layout(*id).label, nchans, config.dependents.size() + 1,
                 out_path);
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

// A WAV's rate as an fscod, or a diagnosis. Shared because every encode path
// asks the same question and A/52 Table 5.6 has the same three answers for
// all of them.
std::optional<ac3::SampleRate> wav_sample_rate(std::uint32_t hz, std::string_view codec) {
    switch (hz) {
        case 48000: return ac3::SampleRate::k48000;
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        default:
            std::println(stderr,
                         "error: sample rate {} is not legal for {} (need 32/44.1/48 kHz)", hz,
                         codec);
            return std::nullopt;
    }
}

// A source's channels routed onto a plan's coded channels, or a diagnosis.
std::optional<plan::Routing> routing_or_error(const plan::Plan& p, std::size_t channels) {
    auto routing = plan::route(p.layout, channels, p.meta.cmixlev, p.meta.surmixlev);
    if (!routing) {
        std::println(stderr, "error: {} channels - {}", channels,
                     plan::describe(plan::PlanError::kNoSourceLayout));
        return std::nullopt;
    }
    return routing;
}

// Says what the routing did, so a run that quietly left half a layout silent
// is visible rather than something to be discovered later on the meters.
void print_routing(const plan::Plan& p, const plan::Routing& routing) {
    if (routing.is_permutation()) {
        std::println("  source carried directly into {}", plan::layout(p.layout).label);
        return;
    }
    const auto names = plan::coded_channel_names(p.layout);
    std::string silent;
    for (int c = 0; c < routing.coded_channels; ++c) {
        bool fed = false;
        for (int s = 0; s < routing.source_channels && !fed; ++s) {
            fed = routing.at(c, s) != 0.0;
        }
        if (!fed) {
            silent += silent.empty() ? "" : " ";
            silent += names[static_cast<std::size_t>(c)];
        }
    }
    std::println("  {} source channels rendered onto {}", routing.source_channels,
                 plan::layout(p.layout).label);
    if (!silent.empty()) {
        std::println("  silent (the source carries nothing that belongs there): {}", silent);
    }
}

// Real program material through the E-AC-3 path. The tone generators above
// exercise field placement; only recorded-style material exercises the coding
// decisions, which is what the Annex E tools are judged on.
int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools, std::string_view layout,
                    const MetaOptions& meta) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto sr = wav_sample_rate(wav->sample_rate, "E-AC-3");
    if (!sr) {
        return 1;
    }
    // An unnamed layout follows the source, which is what this command did
    // before it could be told otherwise.
    std::optional<plan::LayoutId> id;
    if (layout.empty()) {
        id = plan::layout_for_source(wav->channels.size());
        if (!id) {
            std::println(stderr, "error: {} channels - {}", wav->channels.size(),
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
    } else if (id = layout_or_error(layout, plan::Codec::kEac3); !id) {
        return 1;
    }

    plan::Plan p{.codec = plan::Codec::kEac3,
                 .layout = *id,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (p.meta.measure_dialnorm) {
        const auto measured = measured_dialnorm(*wav, *sr, plan::bed_acmod(*id),
                                                plan::bed_lfe(*id));
        if (!measured) {
            std::println(stderr, "error: no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }

    const auto routing = routing_or_error(p, wav->channels.size());
    if (!routing) {
        return 1;
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    assert(static_cast<int>(nchans) == encoder.channel_count());
    const std::size_t total = wav->frame_count();

    std::vector<std::vector<float>> source(wav->channels.size(),
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
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        for (std::size_t c = 0; c < source.size(); ++c) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                source[c][static_cast<std::size_t>(i)] =
                    at < total ? wav->channels[c][at] : 0.0f;
            }
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            std::println(stderr, "error: the encoder cannot express this configuration");
            return 1;
        }
        frames.push_back(std::move(unit->bytes));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                 "tools: {}) to {}",
                 frames.size(), bitrate, wav->sample_rate, plan::layout(*id).label, nchans,
                 plan::format_tools(p.tools), out_path);
    print_routing(p, *routing);
    return 0;
}

// Objects moving in three dimensions, out as one 5.1 E-AC-3 stream carrying
// JOC and OAMD. Each object orbits at its own rate and sits at its own height,
// so no two of them share a direction for long - which is the condition under
// which JOC can actually pull them apart again. Heights are what makes this
// worth doing at all: a 5.1 bed cannot carry them, and the object metadata can.
int run_atmos(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t objects, std::uint32_t orbit_seconds, std::string_view mode,
              const MetaOptions& meta) {
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
                                    .emit_object_metadata = emit_objects},
                                   static_cast<int>(objects)};

    // Distinct tones so the objects are separable in the first place, and a
    // reader with an object renderer can tell which one ended up where.
    std::vector<double> tone_hz(count);
    std::vector<double> rate(count);
    std::vector<double> phase(count);
    std::vector<double> height(count);
    for (std::size_t i = 0; i < count; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
        // Rates that are not simple ratios of each other, so the objects do
        // not lock into formation and stay separable.
        rate[i] = 1.0 / (static_cast<double>(orbit_seconds) * (1.0 + 0.31 * static_cast<double>(i)));
        // Spread around the ring to begin with, or a short clip would show
        // them all bunched in the same quadrant - and objects that share a
        // direction are exactly the ones JOC cannot separate.
        phase[i] = 2.0 * std::numbers::pi * static_cast<double>(i) /
                   static_cast<double>(count);
        height[i] = count == 1 ? 0.5
                               : -1.0 + 2.0 * static_cast<double>(i) /
                                            static_cast<double>(count - 1);
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
    std::vector<std::vector<float>> essences(count,
                                             std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(count);
    std::vector<ac3::oba::ObjectPlacement> placement(count);
    std::vector<std::vector<std::byte>> out;
    out.reserve(static_cast<std::size_t>(frames));

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        // The placement is the object's position at the END of the frame,
        // because that is where both metadata layers interpolate to: OAMD's
        // ramp and the JOC matrix both finish there.
        const double t = static_cast<double>(n0 + ac3::kSamplesPerFrame) / 48000.0;
        for (std::size_t i = 0; i < count; ++i) {
            const double angle = 2.0 * std::numbers::pi * rate[i] * t + phase[i];
            placement[i] = {.position = {.x = 0.5 + 0.5 * std::sin(angle),
                                         .y = 0.5 - 0.5 * std::cos(angle),
                                         .z = height[i]},
                            .gain = 0.7 / std::sqrt(static_cast<double>(count)),
                            // Only the lowest object feeds the LFE, and only a
                            // little: it is the one channel JOC never touches.
                            .lfe_send = i == 0 ? 0.2 : 0.0};
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

// Every channel of a real file as its own object, over a 5.1 bed with JOC and
// OAMD beside it. The synthetic 'atmos' above shows what the object layer can
// express; this is the one that answers what it does to material somebody
// actually recorded - and it is what the GUI's object mode runs, so the two
// front ends can be compared on the same file.
int run_atmos_encode(std::string_view in_path, std::string_view out_path,
                     std::uint32_t bitrate, std::uint32_t objects,
                     const MetaOptions& meta) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto sr = wav_sample_rate(wav->sample_rate, "E-AC-3");
    if (!sr) {
        return 1;
    }
    // One object per source channel unless told otherwise; more objects than
    // the file has channels would leave some carrying nothing.
    const auto count = objects == 0 ? wav->channels.size()
                                    : std::min<std::size_t>(objects, wav->channels.size());
    if (count < 1 || count > 15) {
        std::println(stderr,
                     "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 "
                     "§8.3.2.2 caps the total at 16); this file has {} channels",
                     wav->channels.size());
        return 1;
    }

    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
        const auto measured =
            layout ? measured_dialnorm(*wav, *sr, layout->acmod, layout->lfe) : std::nullopt;
        if (!measured) {
            std::println(stderr, "error: cannot measure loudness for this file; "
                                 "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        dialnorm = *measured;
    }

    ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = dialnorm, .num_bands_idx = 4},
        static_cast<int>(count)};

    // Objects that reach the bed by the same route are exactly the ones JOC
    // cannot pull apart again, so the source's channels are spread across the
    // room rather than stacked at one point. A channel that already has a
    // direction keeps it; the rest fan out evenly.
    std::vector<ac3::oba::ObjectPlacement> placement(count);
    const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
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

    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, wav->sample_rate};
    const std::size_t total = wav->frame_count();
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
                    at < total ? wav->channels[ch][at] : 0.0f;
            }
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
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
    if (!write_frames(out_path, out)) {
        return 1;
    }
    std::println("encoded {} E-AC-3 access units ({} kbps, {} Hz) to {}", out.size(), bitrate,
                 wav->sample_rate, out_path);
    std::println("  {} objects from {} source channels + the bed's LFE = {} objects, "
                 "JOC over a 5.1 downmix",
                 count, wav->channels.size(), ac3::oba::object_count(encoder.program()));
    print_channel_summary(meter);
    return 0;
}

int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t orbit_seconds, const MetaOptions& meta) {
    ac3::spatial::BedRenderer renderer;
    const auto object =
        renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7, .lfe_send = 0.15});
    const plan::Plan p{.codec = plan::Codec::kAc3,
                       .layout = plan::LayoutId::k51,
                       .bitrate_kbps = bitrate,
                       .meta = meta.p};
    ac3::FrameEncoder encoder{plan::ac3_config(p)};
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
    // An orbit visits every speaker equally, so the summary's job here is to
    // show that no channel was left out and none dominates.
    print_channel_summary(meter);
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
        auto frame = encoder.encode_frame(views);
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
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} frames ({} kbps) to {}", frames.size(), bitrate, out_path);
    std::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
    print_channel_summary(meter);
    return 0;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple, std::string_view layout, const MetaOptions& meta) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto sr = wav_sample_rate(wav->sample_rate, "AC-3");
    if (!sr) {
        return 1;
    }
    // An unnamed layout follows the source, which is what this command did
    // before it could be told otherwise. Naming one is how a stereo file
    // reaches a 5.1 stream, or a 5.1 file gets folded down per §7.8.
    std::optional<plan::LayoutId> id;
    if (layout.empty()) {
        id = plan::layout_for_source(wav->channels.size());
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            std::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         wav->channels.size());
            return 1;
        }
    } else if (id = layout_or_error(layout, plan::Codec::kAc3); !id) {
        return 1;
    }

    plan::Plan p{.codec = plan::Codec::kAc3,
                 .layout = *id,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    p.tools.coupling = couple;
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
    if (p.meta.measure_dialnorm) {
        const auto measured =
            measured_dialnorm(*wav, *sr, plan::bed_acmod(*id), plan::bed_lfe(*id));
        if (!measured) {
            std::println(stderr,
                         "error: no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }

    const auto routing = routing_or_error(p, wav->channels.size());
    if (!routing) {
        return 1;
    }

    const auto config = plan::ac3_config(p);
    ac3::FrameEncoder encoder{config};
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, wav->sample_rate};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = wav->frame_count();

    std::vector<std::vector<float>> source(wav->channels.size(),
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
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // The tail frame is zero-padded to a full 1536 samples; the meter sees
        // only the real ones, so the padding cannot pull the RMS down.
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        for (std::size_t c = 0; c < source.size(); ++c) {
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                source[c][static_cast<std::size_t>(i)] =
                    at < total ? wav->channels[c][at] : 0.0f;
            }
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
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
    std::println("encoded {} frames ({} kbps, {} Hz, {}) to {}", frames.size(), bitrate,
                 wav->sample_rate, ac3::analysis::layout_name(config.acmod, config.lfe),
                 out_path);
    print_routing(p, *routing);
    print_channel_summary(meter);
    return 0;
}

int run_decode_eac3(std::span<const std::byte> stream, std::string_view out_path) {
    // Access units, not syncframes: a dependent substream is only meaningful
    // alongside the independent one it extends, and the two are rendered
    // together into one set of speaker feeds.
    const auto units = ac3::split_access_units(stream);
    if (!units) {
        std::println(stderr, "error: stream framing failed (code {})",
                     static_cast<int>(units.error()));
        return 1;
    }
    ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> pcm;
    ac3::DecodedAccessUnit first{};
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        if (!decoded) {
            std::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            return 1;
        }
        if (pcm.empty()) {
            first = *decoded;
            pcm.resize(decoded->channels.size());
        }
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), decoded->channels[ch].begin(),
                           decoded->channels[ch].end());
        }
    }
    if (pcm.empty()) {
        std::println(stderr, "error: no access units");
        return 1;
    }
    // The same WAV speaker order the encode side reads a file in, so a stream
    // decoded here and re-encoded lands every channel back where it started.
    const auto map = plan::wav_order(
        std::span{first.layout.items}.first(static_cast<std::size_t>(first.layout.count)));
    const auto written = ac3::io::write_wav_f32(std::string{out_path}, pcm,
                                                sample_rate_hz(first.sample_rate), map);
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::string speakers;
    for (const auto index : map) {
        speakers += ac3::eac3::chanmap::name(first.layout[static_cast<int>(index)]);
        speakers += ' ';
    }
    std::println("decoded {} E-AC-3 access units ({} substreams each) -> {}", units->size(),
                 first.substream_count, out_path);
    std::println("  {} channels, {} Hz: {}", map.size(), sample_rate_hz(first.sample_rate),
                 speakers);
    return 0;
}

int run_decode(std::string_view in_path, std::string_view out_path,
               const MetaOptions& meta) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
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
        return run_decode_eac3(stream, out_path);
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::println(stderr, "error: {}: {}", in_path, ac3::describe(frames.error()));
        return 1;
    }
    ac3::FrameDecoder decoder{
        {.drc_scale = meta.drc_scale, .heavy_compression = meta.p.heavy.has_value()}};
    std::vector<std::vector<float>> pcm;
    std::optional<ac3::analysis::LevelMeter> meter;
    ac3::DecodedFrame first{};
    bool have_first = false;
    // What the stream actually carried, reported whether or not it was applied.
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    double compr_min_db = 0.0;
    double compr_max_db = 0.0;
    std::size_t compr_frames = 0;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            std::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
            return 1;
        }
        for (const auto word : decoded->dynrng) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            dynrng_min_db = std::min(dynrng_min_db, db);
            dynrng_max_db = std::max(dynrng_max_db, db);
        }
        if (decoded->compr) {
            const double db = ac3::meta::to_db(ac3::meta::compr_gain(*decoded->compr));
            compr_min_db = compr_frames == 0 ? db : std::min(compr_min_db, db);
            compr_max_db = compr_frames == 0 ? db : std::max(compr_max_db, db);
            ++compr_frames;
        }
        if (!have_first) {
            first = *decoded;
            pcm.resize(decoded->channels.size());
            meter.emplace(decoded->acmod, decoded->lfe, sample_rate_hz(decoded->sample_rate));
            have_first = true;
        }
        std::vector<std::span<const float>> views;
        views.reserve(decoded->channels.size());
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), decoded->channels[ch].begin(),
                           decoded->channels[ch].end());
            views.emplace_back(decoded->channels[ch]);
        }
        meter->process(views);
    }
    if (!have_first) {
        std::println(stderr, "error: no frames");
        return 1;
    }
    const auto map = ac3::io::wav_channel_order(first.acmod, first.lfe);
    const auto written = ac3::io::write_wav_f32(std::string{out_path}, pcm,
                                                sample_rate_hz(first.sample_rate), map);
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::println("decoded {} frames -> {} ({}, {} Hz)", frames->size(), out_path,
                 ac3::analysis::layout_name(first.acmod, first.lfe),
                 sample_rate_hz(first.sample_rate));
    std::println("metadata: dialnorm {} (dialogue at -{} dBFS)", first.dialnorm,
                 first.dialnorm);
    std::println("          dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                 meta.drc_scale != 0.0 ? std::format(", applied at scale {}", meta.drc_scale)
                                       : ", not applied");
    if (compr_frames > 0) {
        std::println("          compr  {:+.2f} .. {:+.2f} dB over {} frames{}", compr_min_db,
                     compr_max_db, compr_frames,
                     meta.p.heavy ? ", applied" : ", not applied");
    } else {
        std::println("          compr  absent");
    }
    print_channel_summary(*meter);
    return 0;
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
        if (totals.empty()) {
            first = *decoded;
            totals.resize(decoded->channels.size());
            std::println("{}: {} access units, {} substreams each, {} channels, {} Hz",
                         in_path, units->size(), decoded->substream_count,
                         decoded->channels.size(), sample_rate_hz(decoded->sample_rate));
        }
        for (std::size_t ch = 0; ch < decoded->channels.size(); ++ch) {
            auto& stats = totals[ch];
            for (const float sample : decoded->channels[ch]) {
                const double magnitude = std::abs(static_cast<double>(sample));
                stats.peak = std::max(stats.peak, magnitude);
                stats.sum_squares += magnitude * magnitude;
                ++stats.samples;
                if (magnitude >= ac3::analysis::kFullScale) {
                    ++stats.clipped_samples;
                }
            }
        }
    }
    std::println("");
    std::println("per-channel levels:");
    std::println("  {:<6} {:>8} {:>8}  {:<20} {}", "ch", "peak", "rms", "peak (-60..0 dBFS)",
                 "clipped");
    for (std::size_t ch = 0; ch < totals.size(); ++ch) {
        const auto& stats = totals[ch];
        std::println("  {:<6} {:>8.2f} {:>8.2f}  [{}] {}",
                     ac3::eac3::chanmap::name(first.layout[static_cast<int>(ch)]),
                     stats.peak_db(), stats.rms_db(), meter_bar(stats.peak_db(), 18),
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
            meter->process(views);
        }
        print_channel_summary(*meter);
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
    const auto devices = ac3::sinks::enumerate_render_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::sinks::describe(devices.error()));
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

    const auto devices = ac3::sinks::enumerate_render_devices(content_rate);
    // Enumeration failing and enumeration finding nothing are different
    // answers: the first is the backend saying it could not look, the second
    // is it looking and seeing no endpoints. Reporting both as "none
    // available" sent people hunting for a missing sound device when the real
    // answer was a COM failure.
    if (!devices) {
        std::println(stderr, "error: {}", ac3::sinks::describe(devices.error()));
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

    ac3::sinks::PassthroughSink sink;
    const auto started = sink.start(
        device_id, content_rate,
        eac3 ? ac3::sinks::BitstreamFormat::kEac3 : ac3::sinks::BitstreamFormat::kAc3);
    if (!started) {
        std::println(stderr, "error: {}", ac3::sinks::describe(started.error()));
        return 1;
    }
    std::println("streaming {} {} to \"{}\" ({} Hz{})…", units.size(),
                 eac3 ? "access units" : "frames", device_name, content_rate,
                 eac3 ? ", carrier 4x that" : " carrier");

    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        std::vector<std::byte> burst;
        if (eac3) {
            const auto result = eac3_packer.push(unit);
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
                     std::string_view layout, const MetaOptions& meta) {
    const auto id = layout_or_error(layout, plan::Codec::kEac3);
    if (!id) {
        return 1;
    }
    const plan::Plan p{.codec = plan::Codec::kEac3,
                       .layout = *id,
                       .bitrate_kbps = bitrate,
                       .meta = meta.p};
    const auto unit = ac3::eac3::build_silent_access_unit(plan::eac3_config(p));
    if (!unit) {
        std::println(stderr, "error: invalid E-AC-3 configuration");
        return 1;
    }
    const std::uint64_t count = frame_count(seconds);
    const std::vector<std::vector<std::byte>> frames(static_cast<std::size_t>(count),
                                                     unit->bytes);
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} silent E-AC-3 {} access units ({} substreams, "
                 "{} bytes each, bsid 16) to {}",
                 count, plan::layout(*id).label, unit->substream_count(), unit->bytes.size(),
                 out_path);
    return 0;
}

// Decode a file back to PCM and play it on an ordinary (shared-mode, not
// bitstreamed) output - a sanity-check/preview path, and the offline half of
// live monitoring ('live's --monitor equivalent works the same way, one
// access unit at a time as it is produced instead of read from a file).
// Object metadata (JOC/OAMD) is not applied: the in-repo decoder's E-AC-3
// scope is A/52 Annex E syntax, not TS 103 420's object layer, so an Atmos
// file plays its 5.1 bed - exactly what a legacy decoder hears, which is the
// thing most worth confirming actually sounds right.
int run_monitor(std::string_view in_path, int device_index) {
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

    std::string device_id;
    std::string device_name = "default endpoint";
    if (device_index >= 0) {
        const auto devices = ac3::sinks::enumerate_render_devices();
        if (!devices) {
            std::println(stderr, "error: {}", ac3::sinks::describe(devices.error()));
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

    ac3::sinks::MonitorSink sink;
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
        ac3::Eac3Decoder decoder;
        std::vector<std::size_t> order;
        for (const auto& unit : *units) {
            const auto decoded = decoder.decode_access_unit(unit);
            if (!decoded) {
                std::println(stderr, "error: decode failed (code {})",
                             static_cast<int>(decoded.error()));
                return 1;
            }
            if (order.empty()) {
                order = plan::wav_order(
                    std::span{decoded->layout.items}.first(decoded->layout.count));
                const auto started = sink.start(device_id, sample_rate_hz(decoded->sample_rate),
                                                static_cast<std::uint16_t>(order.size()));
                if (!started) {
                    std::println(stderr, "error: {}", ac3::sinks::describe(started.error()));
                    return 1;
                }
                std::println("monitoring {} ({} channels, {} Hz) on \"{}\"…", in_path,
                             order.size(), sample_rate_hz(decoded->sample_rate), device_name);
            }
            play(interleave_reordered(decoded->channels, order));
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
                    std::println(stderr, "error: {}", ac3::sinks::describe(started.error()));
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
            std::string_view mode) {
    if (mode != "channels" && mode != "atmos") {
        std::println(stderr, "error: mode is 'channels' (default) or 'atmos'");
        return 1;
    }
    const bool atmos = mode == "atmos";

    const auto devices = ac3::capture::enumerate_devices();
    if (!devices) {
        std::println(stderr, "error: {}", ac3::capture::describe(devices.error()));
        return 1;
    }
    if (capture_device < 0 || static_cast<std::size_t>(capture_device) >= devices->size()) {
        std::println(stderr, "error: capture device index {} out of range (see 'ac3cli devices')",
                     capture_device);
        return 1;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(capture_device)];

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

    ac3::capture::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        std::println(stderr, "error: {}", ac3::capture::describe(started.error()));
        return 1;
    }
    const auto channels = capture.channels();
    const auto rate_hz = capture.sample_rate();

    // Object mode pans every captured channel into the 5.1 bed as its own
    // object (mirrors encodeObjects/run_atmos_encode); channel mode carries
    // the first two channels straight through as AC-3 stereo (mirrors
    // run_record, which this supersedes for anything wanting monitor or
    // passthrough alongside the file).
    const std::size_t nobjects = atmos ? std::min<std::size_t>(channels, 15) : 2;

    auto resolve_render_device = [&](int index) -> std::optional<ac3::sinks::RenderDeviceInfo> {
        if (index < 0) {
            return ac3::sinks::RenderDeviceInfo{};  // empty id: default endpoint
        }
        const auto render_devices = ac3::sinks::enumerate_render_devices(rate_hz);
        if (!render_devices || static_cast<std::size_t>(index) >= render_devices->size()) {
            return std::nullopt;
        }
        return (*render_devices)[static_cast<std::size_t>(index)];
    };

    ac3::sinks::MonitorSink monitor_sink;
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
                             ac3::sinks::describe(mstarted.error()));
            } else {
                monitoring = true;
                std::println("monitoring on \"{}\"", target->name.empty() ? "default endpoint"
                                                                          : target->name);
            }
        }
    }

    ac3::sinks::PassthroughSink passthrough_sink;
    bool passing_through = false;
    if (passthrough_device != -2) {
        const auto target = resolve_render_device(passthrough_device);
        const auto format =
            atmos ? ac3::sinks::BitstreamFormat::kEac3 : ac3::sinks::BitstreamFormat::kAc3;
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
                             ac3::sinks::describe(pstarted.error()));
            } else {
                passing_through = true;
                std::println("passthrough ({}) on \"{}\"", atmos ? "E-AC-3" : "AC-3",
                             target->name.empty() ? "default endpoint" : target->name);
            }
        }
    }

    ac3::FrameEncoder ac3_encoder{{.sample_rate = sr, .bitrate_kbps = bitrate}};
    std::optional<ac3::oba::AtmosEncoder> atmos_encoder;
    if (atmos) {
        atmos_encoder.emplace(
            ac3::oba::AtmosConfig{.sample_rate = sr, .bitrate_kbps = bitrate, .num_bands_idx = 4},
            static_cast<int>(nobjects));
    }
    ac3::FrameDecoder ac3_monitor_decoder;
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
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::size_t base = static_cast<std::size_t>(i) * channels;
            for (std::size_t ch = 0; ch < nobjects; ++ch) {
                block[ch][static_cast<std::size_t>(i)] =
                    ch < channels ? interleaved[base + ch] : 0.0f;
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
            const auto frame = ac3_encoder.encode_frame(std::span{views}.first(2));
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
                if (decoded) {
                    const auto order = plan::wav_order(
                        std::span{decoded->layout.items}.first(decoded->layout.count));
                    to_play = interleave_reordered(decoded->channels, order);
                }
            } else {
                const auto decoded = ac3_monitor_decoder.decode_frame(unit_bytes);
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
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} {} ({} kbps) to {}", frames.size(),
                 atmos ? "E-AC-3 access units" : "AC-3 frames", bitrate, out_path);
    std::println("captured {} frames, {} silence-filled, {} dropped", stats.frames_captured,
                 stats.frames_silence_filled, stats.frames_dropped);
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
    const MetaOptions& meta;
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

// What a command needs from the machine's audio hardware. Several commands
// touch it; every other command is file I/O and runs anywhere ac3forge
// compiles.
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
enum class Needs { kNothing, kCapture, kPassthrough, kMonitor };

// The unmet requirement, or nullptr when the platform can satisfy it.
//
// Note what this is not: an OS test. main.cpp never asks whether it is on
// Windows - it asks the one translation unit CMake compiled from
// src/lib/src/platform/<os>/ what that platform can do, and prints the answer
// that unit supplied. The day a Unix capture backend lands, capture flips to
// available in that file alone and 'devices' and 'record' start working here
// with no change to this file.
const ac3::platform::Capability* unmet(Needs needs) {
    const auto& backend = ac3::platform::audio_backend();
    switch (needs) {
        case Needs::kNothing: return nullptr;
        case Needs::kCapture: return backend.capture.available ? nullptr : &backend.capture;
        case Needs::kPassthrough:
            return backend.passthrough.available ? nullptr : &backend.passthrough;
        case Needs::kMonitor: return backend.monitor.available ? nullptr : &backend.monitor;
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

constexpr std::array<Command, 20> kCommands{{
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
    {"atmos-encode", 3, "<in.wav> <out.ec3> [bitrate_kbps] [objects]",
     "every source channel as an object", Needs::kNothing,
     [](const Args& x) {
         return run_atmos_encode(x.str(1), x.str(2), x.u32(3, 448), x.u32(4, 0), x.meta);
     }},
    {"record", 2, "<out.ac3> [seconds] [bitrate_kbps] [device_index]", "", Needs::kCapture,
     [](const Args& x) {
         return run_record(x.str(1), x.u32(2, 5), x.u32(3, 192), x.i32(4, 0));
     }},
    {"live", 3,
     "<out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] "
     "[passthrough_device] [mode]",
     "capture -> encode -> live monitor and/or passthrough", Needs::kCapture,
     [](const Args& x) {
         return run_live(x.str(1), x.i32(2, 0), x.u32(3, 10), x.u32(4, 192), x.i32(5, -2),
                         x.i32(6, -2), x.str(7, "channels"));
     }},
    {"encode", 3, "<in.wav> <out.ac3> [bitrate_kbps] [layout]", "", Needs::kNothing,
     [](const Args& x) {
         return run_encode(x.str(1), x.str(2), x.u32(3, 192), x.couple, x.str(4), x.meta);
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
    {"eac3-encode", 3, "<in.wav> <out.ec3> [bitrate_kbps] [tools] [layout]", "", Needs::kNothing,
     [](const Args& x) {
         return run_eac3_encode(x.str(1), x.str(2), x.u32(3, 192), x.str(4, "none"), x.str(5),
                                x.meta);
     }},
    {"decode", 3, "<in.ac3|in.ec3> <out.wav>", "AC-3 or E-AC-3; bsid decides", Needs::kNothing,
     [](const Args& x) { return run_decode(x.str(1), x.str(2), x.meta); }},
    {"levels", 2, "<in.wav|in.ac3|in.ec3>", "per-channel peak/RMS report", Needs::kNothing,
     [](const Args& x) { return run_levels(x.str(1)); }},
    {"loudness", 2, "<in.wav>", "BS.1770-4 loudness -> dialnorm", Needs::kNothing,
     [](const Args& x) { return run_loudness(x.str(1)); }},
    {"spdif", 3, "<in.ac3> <out.wav>", "IEC 61937 wrap as playable PCM16 WAV", Needs::kNothing,
     [](const Args& x) { return run_spdif(x.str(1), x.str(2)); }},
    {"mkv", 3, "<in.ac3|in.ec3> <out.mkv>", "wrap as a playable Matroska file", Needs::kNothing,
     [](const Args& x) { return run_mkv(x.str(1), x.str(2)); }},
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
     [](const Args& x) { return run_monitor(x.str(1), x.i32(2, -1)); }},
}};

void print_usage() {
    std::println("ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
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
    const auto& backend = ac3::platform::audio_backend();
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
    std::println("live monitor_device/passthrough_device: -2 (default) leaves that leg off,");
    std::println("       -1 is the default render endpoint, N picks one from 'outputs'.");
    std::println("       Either or both may run alongside the file this always writes.");
    std::println("live mode: 'channels' (default) carries stereo straight through; 'atmos'");
    std::println("       pans every captured channel into a 5.1 bed as its own object, moving");
    std::println("       it every frame the same way 'atmos' orbits its synthetic ones — the");
    std::println("       hook a real live position source drops into once one exists.");
    std::println("monitor/live --monitor play the 5.1 BED of an Atmos-mode stream: the in-repo");
    std::println("       decoder's E-AC-3 scope is A/52 Annex E syntax, not TS 103 420's object");
    std::println("       layer, so this is what a legacy decoder hears, not unmixed objects.");
    std::println("");
    std::println("tools:  Annex E coding tools, '+'-joined — {}", plan::kToolsSyntax);
    std::println("        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);");
    std::println("        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;");
    std::println("        atten:N pins the SPX notch depth, noatten removes it");
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
    std::println("        For 'sine' and 'eac3-sine' each speaker gets its own tone");
    std::println("        (L 1000, C 800, R 1200, Ls 600, Rs 1400, LFE 60 Hz); append 'c'");
    std::println("        to a 'sine' layout (stereoc, 51c) to enable channel coupling.");
    std::println("        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a");
    std::println("        source narrower than it leaves the channels it lacks silent, and");
    std::println("        a wider one folds down per §7.8 using cmixlev/surmixlev.");
    std::println("");
    std::println("atmos: objects orbit the room at different heights and rates;");
    std::println("       atmos-encode makes each channel of a real file an object instead.");
    std::println("       Both emit a 5.1 E-AC-3 bed with JOC + OAMD side data (TS 103 420).");
    std::println("       FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("");
    std::println("mkv wraps an AC-3 or E-AC-3 elementary stream in Matroska, taking the");
    std::println("format, packet boundaries, sample rate and channel count from the bitstream");
    std::println("itself — so it cannot be told the wrong ones. E-AC-3 dependent substreams");
    std::println("are grouped into their access unit and counted as the channels they render.");
    std::println("");
    std::println("Without a layout, encode and eac3-encode follow the source: 1 -> mono,");
    std::println("2 -> stereo, 3 to 6 -> 5.1, 8 -> 7.1, 10 -> 5.1.4, 12 -> 7.1.4. Commands");
    std::println("that carry PCM report per-channel levels when they finish; 'record' meters");
    std::println("live. 'couple' turns on channel coupling wherever a command encodes.");
    print_meta_usage();
    std::println("");
    std::println("For decode, drc=<scale> applies §7.7.1 partial compression (0 = ignore,");
    std::println("1 = as encoded) and 'heavy' prefers compr where the stream carries it.");
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    // Split the command line into positional arguments and metadata options. An
    // option is a key=value token or one of the three bare flags, so the
    // positional arguments keep their places whether options are present or
    // not, and options may appear in any order.
    std::vector<char*> args{};      // args[0] is the command
    std::vector<char*> options{};
    bool couple_flag = false;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const std::string_view token{raw[i]};
        const bool is_option = token.find('=') != std::string_view::npos ||
                               token == "couple" || token == "heavy" || token == "mixmeta";
        if (token == "couple") {
            couple_flag = true;
        }
        (is_option ? options : args).push_back(raw[i]);
    }
    MetaOptions meta;
    if (!parse_meta_options(options, meta)) {
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
