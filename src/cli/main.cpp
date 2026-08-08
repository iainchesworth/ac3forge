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
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/passthrough.hpp"
#include "ac3/spatial/spatial.hpp"
#include "matroska/matroska.hpp"

namespace {

// Named here rather than beside eac3_layout so the usage text and the error
// message that rejects a bad layout can never list different sets.
constexpr std::string_view kEac3Layouts = "stereo | 51 | 71 | 512 | 514 | 714";

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

// Why an AC-3 path turns a wider syntax away. Two different walls, each named
// once: the commands that share a wall must not describe it differently, and
// whichever one falls first should only have to be rewritten here.
constexpr std::string_view kDecoderLimit =
    "the in-repo decoder reads only the bsid<=8 syntax";
constexpr std::string_view kPackerLimit =
    "the IEC 61937 packer emits AC-3 bursts only (data type 1, one 6144-byte burst per frame)";

// Reports (and returns true) when a stream is a syncframe format nothing here
// can handle. E-AC-3 shares AC-3's 0x0B77 sync word, so a command that sniffs
// only the sync word ends up blaming the file for the reader's limits. bsid
// separates them: Annex E places it at bits 40..44 of the frame, exactly where
// §5.4.1.1 puts AC-3's, so that a decoder can identify the variant before it
// has parsed anything else. `limitation` names what actually cannot cope, so
// the message says why rather than only what.
//
// A stream with no sync word at all is passed through untouched: the caller's
// framer has a better vocabulary for malformed input than a guess made from
// six bytes.
bool reject_non_ac3_syntax(std::span<const std::byte> stream, std::string_view path,
                           std::string_view limitation) {
    if (stream.size() < 6 || std::to_integer<int>(stream[0]) != 0x0B ||
        std::to_integer<int>(stream[1]) != 0x77) {
        return false;
    }
    const auto bsid = std::to_integer<unsigned>(stream[5]) >> 3;
    if (bsid <= 8) {
        return false;
    }
    std::println(stderr, "error: {} is {} (bsid {}); {}", path,
                 bsid > 10 ? "E-AC-3" : "AC-3 alternate syntax (A/52 Annex D)", bsid,
                 limitation);
    return true;
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
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, 48000};

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
    std::println("wrote {} {} frames ({} kbps) to {}", count, surround ? "5.1" : "stereo",
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
struct Eac3Layout {
    ac3::eac3::AccessUnitConfig config;
    std::vector<double> tones;  // one per encoder input channel, in coded order
};

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

// Which Annex E tools to switch on, as a '+'-joined list ("cpl", "cpl+spx",
// "all", "none"). Every tool is a trade rather than a free win, so they are
// selected rather than assumed - and being able to encode the same material
// with and without one is the only way to say whether it earned its place.
constexpr std::string_view kEac3Tools =
    "none | cpl | spx | aht | all (cpl:N / spx:N pin a band edge, aht:N the gain mode)";

bool parse_eac3_tools(std::string_view text, ac3::eac3::FrameConfig& config) {
    if (text.empty() || text == "none") {
        return true;
    }
    while (!text.empty()) {
        const auto split = text.find('+');
        const auto token = text.substr(0, split);
        // "cpl:N" pins the coupling begin frequency code, which is how a
        // band-edge question gets answered by experiment rather than argument.
        if (token.starts_with("cpl:")) {
            config.coupling = true;
            config.cplbegf = static_cast<int>(parse_u32_or(token.substr(4), 99));
            if (config.cplbegf > 15) {
                return false;
            }
        } else if (token.starts_with("spx:")) {
            config.spx = true;
            config.spxbegf = static_cast<int>(parse_u32_or(token.substr(4), 99));
            if (config.spxbegf > 7) {
                return false;
            }
        } else if (token == "cpl") {
            config.coupling = true;
        } else if (token == "spx") {
            config.spx = true;
        } else if (token == "noatten") {
            // Spectral extension without its band-border notch, for the A/B.
            config.spx_atten = false;
        } else if (token.starts_with("atten:")) {
            config.spxattencod = static_cast<int>(parse_u32_or(token.substr(6), 99));
            if (config.spxattencod > 31) {
                return false;
            }
        } else if (token.starts_with("aht:")) {
            // "aht:0" is AHT with gain-adaptive quantization switched off,
            // which is how GAQ's own contribution gets measured.
            config.aht = true;
            config.gaqmod = static_cast<int>(parse_u32_or(token.substr(4), 99));
            if (config.gaqmod > 3) {
                return false;
            }
        } else if (token == "aht") {
            config.aht = true;
        } else if (token == "all") {
            config.coupling = true;
            config.spx = true;
            config.aht = true;
        } else {
            return false;
        }
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    }
    return true;
}

// Real program material through the E-AC-3 path. The tone generators above
// exercise field placement; only recorded-style material exercises the coding
// decisions, which is what the Annex E tools are judged on.
int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools) {
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
            std::println(stderr,
                         "error: sample rate {} is not legal for E-AC-3 (need 32/44.1/48 kHz)",
                         wav->sample_rate);
            return 1;
    }

    // WAV channel order is not AC-3 channel order, so a 5.1 file has to be
    // permuted on the way in - the inverse of wav_channel_map below.
    ac3::eac3::FrameConfig config{.sample_rate = sr, .bitrate_kbps = bitrate};
    std::vector<std::size_t> source;  // coded channel -> wav channel
    switch (wav->channels.size()) {
        case 1:
            config.acmod = ac3::Acmod::k1_0;
            source = {0};
            break;
        case 2:
            config.acmod = ac3::Acmod::k2_0;
            source = {0, 1};
            break;
        case 6:
            config.acmod = ac3::Acmod::k3_2;
            config.lfe = true;
            source = {0, 2, 1, 4, 5, 3};  // L C R Ls Rs LFE <- FL FR FC LFE BL BR
            break;
        default:
            std::println(stderr, "error: {} channels; expected 1, 2 or 6",
                         wav->channels.size());
            return 1;
    }

    if (!parse_eac3_tools(tools, config)) {
        std::println(stderr, "error: unknown tool set '{}' ({})", tools, kEac3Tools);
        return 1;
    }

    ac3::eac3::FrameEncoder encoder{config};
    const auto nchans = source.size();
    const std::size_t total = wav->frame_count();
    std::vector<std::vector<float>> block(nchans,
                                          std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        for (std::size_t c = 0; c < nchans; ++c) {
            const auto& channel = wav->channels[source[c]];
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[c][static_cast<std::size_t>(i)] = at < total ? channel[at] : 0.0f;
            }
            views[c] = block[c];
        }
        auto frame = encoder.encode_frame(views);
        if (!frame) {
            std::println(stderr, "error: the encoder cannot express this configuration");
            return 1;
        }
        frames.push_back(std::move(*frame));
    }
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("encoded {} E-AC-3 frames ({} kbps, {} Hz, {} channels, tools: {}) to {}",
                 frames.size(), bitrate, wav->sample_rate, nchans,
                 tools.empty() ? "none" : tools, out_path);
    return 0;
}

// Objects moving in three dimensions, out as one 5.1 E-AC-3 stream carrying
// JOC and OAMD. Each object orbits at its own rate and sits at its own height,
// so no two of them share a direction for long - which is the condition under
// which JOC can actually pull them apart again. Heights are what makes this
// worth doing at all: a 5.1 bed cannot carry them, and the object metadata can.
int run_atmos(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t objects, std::uint32_t orbit_seconds) {
    if (objects < 1 || objects > 15) {
        std::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return 1;
    }
    const auto count = static_cast<std::size_t>(objects);
    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = bitrate, .num_bands_idx = 4}, static_cast<int>(objects)};

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
    std::println("  {} dynamic objects + the bed's LFE = {} objects, JOC over a 5.1 downmix",
                 objects, ac3::oba::object_count(encoder.program()));
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
    ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, 48000};

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
               bool couple, const MetaOptions& meta) {
    const auto wav = ac3::io::read_wav(std::string{in_path});
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto layout = ac3::io::ac3_layout_for(wav->channels.size());
    if (!layout) {
        std::println(stderr,
                     "error: encode handles 1 to 6 channels ({} given); no AC-3 coding mode is "
                     "wider than 3/2 + LFE",
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
    // which is why it happens here rather than inside the frame encoder. It
    // gets the real layout rather than an assumed stereo one, because the
    // BS.1770 channel weighting depends on which coded positions are surrounds.
    int dialnorm = meta.dialnorm;
    if (meta.measure_dialnorm) {
        const auto measured = measured_dialnorm(*wav, sr, layout->acmod, layout->lfe);
        if (!measured) {
            std::println(stderr,
                         "error: no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly");
            return 1;
        }
        dialnorm = *measured;
    }

    // Coupling shares coefficients between full-bandwidth channels (§7.4), so
    // a mono program has nothing to share it with.
    const bool coupling = couple && ac3::fullbw_channel_count(layout->acmod) >= 2;
    ac3::FrameEncoder encoder{{.sample_rate = sr,
                               .bitrate_kbps = bitrate,
                               .dialnorm = dialnorm,
                               .acmod = layout->acmod,
                               .lfe = layout->lfe,
                               .coupling = coupling,
                               .drc = meta.drc,
                               .heavy = meta.heavy,
                               .cmixlev = meta.cmixlev,
                               .surmixlev = meta.surmixlev}};
    ac3::analysis::LevelMeter meter{layout->acmod, layout->lfe, wav->sample_rate};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const std::size_t total = wav->frame_count();
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // The tail frame is zero-padded to a full 1536 samples; the meter sees
        // only the real ones, so the padding cannot pull the RMS down.
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        for (std::size_t c = 0; c < nchans; ++c) {
            const auto& source = wav->channels[layout->wav_index[c]];
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[c][static_cast<std::size_t>(i)] = at < total ? source[at] : 0.0f;
            }
            views[c] = block[c];
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
                 wav->sample_rate, ac3::analysis::layout_name(layout->acmod, layout->lfe),
                 out_path);
    print_channel_summary(meter);
    return 0;
}

// Standard WAVEFORMATEXTENSIBLE speaker order, as far as Table E2.5 and it
// overlap. Three of E-AC-3's locations (Lw/Rw, Lsd/Rsd, LFE2) have no slot in
// that order at all; they follow in bitstream order rather than being dropped.
constexpr std::array<ac3::eac3::chanmap::Location, 17> kWavSpeakerOrder = {
    ac3::eac3::chanmap::Location::kLeft,           // FL
    ac3::eac3::chanmap::Location::kRight,          // FR
    ac3::eac3::chanmap::Location::kCentre,         // FC
    ac3::eac3::chanmap::Location::kLfe,            // LFE
    ac3::eac3::chanmap::Location::kLrs,            // BL
    ac3::eac3::chanmap::Location::kRrs,            // BR
    ac3::eac3::chanmap::Location::kLc,             // FLC
    ac3::eac3::chanmap::Location::kRc,             // FRC
    ac3::eac3::chanmap::Location::kCs,             // BC
    ac3::eac3::chanmap::Location::kLeftSurround,   // SL
    ac3::eac3::chanmap::Location::kRightSurround,  // SR
    ac3::eac3::chanmap::Location::kTs,             // TC
    ac3::eac3::chanmap::Location::kVhl,            // TFL
    ac3::eac3::chanmap::Location::kVhc,            // TFC
    ac3::eac3::chanmap::Location::kVhr,            // TFR
    ac3::eac3::chanmap::Location::kLts,            // TBL
    ac3::eac3::chanmap::Location::kRts,            // TBR
};

// Rendered layout -> WAV order, as source indices per output position. A 5.1
// bed comes out {0, 2, 1, 5, 3, 4}, which is what ac3::io::wav_channel_order
// returns for the same layout - the AC-3 path uses that directly, and this
// exists only because a chanmap layout cannot be named by acmod and lfeon.
std::vector<std::size_t> wav_channel_map(const ac3::eac3::chanmap::Layout& layout) {
    std::vector<std::size_t> map;
    std::vector<bool> placed(static_cast<std::size_t>(layout.count), false);
    for (const auto speaker : kWavSpeakerOrder) {
        const int index = layout.index_of(speaker);
        if (index >= 0) {
            map.push_back(static_cast<std::size_t>(index));
            placed[static_cast<std::size_t>(index)] = true;
        }
    }
    for (int i = 0; i < layout.count; ++i) {
        if (!placed[static_cast<std::size_t>(i)]) {
            map.push_back(static_cast<std::size_t>(i));
        }
    }
    return map;
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
    const auto map = wav_channel_map(first.layout);
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
    // decode no longer has an AC-3-only wall to turn E-AC-3 away at; spdif and
    // play still do, which is why kDecoderLimit's counterpart survives.
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
        {.drc_scale = meta.drc_scale, .heavy_compression = meta.heavy.has_value()}};
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
                     meta.heavy ? ", applied" : ", not applied");
    } else {
        std::println("          compr  absent");
    }
    print_channel_summary(*meter);
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
        if (reject_non_ac3_syntax(bytes, in_path, kDecoderLimit)) {
            return 1;
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
int run_spdif(std::string_view in_path, std::string_view out_path) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        std::println(stderr, "error: cannot read {}", in_path);
        return 1;
    }
    if (reject_non_ac3_syntax(stream, in_path, kPackerLimit)) {
        return 1;
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
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
    if (reject_non_ac3_syntax(stream, in_path, kPackerLimit)) {
        return 1;
    }
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        std::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
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


int run_eac3_silence(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
                     std::string_view layout, const MetaOptions& meta) {
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
    if (!write_frames(out_path, frames)) {
        return 1;
    }
    std::println("wrote {} silent E-AC-3 {} access units ({} substreams, "
                 "{} bytes each, bsid 16) to {}",
                 count, layout, unit->substream_count(), unit->bytes.size(), out_path);
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
    [[nodiscard]] int i32(std::size_t i, int fallback) const {
        return i < a.size() ? static_cast<int>(parse_u32_or(a[i], 0)) : fallback;
    }
};

struct Command {
    std::string_view name;
    std::size_t min_args;  // positional count INCLUDING the command itself
    std::string_view spec;
    std::string_view note;
    int (*run)(const Args&);
};

constexpr std::array<Command, 16> kCommands{{
    {"silence", 2, "<out.ac3> [seconds] [bitrate_kbps]", "",
     [](const Args& x) { return run_silence(x.str(1), x.u32(2, 5), x.u32(3, 192)); }},
    {"sine", 2, "<out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "",
     [](const Args& x) {
         return run_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000), x.u32(5, 50),
                         x.str(6, "stereo"), x.couple, x.meta);
     }},
    {"orbit", 2, "<out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]", "",
     [](const Args& x) {
         return run_orbit(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.meta);
     }},
    {"atmos", 2, "<out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds]", "",
     [](const Args& x) {
         return run_atmos(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.u32(5, 6));
     }},
    {"record", 2, "<out.ac3> [seconds] [bitrate_kbps] [device_index]", "",
     [](const Args& x) {
         return run_record(x.str(1), x.u32(2, 5), x.u32(3, 192), x.i32(4, 0));
     }},
    {"encode", 3, "<in.wav> <out.ac3> [bitrate_kbps] [couple]", "",
     [](const Args& x) {
         return run_encode(x.str(1), x.str(2), x.u32(3, 192), x.couple, x.meta);
     }},
    {"eac3-silence", 2, "<out.ec3> [seconds] [bitrate_kbps] [layout]", "",
     [](const Args& x) {
         return run_eac3_silence(x.str(1), x.u32(2, 5), x.u32(3, 192), x.str(4, "stereo"),
                                 x.meta);
     }},
    {"eac3-sine", 2,
     "<out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "",
     [](const Args& x) {
         return run_eac3_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000),
                              x.u32(5, 50), x.str(6, "stereo"), x.meta);
     }},
    {"eac3-encode", 3, "<in.wav> <out.ec3> [bitrate_kbps] [tools]", "",
     [](const Args& x) {
         return run_eac3_encode(x.str(1), x.str(2), x.u32(3, 192), x.str(4, "none"));
     }},
    {"decode", 3, "<in.ac3|in.ec3> <out.wav>", "AC-3 or E-AC-3; bsid decides",
     [](const Args& x) { return run_decode(x.str(1), x.str(2), x.meta); }},
    {"levels", 2, "<in.wav|in.ac3>", "per-channel peak/RMS report",
     [](const Args& x) { return run_levels(x.str(1)); }},
    {"loudness", 2, "<in.wav>", "BS.1770-4 loudness -> dialnorm",
     [](const Args& x) { return run_loudness(x.str(1)); }},
    {"spdif", 3, "<in.ac3> <out.wav>", "IEC 61937 wrap as playable PCM16 WAV",
     [](const Args& x) { return run_spdif(x.str(1), x.str(2)); }},
    {"mkv", 3, "<in.ac3|in.ec3> <out.mkv>", "wrap as a playable Matroska file",
     [](const Args& x) { return run_mkv(x.str(1), x.str(2)); }},
    {"devices", 1, "", "input and loopback capture endpoints",
     [](const Args&) { return run_devices(); }},
    {"outputs", 1, "", "render endpoints + AC-3 passthrough support",
     [](const Args&) { return run_outputs(); }},
}};

void print_usage() {
    std::println("ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    std::println("Usage:");
    for (const auto& c : kCommands) {
        std::string line = std::format("  ac3cli {:<13}{}", c.name, c.spec);
        if (!c.note.empty()) {
            line = std::format("{:<62}({})", line, c.note);
        }
        std::println("{}", line);
    }
    std::println("");
    std::println("");
    std::println("tools:  Annex E coding tools, '+'-joined — none | cpl | spx | aht | all;");
    std::println("        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);");
    std::println("        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;");
    std::println("        atten:N pins the SPX notch depth, noatten removes it");
    std::println("atmos: objects orbit the room at different heights and rates,");
    std::println("       encoded as a 5.1 E-AC-3 bed with JOC + OAMD side data");
    std::println("       (TS 103 420). FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("");
    std::println("layout: stereo (default) | 51 — 5.1 uses per-channel tones;");
    std::println("        append 'c' (stereoc, 51c) to enable channel coupling");
    std::println("        (L 1000, C 800, R 1200, SL 600, SR 1400, LFE 60 Hz)");
    std::println("");
    std::println("mkv wraps an AC-3 or E-AC-3 elementary stream in Matroska, taking the");
    std::println("format, packet boundaries, sample rate and channel count from the bitstream");
    std::println("itself — so it cannot be told the wrong ones. E-AC-3 dependent substreams");
    std::println("are grouped into their access unit and counted as the channels they render.");
    std::println("");
    std::println("encode takes 1 to 6 channel WAVs and picks the acmod to match: 1 -> 1/0,");
    std::println("2 -> 2/0, 3 -> 3/0, 4 -> 2/2, 5 -> 3/2, 6 -> 3/2 + LFE. Commands that");
    std::println("carry PCM report per-channel levels when they finish; 'record' meters live.");
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
        return c.run(Args{args, meta, couple_flag});
    }
    std::println(stderr, "error: unknown command '{}'", command);
    print_usage();
    return 1;
}
