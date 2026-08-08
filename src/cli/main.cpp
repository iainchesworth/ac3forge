#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
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

#include "ac3/capture/capture.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/passthrough.hpp"
#include "ac3/spatial/spatial.hpp"
#include "matroska/matroska.hpp"

namespace {

void print_meta_usage();

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
    std::println("  ac3cli loudness <in.wav>             (BS.1770-4 loudness -> dialnorm)");
    std::println("  ac3cli spdif   <in.ac3> <out.wav>   (IEC 61937 wrap as playable PCM16 WAV)");
    std::println("  ac3cli outputs                      (render endpoints + AC-3 passthrough support)");
    std::println("  ac3cli play    <in.ac3> [device_index]  (exclusive-mode IEC 61937 passthrough)");
    std::println("");
    std::println("layout: stereo (default) | 51 — 5.1 uses per-channel tones;");
    std::println("        append 'c' (stereoc, 51c) to enable channel coupling");
    std::println("        (L 1000, C 800, R 1200, SL 600, SR 1400, LFE 60 Hz)");
    std::println("");
    print_meta_usage();
    std::println("");
    std::println("For decode, drc=<scale> applies §7.7.1 partial compression (0 = ignore,");
    std::println("1 = as encoded) and 'heavy' prefers compr where the stream carries it.");
}

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

struct MetaOptions {
    std::optional<ac3::meta::Profile> drc;
    std::optional<ac3::meta::HeavyConfig> heavy;
    ac3::meta::CentreMixLevel cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB;
    ac3::meta::SurroundMixLevel surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB;
    int dialnorm = 31;
    bool measure_dialnorm = false;
    bool mixmeta = false;
    std::optional<int> lfemix = ac3::meta::kLfeMixLevelIdeal;
    ac3::meta::DownmixMode dmixmod = ac3::meta::DownmixMode::kLoRo;
    // Decoder side, for 'decode'.
    double drc_scale = 0.0;
    bool apply_heavy = false;
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
                out.heavy.emplace();
            } else if (token == "mixmeta") {
                out.mixmeta = true;
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
            out.drc = ac3::meta::profile(id);
            continue;
        }
        if (key == "ceiling" || key == "dialogue") {
            double db = 0.0;
            if (!parse_double(value, db)) {
                std::println(stderr, "error: {} needs a level in dBFS", key);
                return false;
            }
            if (!out.heavy) {
                out.heavy.emplace();
            }
            if (key == "ceiling") {
                out.heavy->peak_ceiling_dbfs = db;
            } else {
                out.heavy->dialogue_target_dbfs = db;
            }
            continue;
        }
        if (key == "dialnorm") {
            if (value == "auto") {
                out.measure_dialnorm = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                std::println(stderr, "error: dialnorm must be auto or 1..31 (§5.4.2.8)");
                return false;
            }
            out.dialnorm = static_cast<int>(n);
            continue;
        }
        if (key == "cmixlev") {
            if (value == "-3") {
                out.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
            } else if (value == "-4.5") {
                out.cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB;
            } else if (value == "-6") {
                out.cmixlev = ac3::meta::CentreMixLevel::kMinus6dB;
            } else {
                std::println(stderr, "error: cmixlev must be -3, -4.5 or -6 (Table 5.9)");
                return false;
            }
            continue;
        }
        if (key == "surmixlev") {
            if (value == "-3") {
                out.surmixlev = ac3::meta::SurroundMixLevel::kMinus3dB;
            } else if (value == "-6") {
                out.surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB;
            } else if (value == "off") {
                out.surmixlev = ac3::meta::SurroundMixLevel::kSilent;
            } else {
                std::println(stderr, "error: surmixlev must be -3, -6 or off (Table 5.10)");
                return false;
            }
            continue;
        }
        if (key == "lfemix") {
            out.mixmeta = true;
            if (value == "off") {
                out.lfemix = std::nullopt;
                continue;
            }
            const auto n = parse_u32_or(value, 99);
            if (n > 31) {
                std::println(stderr, "error: lfemix must be off or 0..31 (§E2.3.1.11)");
                return false;
            }
            out.lfemix = static_cast<int>(n);
            continue;
        }
        if (key == "dmixmod") {
            out.mixmeta = true;
            if (value == "ltrt") {
                out.dmixmod = ac3::meta::DownmixMode::kLtRt;
            } else if (value == "loro") {
                out.dmixmod = ac3::meta::DownmixMode::kLoRo;
            } else if (value == "none") {
                out.dmixmod = ac3::meta::DownmixMode::kNotIndicated;
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

// The two coarse AC-3 levels have no exact 3-bit twins for every value, but
// each one they do have is the same coefficient, so an E-AC-3 stream asked for
// "-4.5 dB centre" gets the level a listener would measure either way.
ac3::meta::MixLevel widen(ac3::meta::CentreMixLevel value) {
    switch (value) {
        case ac3::meta::CentreMixLevel::kMinus3dB: return ac3::meta::MixLevel::kMinus3dB;
        case ac3::meta::CentreMixLevel::kMinus4_5dB: return ac3::meta::MixLevel::kMinus4_5dB;
        case ac3::meta::CentreMixLevel::kMinus6dB: return ac3::meta::MixLevel::kMinus6dB;
    }
    return ac3::meta::MixLevel::kMinus4_5dB;
}

ac3::meta::MixLevel widen(ac3::meta::SurroundMixLevel value) {
    switch (value) {
        case ac3::meta::SurroundMixLevel::kMinus3dB: return ac3::meta::MixLevel::kMinus3dB;
        case ac3::meta::SurroundMixLevel::kMinus6dB: return ac3::meta::MixLevel::kMinus6dB;
        case ac3::meta::SurroundMixLevel::kSilent: return ac3::meta::MixLevel::kSilent;
    }
    return ac3::meta::MixLevel::kMinus6dB;
}

ac3::meta::MixMetadata mix_metadata(const MetaOptions& options) {
    return {.dmixmod = options.dmixmod,
            // Lt/Rt folds down into a matrix that will be re-decoded, so the
            // centre traditionally sits 1.5 dB hotter there than in Lo/Ro.
            .ltrtcmixlev = ac3::meta::MixLevel::kMinus3dB,
            .lorocmixlev = widen(options.cmixlev),
            .ltrtsurmixlev = ac3::meta::MixLevel::kMinus3dB,
            .lorosurmixlev = widen(options.surmixlev),
            .lfemixlevcod = options.lfemix};
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
             std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
             bool couple_flag, const MetaOptions& meta) {
    // Layouts: stereo | 51, each optionally suffixed with "c" to turn channel
    // coupling on (e.g. 51c). A bare 'couple' token does the same, so the flag
    // that works for 'encode' is not silently ignored here.
    const bool couple = couple_flag || (!layout.empty() && layout.back() == 'c');
    const std::string_view base = couple ? layout.substr(0, layout.size() - 1) : layout;
    const bool surround = base == "51";
    ac3::EncoderConfig config{.bitrate_kbps = bitrate,
                              .dialnorm = meta.dialnorm,
                              .coupling = couple,
                              .drc = meta.drc,
                              .heavy = meta.heavy,
                              .cmixlev = meta.cmixlev,
                              .surmixlev = meta.surmixlev};
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

// The same tone generator as run_sine, but through the E-AC-3 container.
// Real audio is the only input that can detect a frame-layout error at all:
// with silence every bap is zero, so a stray bit lands in zero-filled aux
// data and the frame still "decodes".
// Layouts wider than 5.1 need a dependent substream: the independent one
// always carries a self-sufficient 5.1 bed, and the extra channels ride along
// beside it with a chanmap saying where they belong.
struct Eac3Layout {
    ac3::eac3::AccessUnitConfig config;
    std::vector<double> tones;  // one per encoder input channel, in coded order
};

constexpr std::string_view kEac3Layouts = "stereo | 51 | 71 | 512 | 514 | 714";

std::optional<Eac3Layout> eac3_layout(std::string_view name, std::uint32_t bitrate,
                                      const MetaOptions& meta = {}) {
    Eac3Layout out;
    out.config.independent.bitrate_kbps = bitrate;
    out.config.independent.dialnorm = meta.dialnorm;
    out.config.independent.drc = meta.drc;
    out.config.independent.heavy = meta.heavy;
    if (meta.mixmeta) {
        out.config.independent.mixing = mix_metadata(meta);
    }
    if (name == "stereo") {
        out.tones = {1000.0, 1000.0};
        return out;
    }
    out.config.independent.acmod = ac3::Acmod::k3_2;
    out.config.independent.lfe = true;
    out.tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};  // L C R Ls Rs LFE
    if (name == "51") {
        return out;
    }
    // A dependent gets its own slice of the rate rather than a share of the
    // independent's - substreams occupy one frame period, not one frame.
    const std::uint32_t dep_kbps = bitrate / 2;
    // The spec's own worked example: acmod 2/2 with bits 3, 4 and 6, so Ls and
    // Rs REPLACE the bed's surrounds and Lrs/Rrs are new. The replacement
    // tones are deliberately not the bed's 600/1400 - identical ones could not
    // tell the overwrite happening apart from the dependent being ignored.
    const ac3::eac3::FrameConfig rear{.bitrate_kbps = dep_kbps,
                                      .acmod = ac3::Acmod::k2_2,
                                      .chanmap = ac3::eac3::chanmap::k71Rear};
    if (name == "71") {
        out.config.dependents.push_back(rear);
        out.tones.insert(out.tones.end(), {500.0, 1600.0, 400.0, 1800.0});
        return out;
    }
    if (name == "512") {  // a 5.1 bed with two height channels above it
        out.config.dependents.push_back({.bitrate_kbps = dep_kbps,
                                         .acmod = ac3::Acmod::k2_0,
                                         .chanmap = ac3::eac3::chanmap::k512Height});
        out.tones.insert(out.tones.end(), {2000.0, 2400.0});
        return out;
    }
    const ac3::eac3::FrameConfig top{.bitrate_kbps = dep_kbps,
                                     .acmod = ac3::Acmod::k2_2,
                                     .chanmap = ac3::eac3::chanmap::kTopQuad};
    if (name == "514") {
        // Four new channels, so this is the widest immersive layout a SINGLE
        // dependent can carry - see the channel-budget note beside kTopQuad.
        out.config.dependents.push_back(top);
        out.tones.insert(out.tones.end(), {2000.0, 2400.0, 2800.0, 3200.0});
        return out;
    }
    if (name == "714") {
        // Six new channels, one more than a dependent can hold, so this needs
        // two: the 7.1 rear pair, then the ceiling quad.
        //
        // Spec-correct and rejected by FFmpeg, which refuses any frame with
        // substreamid != 0 in ff_ac3_parse_header - it implements the TS 102
        // 366 Annex J profile, where a stream "shall" hold at most one
        // dependent, numbered 0. Every real delivery path caps it there and
        // ships 7.1.4 as Atmos objects instead, so this layout has no oracle.
        out.config.dependents.push_back(rear);
        out.config.dependents.push_back(top);
        out.tones.insert(out.tones.end(),
                         {500.0, 1600.0, 400.0, 1800.0, 2000.0, 2400.0, 2800.0, 3200.0});
        return out;
    }
    return std::nullopt;
}

// Channels a layout RENDERS, which is not the channel count the encoder is
// fed: a dependent substream's channels may overwrite the bed's rather than
// add to it, so 7.1 codes ten and renders eight.
int rendered_channels(const ac3::eac3::AccessUnitConfig& config) {
    // Start from the bed's own locations, then union in each dependent's map.
    const int bed = ac3::fullbw_channel_count(config.independent.acmod) +
                    (config.independent.lfe ? 1 : 0);
    std::uint16_t extra = 0;
    for (const auto& dep : config.dependents) {
        if (dep.chanmap) {
            extra = static_cast<std::uint16_t>(extra | *dep.chanmap);
        }
    }
    // The bed already covers L/C/R/Ls/Rs/LFE, so only locations outside it add
    // to the count.
    constexpr std::uint16_t kBedLocations =
        ac3::eac3::chanmap::kLeft | ac3::eac3::chanmap::kCentre |
        ac3::eac3::chanmap::kRight | ac3::eac3::chanmap::kLeftSurround |
        ac3::eac3::chanmap::kRightSurround | ac3::eac3::chanmap::kLfe;
    return bed + ac3::eac3::chanmap::channel_count(
                     static_cast<std::uint16_t>(extra & ~kBedLocations));
}

int run_eac3_mkv(std::string_view out_path, std::string_view ec3_path,
                 std::uint32_t bitrate, std::string_view layout) {
    const auto chosen = eac3_layout(layout, bitrate);
    if (!chosen) {
        std::println(stderr, "error: unknown layout '{}' ({})", layout, kEac3Layouts);
        return 1;
    }
    std::ifstream in{std::string{ec3_path}, std::ios::binary};
    if (!in) {
        std::println(stderr, "error: cannot open {}", ec3_path);
        return 1;
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};

    // Split the elementary stream back into access units. A new one begins at
    // each independent substream (strmtyp 0), and every syncframe declares its
    // own length in frmsiz - so the container's packet boundaries come from
    // the bitstream itself rather than from anything we remembered.
    std::vector<std::vector<std::byte>> units;
    std::size_t offset = 0;
    while (offset + 4 <= raw.size()) {
        const auto byte = [&](std::size_t i) {
            return static_cast<std::uint8_t>(raw[offset + i]);
        };
        if (byte(0) != 0x0B || byte(1) != 0x77) {
            std::println(stderr, "error: lost sync at byte {}", offset);
            return 1;
        }
        const int strmtyp = byte(2) >> 6;
        const std::size_t size =
            ((static_cast<std::size_t>(byte(2) & 0x07) << 8 | byte(3)) + 1) * 2;
        if (offset + size > raw.size()) {
            break;  // a truncated tail frame is not an access unit
        }
        if (strmtyp == 0 || units.empty()) {
            units.emplace_back();
        }
        const auto* start = reinterpret_cast<const std::byte*>(raw.data() + offset);
        units.back().insert(units.back().end(), start, start + size);
        offset += size;
    }
    if (units.empty()) {
        std::println(stderr, "error: no access units in {}", ec3_path);
        return 1;
    }

    const matroska::AudioTrack track{
        .codec_id = std::string{matroska::kCodecEac3},
        .sample_rate = ac3::sample_rate_hz(chosen->config.independent.sample_rate),
        .channels = rendered_channels(chosen->config),
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
    std::println("wrote {} access units ({} channels, {} bytes) to {}", units.size(),
                 track.channels, file->size(), out_path);
    return 0;
}

int run_eac3_sine(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                  std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                  const MetaOptions& meta) {
    auto chosen = eac3_layout(layout, bitrate, meta);
    if (!chosen) {
        std::println(stderr, "error: unknown layout '{}' ({})", layout, kEac3Layouts);
        return 1;
    }
    const auto& config = chosen->config;
    auto tone_hz = chosen->tones;
    if (layout == "stereo") {
        tone_hz = {static_cast<double>(freq_hz), static_cast<double>(freq_hz)};
    }
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    assert(nchans == tone_hz.size());
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
    std::println("wrote {} E-AC-3 {} access units ({} channels, {} substreams, bsid 16) to {}",
                 count, layout, nchans, config.dependents.size() + 1, out_path);
    return 0;
}

int run_orbit(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t orbit_seconds, const MetaOptions& meta) {
    ac3::spatial::BedRenderer renderer;
    const auto object =
        renderer.add_object({.azimuth_deg = 0.0, .gain = 0.7, .lfe_send = 0.15});
    ac3::FrameEncoder encoder{{.bitrate_kbps = bitrate,
                               .dialnorm = meta.dialnorm,
                               .acmod = ac3::Acmod::k3_2,
                               .lfe = true,
                               .drc = meta.drc,
                               .heavy = meta.heavy,
                               .cmixlev = meta.cmixlev,
                               .surmixlev = meta.surmixlev}};

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
               bool couple, const MetaOptions& meta) {
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

    // §5.4.2.8 says dialnorm "shall affect the sound reproduction level", so
    // getting it wrong is not a cosmetic error - a stream that claims 31 when
    // dialogue is really at -18 plays 13 dB too loud on a levelled system.
    // Measuring needs the whole programme (the BS.1770 relative gate does),
    // which is why it happens here rather than inside the frame encoder.
    int dialnorm = meta.dialnorm;
    if (meta.measure_dialnorm) {
        const auto measured = measured_dialnorm(*wav, sr, ac3::Acmod::k2_0, false);
        if (!measured) {
            std::println(stderr,
                         "error: no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        dialnorm = *measured;
    }

    ac3::FrameEncoder encoder{{.sample_rate = sr,
                               .bitrate_kbps = bitrate,
                               .dialnorm = dialnorm,
                               .coupling = couple,
                               .drc = meta.drc,
                               .heavy = meta.heavy,
                               .cmixlev = meta.cmixlev,
                               .surmixlev = meta.surmixlev}};
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

int run_decode(std::string_view in_path, std::string_view out_path,
               const MetaOptions& meta) {
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
    ac3::FrameDecoder decoder{
        {.drc_scale = meta.drc_scale, .heavy_compression = meta.heavy.has_value()}};
    std::vector<std::vector<float>> pcm;
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
            std::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
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
    std::println("metadata: dialnorm {} (dialogue at -{} dBFS)", first.dialnorm,
                 first.dialnorm);
    std::println("          dynrng {:+.2f} .. {:+.2f} dB{}", dynrng_min_db, dynrng_max_db,
                 meta.drc_scale != 0.0 ? std::format(", applied at scale {}", meta.drc_scale)
                                       : ", not applied");
    if (compr_frames > 0) {
        std::println("          compr  {:+.2f} .. {:+.2f} dB over {} frames{}", compr_min_db,
                     compr_max_db, compr_frames,
                     meta.heavy ? ", applied" : ", not applied");
    } else {
        std::println("          compr  absent");
    }
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
    // Channel weighting depends on which coded positions are surrounds, so the
    // layout has to be inferred from the channel count (Table 5.8).
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    bool lfe = false;
    switch (wav->channels.size()) {
        case 1: acmod = ac3::Acmod::k1_0; break;
        case 2: acmod = ac3::Acmod::k2_0; break;
        case 3: acmod = ac3::Acmod::k3_0; break;
        case 4: acmod = ac3::Acmod::k2_2; break;
        case 5: acmod = ac3::Acmod::k3_2; break;
        case 6: acmod = ac3::Acmod::k3_2; lfe = true; break;
        default:
            std::println(stderr, "error: {} channels is not an AC-3 layout",
                         wav->channels.size());
            return 1;
    }
    const auto dialnorm = measured_dialnorm(*wav, sr, acmod, lfe);
    if (!dialnorm) {
        std::println("no audio above the -70 LKFS absolute gate: loudness undefined");
        return 1;
    }
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

    if (!args.empty() && std::string_view{args[0]} == "devices") {
        return run_devices();
    }
    if (!args.empty() && std::string_view{args[0]} == "outputs") {
        return run_outputs();
    }
    if (args.size() < 2) {
        print_usage();
        return args.empty() ? 0 : 1;
    }
    const std::string_view command{args[0]};
    if (command == "record") {
        return run_record(args[1], args.size() > 2 ? parse_u32_or(args[2], 5) : 5,
                          args.size() > 3 ? parse_u32_or(args[3], 192) : 192,
                          args.size() > 4 ? static_cast<int>(parse_u32_or(args[4], 0)) : 0);
    }
    if (command == "silence") {
        return run_silence(args[1], args.size() > 2 ? parse_u32_or(args[2], 5) : 5,
                           args.size() > 3 ? parse_u32_or(args[3], 192) : 192);
    }
    if (command == "loudness") {
        return run_loudness(args[1]);
    }
    if (command == "eac3-silence") {
        const auto seconds = args.size() > 2 ? parse_u32_or(args[2], 5) : 5;
        const auto bitrate = args.size() > 3 ? parse_u32_or(args[3], 192) : 192;
        const std::string_view layout = args.size() > 4 ? args[4] : "stereo";
        const auto chosen = eac3_layout(layout, bitrate, meta);
        if (!chosen) {
            std::println(stderr, "error: unknown layout '{}' ({})", layout, kEac3Layouts);
            return 1;
        }
        const auto unit = ac3::eac3::build_silent_access_unit(chosen->config);
        if (!unit) {
            std::println(stderr, "error: invalid E-AC-3 configuration");
            return 1;
        }
        const std::uint64_t count = (static_cast<std::uint64_t>(seconds) * 48000 + 1535) / 1536;
        const std::vector<std::vector<std::byte>> frames(static_cast<std::size_t>(count),
                                                         unit->bytes);
        if (!write_frames(args[2], frames)) {
            return 1;
        }
        std::println("wrote {} silent E-AC-3 {} access units ({} substreams, "
                     "{} bytes each, bsid 16) to {}",
                     count, layout, unit->substream_count(), unit->bytes.size(), args[1]);
        return 0;
    }
    if (command == "eac3-mkv" && args.size() > 2) {
        return run_eac3_mkv(args[1], args[2],
                            args.size() > 3 ? parse_u32_or(args[3], 448) : 448,
                            args.size() > 4 ? std::string_view{args[4]} : "51");
    }
    if (command == "eac3-sine") {
        return run_eac3_sine(args[1], args.size() > 2 ? parse_u32_or(args[2], 5) : 5,
                             args.size() > 3 ? parse_u32_or(args[3], 192) : 192,
                             args.size() > 4 ? parse_u32_or(args[4], 1000) : 1000,
                             args.size() > 5 ? parse_u32_or(args[5], 50) : 50,
                             args.size() > 6 ? std::string_view{args[6]} : "stereo", meta);
    }
    if (command == "sine") {
        return run_sine(args[1], args.size() > 2 ? parse_u32_or(args[2], 5) : 5,
                        args.size() > 3 ? parse_u32_or(args[3], 192) : 192,
                        args.size() > 4 ? parse_u32_or(args[4], 1000) : 1000,
                        args.size() > 5 ? parse_u32_or(args[5], 50) : 50,
                        args.size() > 6 ? std::string_view{args[6]} : "stereo", couple_flag,
                        meta);
    }
    if (command == "orbit") {
        return run_orbit(args[1], args.size() > 2 ? parse_u32_or(args[2], 8) : 8,
                         args.size() > 3 ? parse_u32_or(args[3], 448) : 448,
                         args.size() > 4 ? parse_u32_or(args[4], 4) : 4, meta);
    }
    if (command == "encode" && args.size() > 2) {
        return run_encode(args[1], args[2], args.size() > 3 ? parse_u32_or(args[3], 192) : 192,
                          couple_flag, meta);
    }
    if (command == "decode" && args.size() > 2) {
        return run_decode(args[1], args[2], meta);
    }
    if (command == "spdif" && args.size() > 2) {
        return run_spdif(args[1], args[2]);
    }
    if (command == "play") {
        return run_play(args[1],
                        args.size() > 2 ? static_cast<int>(parse_u32_or(args[2], 0)) : -1);
    }
    print_usage();
    return 1;
}
