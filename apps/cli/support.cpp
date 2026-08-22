#include "support.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <print>

#include "ac3/encoder/assignment.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/meta/qc.hpp"
#include "platform/stdio_binary.hpp"

namespace ac3cli {

namespace plan = ac3::plan;

namespace {

bool parse_double(std::string_view text, double& out) {
    // from_chars for floating point needs the locale-independent form, which
    // is what a command line gives.
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::vector<std::byte> to_bytes(std::span<const char> raw) {
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

// Wraps ac3::io::write_wav_f32 to honor the "-" stdout convention: "-"
// writes the WAV to stdout, binary mode set first, instead of opening a file
// with that literal name. ac3::io::write_wav_f32(std::ostream&, ...) never
// seeks (see its own comment), so this is exactly as safe on the unseekable
// pipe stdout usually is as the path overload is on a plain file.
std::expected<void, ac3::io::WavError> write_wav_f32_arg(
        std::string_view path, std::span<const std::vector<float>> channels,
        std::uint32_t sample_rate, std::span<const std::size_t> channel_order = {}) {
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        auto result = ac3::io::write_wav_f32(std::cout, channels, sample_rate, channel_order);
        std::cout.flush();
        return result;
    }
    return ac3::io::write_wav_f32(std::string{path}, channels, sample_rate, channel_order);
}

}  // namespace

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

void print_meta_usage() {
    std::println("metadata options (any order, after the positional arguments):");
    std::println("  drc=<profile>     §7.7.1 dynamic range control per block");
    std::println("                    {}", ac3::meta::kProfileNames);
    std::println("  heavy             §7.7.2 heavy compression: a peak ceiling in the");
    std::println("                    mono downmix, at syncframe resolution");
    std::println("  ceiling=<dBFS>    that ceiling (default -0.5)");
    std::println("  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)");
    std::println("  drc2=<profile>    Ch2's own DRC profile, layout 1+1 only (§7.7.1) - not "
                 "inherited from drc=, set both to compress both programmes alike");
    std::println("  heavy2            Ch2's own heavy compression, layout 1+1 only (§7.7.2.2)");
    std::println("  ceiling2=<dBFS>   that ceiling for Ch2 (default -0.5)");
    std::println("  dialogue2=<dBFS>  where Ch2's heavy compression puts dialogue (default -20)");
    std::println("  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)");
    std::println("  dialnorm=<1..31>  set it directly (default 31)");
    std::println("  dialnorm2=auto | <1..31>   Ch2's own dialnorm, layout 1+1 only "
                 "(§5.4.2.16, default 31)");
    std::println("  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)");
    std::println("  surmixlev=-3|-6|off     surround downmix level (Table 5.10)");
    std::println("  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)");
    std::println("  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)");
    std::println("  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)");
    std::println("  keep-partial      encode/eac3-encode/atmos-encode: if the run fails partway, "
                 "keep whatever frames were already encoded (named beside the intended output as "
                 "<name>.partial.<ext>) instead of discarding them - off by default, matching the "
                 "GUI's own keep-partial-output preference");
    std::println("  fast-mdct=off     force the direct §8.2.3.2 forward MDCT instead of the "
                 "default §7.9.4 fast path (identical streams to within ~1e-12 coefficient "
                 "error; the direct form is the validation oracle) - applies wherever this "
                 "command encodes, incl. atmos/record/live/eac3-sine; eac3-encode alone has a "
                 "[tools] positional argument whose bare nofastmdct token reaches the same "
                 "field instead; bare fast-mdct (the old opt-in) is a no-op");
    std::println("  mode=reference    force BOTH transforms onto the spec's own direct "
                 "evaluations (the forms every fast-path test validates against): the §8.2.3.2 "
                 "forward MDCT wherever this command encodes, and §7.9.4's step-3 inverse in "
                 "'decode' - for runs where bit-for-bit agreement with the spec's stated "
                 "arithmetic matters more than speed. mode=performance (the default) keeps "
                 "both fast paths: 215-285 dB SNR against reference on 180 s programmes, "
                 "4.5-4.7x faster decodes. Tokens apply in order, so a later fast-mdct=off / "
                 "fast-imdct=off still adjusts one half on its own");
    std::println("  fast-imdct=off    decode: force just the direct §7.9.4 step-3 inverse "
                 "(mode=reference's decode half); bare fast-imdct names the default");
    std::println("  sign-objects      atmos/atmos-path/atmos-encode: write a keyed EMDF object "
                 "signature (needs signing-key=); see docs/concepts/object-signing.md");
    std::println("  verify-objects    decode/monitor: check each frame's EMDF object signature "
                 "against signing-key= instead of just playing it - a mismatch refuses the "
                 "command; omitted (the default) decodes signed and unsigned streams alike, "
                 "unchecked");
    std::println("  signing-key=<path>      the key file sign-objects/verify-objects use "
                 "(or AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY)");
    std::println();
    std::println("source options (encode/eac3-encode; any order, after the positional "
                 "arguments):");
    std::println("  src=<path>        an additional input source; repeat for more than one");
    std::println("  map=<spec>        {}", plan::kAssignmentSyntax);
    std::println("                    once given, every loaded channel must appear - explicit "
                 "'none' silences the goes-nowhere warning without giving it anywhere to go");
    std::println("  offset=<sourceIndex>:<seconds>   leading silence ahead of that source's own "
                 "channels (seconds >= 0), same 0-based numbering as src=");
    std::println("                    the programme is still as long as the longest one once "
                 "every offset is applied");
    std::println();
    std::println("record/live options (record, live; any order, after the positional "
                 "arguments):");
    std::println("  container=mkv     write straight to Matroska instead of the bare elementary");
    std::println("                    stream this writes by default - same shape of choice as");
    std::println("                    the GUI's own Container setting");
    std::println("  container=raw     the default, spelled out");
    std::println();
    std::println("live options (live; any order, after the positional arguments):");
    std::println("  capture2=<index>  a second capture device, clock-conformed to the first "
                 "(see 'devices')");
    std::println();
    std::println("qc options (qc; any order, after the positional arguments):");
    std::println("  preset=<name>     gate the measurement against a named delivery spec");
    std::println("                    {}", ac3::meta::kQcPresetNames);
    std::println("  preset=all        gate against every preset above");
    std::println("                    omitted: measure and report only, no gate");
}

bool parse_options(std::span<char*> tokens, Options& out) {
    for (char* raw : tokens) {
        const std::string_view token{raw};
        const auto eq = token.find('=');
        const std::string_view key = token.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (token == "couple" || token == "heavy" || token == "heavy2" || token == "mixmeta" ||
            token == "keep-partial" || token == "fast-mdct") {
            if (token == "heavy") {
                out.p.heavy.emplace();
            } else if (token == "heavy2") {
                out.p.heavy2.emplace();
            } else if (token == "mixmeta") {
                out.p.mixmeta = true;
            } else if (token == "keep-partial") {
                out.keep_partial = true;
            } else if (token == "fast-mdct") {
                out.fast_mdct = true;
            }
            continue;
        }
        if (token == "fast-imdct") {
            out.fast_imdct = true;
            continue;
        }
        if (token == "sign-objects") {
            out.sign_objects = true;
            continue;
        }
        if (token == "verify-objects") {
            out.verify_objects = true;
            continue;
        }
        if (key == "fast-mdct") {
            // The bare word (handled above) is the historical opt-in; with
            // the fast path now the default, the value form exists for the
            // direction that still needs saying.
            if (value == "off") {
                out.fast_mdct = false;
                continue;
            }
            std::println(stderr,
                         "error: the fast MDCT is the default; 'fast-mdct=off' forces the "
                         "direct §8.2.3.2 transform (got '{}')",
                         token);
            return false;
        }
        if (key == "fast-imdct") {
            // Same shape as fast-mdct above, decode side: the fast inverse
            // is the default since its evidence was accepted, so the value
            // form exists for the direction that still needs saying. The
            // bare word (handled above) now just names what already happens.
            if (value == "off") {
                out.fast_imdct = false;
                continue;
            }
            std::println(stderr,
                         "error: the fast IMDCT is the default; 'fast-imdct=off' forces the "
                         "direct §7.9.4 step-3 evaluation (got '{}')",
                         token);
            return false;
        }
        if (key == "mode") {
            // The two transform switches as one intent-level toggle:
            // performance (the default state - both fast paths) for normal
            // runs, reference (both spec-direct evaluations, the forms
            // every fast-path test validates against) for runs where
            // bit-for-bit agreement with the spec's stated arithmetic
            // matters more than speed. Tokens apply in order, so a later
            // fast-mdct=off / fast-imdct=off can still adjust one half.
            if (value == "performance") {
                out.fast_mdct = true;
                out.fast_imdct = true;
                continue;
            }
            if (value == "reference") {
                out.fast_mdct = false;
                out.fast_imdct = false;
                continue;
            }
            std::println(stderr,
                         "error: mode is 'performance' (the default) or 'reference' (got '{}')",
                         token);
            return false;
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
        if (key == "drc2") {
            // Encode-side only, unlike drc= - nothing on the decode side
            // corresponds to a per-programme DRC profile, since a decoder
            // just applies whatever dynrng2 the stream carries.
            ac3::meta::ProfileId id{};
            if (!ac3::meta::parse_profile(value, id)) {
                std::println(stderr, "error: unknown DRC profile '{}' ({})", value,
                             ac3::meta::kProfileNames);
                return false;
            }
            out.p.drc2 = ac3::meta::profile(id);
            continue;
        }
        if (key == "ceiling2" || key == "dialogue2") {
            double db = 0.0;
            if (!parse_double(value, db)) {
                std::println(stderr, "error: {} needs a level in dBFS", key);
                return false;
            }
            if (!out.p.heavy2) {
                out.p.heavy2.emplace();
            }
            if (key == "ceiling2") {
                out.p.heavy2->peak_ceiling_dbfs = db;
            } else {
                out.p.heavy2->dialogue_target_dbfs = db;
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
        if (key == "dialnorm2") {
            if (value == "auto") {
                out.p.measure_dialnorm2 = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                std::println(stderr, "error: dialnorm2 must be auto or 1..31 (§5.4.2.16)");
                return false;
            }
            out.p.dialnorm2 = static_cast<int>(n);
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
        if (key == "src") {
            if (value.empty()) {
                std::println(stderr, "error: src= needs a file path");
                return false;
            }
            out.sources.emplace_back(value);
            continue;
        }
        if (key == "map") {
            if (value.empty()) {
                std::println(stderr, "error: map= needs a spec ({})", plan::kAssignmentSyntax);
                return false;
            }
            out.map_spec = std::string{value};
            continue;
        }
        if (key == "offset") {
            const auto colon = value.find(':');
            std::size_t index = 0;
            double seconds = 0.0;
            bool ok = colon != std::string_view::npos;
            if (ok) {
                const auto index_text = value.substr(0, colon);
                const auto seconds_text = value.substr(colon + 1);
                const auto [ptr, ec] = std::from_chars(
                    index_text.data(), index_text.data() + index_text.size(), index);
                ok = ec == std::errc{} && ptr == index_text.data() + index_text.size();
                ok = ok && parse_double(seconds_text, seconds) && seconds >= 0.0;
            }
            if (!ok) {
                std::println(stderr,
                             "error: offset= needs <sourceIndex>:<seconds> (seconds >= 0)");
                return false;
            }
            // A given sourceIndex may appear more than once; offset_samples_for
            // reads this in order and keeps the last match, so no dedupe here.
            out.offsets.emplace_back(index, seconds);
            continue;
        }
        if (key == "capture2") {
            int index = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), index);
            const bool ok =
                ec == std::errc{} && ptr == value.data() + value.size() && index >= 0;
            if (!ok) {
                std::println(stderr, "error: capture2= needs a non-negative device index");
                return false;
            }
            out.capture2 = index;
            continue;
        }
        if (key == "container") {
            if (value == "mkv" || value == "matroska") {
                out.matroska_container = true;
            } else if (value == "raw") {
                out.matroska_container = false;
            } else {
                std::println(stderr, "error: container must be raw or mkv (got '{}')", token);
                return false;
            }
            continue;
        }
        if (key == "preset") {
            if (value != "all") {
                ac3::meta::QcPresetId id{};
                if (!ac3::meta::parse_qc_preset(value, id)) {
                    std::println(stderr, "error: unknown qc preset '{}' ({} | all)", value,
                                 ac3::meta::kQcPresetNames);
                    return false;
                }
            }
            out.qc_preset = std::string{value};
            continue;
        }
        if (key == "signing-key") {
            if (value.empty()) {
                std::println(stderr, "error: signing-key= needs a key file path");
                return false;
            }
            out.signing_key = std::string{value};
            continue;
        }
        std::println(stderr, "error: unknown option '{}'", token);
        print_meta_usage();
        return false;
    }
    return true;
}

std::optional<int> finish_measurement(const ac3::meta::LoudnessMeter& meter,
                                      std::string_view programme, std::string_view field,
                                      FILE* out) {
    const auto lkfs = meter.integrated_lkfs();
    if (!lkfs) {
        return std::nullopt;
    }
    const int dialnorm = ac3::meta::dialnorm_from_lkfs(*lkfs);
    if (programme.empty()) {
        std::println(out, "measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", *lkfs, field,
                     dialnorm);
    } else {
        std::println(out, "{} measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", programme, *lkfs,
                     field, dialnorm);
    }
    return dialnorm;
}

std::optional<int> measured_dialnorm(const ac3::io::WavData& wav, ac3::SampleRate rate,
                                     ac3::Acmod acmod, bool lfe, FILE* out) {
    ac3::meta::LoudnessMeter meter{rate, acmod, lfe};
    std::vector<std::span<const float>> views;
    views.reserve(wav.channels.size());
    for (const auto& channel : wav.channels) {
        views.emplace_back(channel);
    }
    meter.push(views);
    return finish_measurement(meter, {}, "dialnorm", out);
}

std::optional<int> measured_dialnorm_channel(std::span<const float> channel, ac3::SampleRate rate,
                                             std::string_view programme, std::string_view field,
                                             FILE* out) {
    ac3::meta::LoudnessMeter meter{rate, ac3::Acmod::k1_0, false};
    const std::array<std::span<const float>, 1> views{channel};
    meter.push(views);
    return finish_measurement(meter, programme, field, out);
}

bool prepare_dual_mono_source(ac3::io::WavData& wav, std::string_view layout,
                              std::string_view in2_path) {
    if (layout != "1+1") {
        if (!in2_path.empty()) {
            std::println(stderr,
                         "error: a second input file is only meaningful with layout 1+1 "
                         "(got layout '{}')",
                         layout);
            return false;
        }
        return true;
    }
    if (in2_path.empty()) {
        if (wav.channels.size() != 2) {
            std::println(stderr,
                         "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                         "two mono files; the source has {} channel(s) and no second file "
                         "was given",
                         wav.channels.size());
            return false;
        }
        return true;
    }
    if (wav.channels.size() != 1) {
        std::println(stderr,
                     "error: layout 1+1 with a second input file needs the first file to be "
                     "mono (Ch1); it has {} channels",
                     wav.channels.size());
        return false;
    }
    auto second = ac3::io::read_wav(std::string{in2_path});
    if (!second) {
        std::println(stderr, "error: {}: {}", in2_path, ac3::io::describe(second.error()));
        return false;
    }
    if (second->channels.size() != 1) {
        std::println(stderr, "error: {} must be mono (Ch2); it has {} channels", in2_path,
                     second->channels.size());
        return false;
    }
    if (second->sample_rate != wav.sample_rate) {
        std::println(stderr,
                     "error: {} is {} Hz, but the first file is {} Hz - both programmes must "
                     "share a sample rate",
                     in2_path, second->sample_rate, wav.sample_rate);
        return false;
    }
    wav.channels.push_back(std::move(second->channels.front()));
    return true;
}

bool is_stdio_path(std::string_view path) { return path == "-"; }

FILE* status_stream(std::string_view out_path) { return is_stdio_path(out_path) ? stderr : stdout; }

bool write_frames(std::string_view path, std::span<const std::vector<std::byte>> frames) {
    if (is_stdio_path(path)) {
        // set_stdio_binary() before the first byte, not once at startup: a
        // command that never touches "-" (the overwhelming majority of
        // invocations) should not pay for it, and calling it more than once
        // in the rare case both the input and output of one command are "-"
        // is harmless - see platform/stdio_binary.hpp for what it fixes.
        ac3::cli::platform::set_stdio_binary();
        for (const auto& frame : frames) {
            std::cout.write(reinterpret_cast<const char*>(frame.data()),
                            static_cast<std::streamsize>(frame.size()));
        }
        std::cout.flush();
        if (!std::cout) {
            std::println(stderr, "error: cannot write to stdout");
            return false;
        }
        return true;
    }
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

bool write_frames_or_mux(std::string_view path, bool matroska, const matroska::AudioTrack& track,
                         std::span<const std::vector<std::byte>> frames) {
    if (!matroska) {
        return write_frames(path, frames);
    }
    const auto file = matroska::mux(track, frames);
    if (!file) {
        std::println(stderr, "error: {}", matroska::describe(file.error()));
        return false;
    }
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
             static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return false;
    }
    return true;
}

std::string partial_output_path(std::string_view path) {
    const auto dot = path.rfind('.');
    const auto slash = path.find_last_of("/\\");
    if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash)) {
        return std::string(path.substr(0, dot)) + ".partial" + std::string(path.substr(dot));
    }
    return std::string(path) + ".partial";
}

bool EncodedStreamSink::open(std::string_view path, bool keep_partial, bool defer) {
    path_ = std::string{path};
    keep_partial_ = keep_partial;
    defer_ = defer;
    stdio_ = is_stdio_path(path);
    // Deferring means nothing leaves until close() - so nothing to create
    // yet either. The destination (file or "-", write_frames handles both)
    // is only touched then, exactly as the pre-sink code shape did.
    if (!stdio_ && !defer_) {
        file_.open(path_, std::ios::binary);
        if (!file_) {
            std::println(stderr, "error: cannot open {} for writing", path_);
            return false;
        }
    }
    open_ = true;
    return true;
}

bool EncodedStreamSink::push(std::vector<std::byte>&& frame) {
    if (!defer_) {
        return push(std::span<const std::byte>{frame});
    }
    min_bytes_ = frames_ == 0 ? frame.size() : std::min(min_bytes_, frame.size());
    max_bytes_ = std::max(max_bytes_, frame.size());
    total_bytes_ += frame.size();
    ++frames_;
    deferred_.push_back(std::move(frame));
    return true;
}

bool EncodedStreamSink::push(std::span<const std::byte> frame) {
    if (defer_) {
        deferred_.emplace_back(frame.begin(), frame.end());
    } else if (stdio_) {
        buffered_.insert(buffered_.end(), frame.begin(), frame.end());
    } else {
        file_.write(reinterpret_cast<const char*>(frame.data()),
                    static_cast<std::streamsize>(frame.size()));
        if (!file_) {
            std::println(stderr, "error: cannot write to {}", path_);
            return false;
        }
    }
    min_bytes_ = frames_ == 0 ? frame.size() : std::min(min_bytes_, frame.size());
    max_bytes_ = std::max(max_bytes_, frame.size());
    total_bytes_ += frame.size();
    ++frames_;
    return true;
}

bool EncodedStreamSink::close() {
    open_ = false;
    if (defer_) {
        return write_frames(path_, deferred_);
    }
    if (stdio_) {
        ac3::cli::platform::set_stdio_binary();
        std::cout.write(reinterpret_cast<const char*>(buffered_.data()),
                        static_cast<std::streamsize>(buffered_.size()));
        std::cout.flush();
        if (!std::cout) {
            std::println(stderr, "error: cannot write to stdout");
            return false;
        }
        return true;
    }
    file_.close();
    if (file_.fail()) {
        std::println(stderr, "error: cannot write to {}", path_);
        return false;
    }
    return true;
}

void EncodedStreamSink::abort() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (defer_) {
        // Nothing has left this process yet, so the pre-sink helper IS the
        // right behaviour, note wording and all.
        write_partial_output(path_, keep_partial_, deferred_);
        return;
    }
    if (stdio_) {
        // "beside the intended output" has no meaning for a pipe - see
        // write_partial_output's identical stdout reasoning and wording.
        if (keep_partial_ && frames_ > 0) {
            ac3::cli::platform::set_stdio_binary();
            std::cout.write(reinterpret_cast<const char*>(buffered_.data()),
                            static_cast<std::streamsize>(buffered_.size()));
            std::cout.flush();
            if (std::cout) {
                std::println(stderr,
                             "note: the {} frames already encoded were written to stdout",
                             frames_);
            }
        }
        return;
    }
    file_.close();
    if (keep_partial_ && frames_ > 0) {
        // The bytes are already on disk at the intended path; keep-partial's
        // contract is that they live at the .partial name instead, so a
        // half-finished take can never be mistaken for a finished one.
        const auto partial = partial_output_path(path_);
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path{path_}, std::filesystem::path{partial},
                                 ec);
        if (!ec) {
            std::println(stderr, "note: the {} frames already encoded are kept at {}", frames_,
                         partial);
        } else {
            // Same stance as write_partial_output: report, but the ORIGINAL
            // error stays the one that matters.
            std::println(stderr, "note: could not move the partial output to {} ({})", partial,
                         ec.message());
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{path_}, ec);
    }
}

namespace {

void put_u16(std::ostream& out, std::uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), 2);
}

void put_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), 4);
}

}  // namespace

bool Pcm16RawWavSink::open(std::string_view path, std::uint32_t sample_rate,
                           std::uint16_t channels) {
    path_ = std::string{path};
    data_bytes_ = 0;
    // Create/truncate first, then reopen read+write for the close()-time
    // size patch - the same two-step ac3::io::WavStreamWriter::open uses,
    // and for the same reason: `in|out|trunc` is not reliably
    // create-capable for a not-yet-existing file everywhere.
    {
        std::ofstream create{path_, std::ios::binary | std::ios::trunc};
        if (!create) {
            std::println(stderr, "error: cannot open {} for writing", path_);
            return false;
        }
        // Field for field ac3::io::write_wav_pcm16_raw's header (format tag
        // 1, 16-bit), sizes zero until close() patches them.
        const auto block_align = static_cast<std::uint32_t>(channels) * 2;
        create.write("RIFF", 4);
        put_u32(create, 36);
        create.write("WAVE", 4);
        create.write("fmt ", 4);
        put_u32(create, 16);
        put_u16(create, 1);  // PCM
        put_u16(create, channels);
        put_u32(create, sample_rate);
        put_u32(create, sample_rate * block_align);
        put_u16(create, static_cast<std::uint16_t>(block_align));
        put_u16(create, 16);
        create.write("data", 4);
        put_u32(create, 0);
        if (!create) {
            std::println(stderr, "error: cannot write to {}", path_);
            return false;
        }
    }
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) {
        std::println(stderr, "error: cannot open {} for writing", path_);
        return false;
    }
    file_.seekp(0, std::ios::end);
    open_ = true;
    return true;
}

bool Pcm16RawWavSink::push(std::span<const std::byte> bytes) {
    file_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!file_) {
        std::println(stderr, "error: cannot write to {}", path_);
        return false;
    }
    data_bytes_ += bytes.size();
    return true;
}

bool Pcm16RawWavSink::close() {
    open_ = false;
    const auto data_bytes = static_cast<std::uint32_t>(data_bytes_);
    file_.seekp(4, std::ios::beg);
    put_u32(file_, 36 + data_bytes);
    file_.seekp(40, std::ios::beg);
    put_u32(file_, data_bytes);
    file_.close();
    if (file_.fail()) {
        std::println(stderr, "error: cannot write to {}", path_);
        return false;
    }
    return true;
}

void Pcm16RawWavSink::abort() {
    if (!open_) {
        return;
    }
    open_ = false;
    file_.close();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path{path_}, ec);
}

bool write_repeated_frame(std::string_view path, std::span<const std::byte> frame,
                          std::uint64_t count) {
    const auto emit = [&](std::ostream& out) {
        for (std::uint64_t i = 0; i < count; ++i) {
            out.write(reinterpret_cast<const char*>(frame.data()),
                      static_cast<std::streamsize>(frame.size()));
        }
        return static_cast<bool>(out);
    };
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        const bool ok = emit(std::cout);
        std::cout.flush();
        if (!ok || !std::cout) {
            std::println(stderr, "error: cannot write to stdout");
            return false;
        }
        return true;
    }
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", path);
        return false;
    }
    return emit(out);
}

void write_partial_output(std::string_view out_path, bool keep_partial,
                          std::span<const std::vector<std::byte>> frames) {
    if (!keep_partial || frames.empty()) {
        return;
    }
    if (is_stdio_path(out_path)) {
        // "beside the intended output" (partial_output_path's naming below)
        // has no meaning for a pipe - stdout IS the intended output, and a
        // literal file called "-.partial" is not what keep-partial means
        // here. So the frames already encoded go straight to stdout instead,
        // the closest equivalent a single output stream can offer.
        if (write_frames(out_path, frames)) {
            std::println(stderr, "note: the {} frames already encoded were written to stdout",
                         frames.size());
        }
        return;
    }
    const auto partial = partial_output_path(out_path);
    if (write_frames(partial, frames)) {
        std::println(stderr, "note: the {} frames already encoded are kept at {}", frames.size(),
                     partial);
    }
}

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
    if (is_stdio_path(path)) {
        // stdin's length is unknown up front, so this path keeps the
        // iterator read (and to_bytes' copy) the file branch below no
        // longer needs.
        ac3::cli::platform::set_stdio_binary();
        const std::vector<char> raw{std::istreambuf_iterator<char>(std::cin),
                                    std::istreambuf_iterator<char>()};
        return to_bytes(raw);
    }
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return {};
    }
    // Sized read straight into the byte buffer: the iterator+to_bytes route
    // held the file twice (char copy plus byte copy) at its return point.
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) {
        return {};
    }
    in.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    in.read(reinterpret_cast<char*>(bytes.data()), end);
    if (in.gcount() != end) {
        return {};
    }
    return bytes;
}

std::expected<ac3::io::WavData, ac3::io::WavError> read_wav_arg(std::string_view path) {
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        return ac3::io::read_wav(std::cin);
    }
    return ac3::io::read_wav(std::string{path});
}

bool PlanarWavSink::open(std::string_view path, std::uint32_t sample_rate, std::size_t slots,
                         std::span<const std::size_t> order) {
    path_ = std::string{path};
    stdio_ = is_stdio_path(path);
    sample_rate_ = sample_rate;
    slots_.assign(slots, {});
    consumed_.assign(slots, 0);
    order_.assign(order.begin(), order.end());
    if (order_.empty()) {
        order_.resize(slots);
        for (std::size_t i = 0; i < slots; ++i) {
            order_[i] = i;
        }
    }
    if (!stdio_) {
        if (!writer_.open(path_, sample_rate, static_cast<std::uint16_t>(slots))) {
            return false;
        }
    }
    open_ = true;
    return true;
}

bool PlanarWavSink::append(std::size_t slot, std::span<const float> samples) {
    auto& buffer = slots_[slot];
    buffer.insert(buffer.end(), samples.begin(), samples.end());
    return drain();
}

std::expected<void, ac3::io::WavError> PlanarWavSink::close() {
    if (!open_) {
        return std::unexpected(ac3::io::WavError::kCannotOpen);
    }
    open_ = false;
    if (stdio_) {
        return write_wav_f32_arg(path_, slots_, sample_rate_, order_);
    }
    if (!drain()) {
        writer_.close();
        return std::unexpected(ac3::io::WavError::kCannotOpen);
    }
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        if (slots_[s].size() != consumed_[s]) {
            std::println(stderr,
                         "warning: dropped a ragged tail the substreams never evened out");
            break;
        }
    }
    writer_.close();
    return {};
}

void PlanarWavSink::abort() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (!stdio_) {
        writer_.close();
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{path_}, ec);
    }
}

bool PlanarWavSink::drain() {
    if (stdio_) {
        return true;
    }
    std::size_t ready = std::numeric_limits<std::size_t>::max();
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        ready = std::min(ready, slots_[s].size() - consumed_[s]);
    }
    if (ready == 0 || ready == std::numeric_limits<std::size_t>::max()) {
        return true;
    }
    scratch_.resize(ready * slots_.size());
    for (std::size_t pos = 0; pos < ready; ++pos) {
        for (std::size_t w = 0; w < order_.size(); ++w) {
            const auto slot = order_[w];
            scratch_[pos * order_.size() + w] = slots_[slot][consumed_[slot] + pos];
        }
    }
    if (!writer_.write(scratch_)) {
        return false;
    }
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        consumed_[s] += ready;
        // Keep the carry small: once the consumed prefix dominates,
        // shift the remainder down rather than growing forever.
        if (consumed_[s] > 8192 && consumed_[s] > slots_[s].size() / 2) {
            slots_[s].erase(slots_[s].begin(),
                            slots_[s].begin() + static_cast<std::ptrdiff_t>(consumed_[s]));
            consumed_[s] = 0;
        }
    }
    return true;
}

std::string meter_bar(double db, int width) {
    std::string bar(static_cast<std::size_t>(width), '-');
    const auto filled = static_cast<int>(std::lround(ac3::analysis::meter_fraction(db) * width));
    for (int i = 0; i < filled; ++i) {
        bar[static_cast<std::size_t>(i)] = '#';
    }
    return bar;
}

void print_channel_summary(const ac3::analysis::LevelMeter& meter, FILE* out) {
    const auto acmod = meter.acmod();
    const bool lfe = meter.lfe();
    std::println(out, "");
    std::println(out, "per-channel levels ({}):", ac3::analysis::layout_name(acmod, lfe));
    std::println(out, "  {:<4} {:>8} {:>8}  {:<20} {}", "ch", "peak", "rms",
                "peak (-60..0 dBFS)", "clipped");
    for (int ch = 0; ch < meter.channel_count(); ++ch) {
        const auto& stats = meter.summary()[static_cast<std::size_t>(ch)];
        std::println(out, "  {:<4} {:>8.2f} {:>8.2f}  [{}] {}",
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
        std::println(out, "  soundfield: {:.0f}° azimuth, focus {:.2f} (1.0 = a single speaker)",
                     azimuth == 0.0 ? 0.0 : azimuth, field.magnitude);
    }
}

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
    (void)std::fflush(stdout);  // best-effort: a live meter with nothing left to do on failure
}

bool resolve_layout(std::string_view name, ac3::plan::Codec codec, ac3::plan::Plan& plan_out,
                    std::string& label) {
    if (const auto id = ac3::plan::parse_layout(name)) {
        if (!ac3::plan::carries(codec, *id)) {
            std::println(stderr, "error: {} cannot carry {} - {}", ac3::plan::codec_label(codec),
                         ac3::plan::layout(*id).label,
                         ac3::plan::describe(ac3::plan::PlanError::kLayoutNeedsEac3));
            return false;
        }
        plan_out.layout = *id;
        plan_out.custom_locations = std::nullopt;
        label = std::string(ac3::plan::layout(*id).label);
        return true;
    }
    const auto custom = ac3::plan::parse_channels(name);
    if (!custom) {
        std::println(stderr, "error: unknown layout '{}' ({})", name,
                     ac3::plan::layout_names(codec));
        return false;
    }
    const auto allocated = ac3::eac3::chanmap::allocate(*custom);
    if (!allocated) {
        std::println(stderr, "error: channel selection '{}' is invalid - {}", name,
                     ac3::eac3::chanmap::describe(allocated.error()));
        return false;
    }
    if (codec == ac3::plan::Codec::kAc3 && !allocated->dependents.empty()) {
        std::println(stderr, "error: {} cannot carry '{}' - {}", ac3::plan::codec_label(codec),
                     name, ac3::plan::describe(ac3::plan::PlanError::kLayoutNeedsEac3));
        return false;
    }
    plan_out.custom_locations = custom;
    label = ac3::plan::format_channels(*custom);
    return true;
}

std::optional<ac3::SampleRate> wav_sample_rate(std::uint32_t hz, std::string_view codec,
                                               bool eac3) {
    switch (hz) {
        case 48000: return ac3::SampleRate::k48000;
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        case 24000: if (eac3) return ac3::SampleRate::k24000; break;
        case 22050: if (eac3) return ac3::SampleRate::k22050; break;
        case 16000: if (eac3) return ac3::SampleRate::k16000; break;
        default: break;
    }
    std::println(stderr, "error: sample rate {} is not legal for {} (need {})", hz, codec,
                eac3 ? "32/44.1/48 kHz, or 16/22.05/24 kHz" : "32/44.1/48 kHz");
    return std::nullopt;
}

std::optional<ac3::plan::Routing> routing_or_error(const ac3::plan::Plan& p, std::size_t channels) {
    auto routing = plan::route(plan::resolve(p), channels, p.meta.cmixlev, p.meta.surmixlev);
    if (!routing) {
        std::println(stderr, "error: {} channels - {}", channels,
                     plan::describe(plan::PlanError::kNoSourceLayout));
        return std::nullopt;
    }
    return routing;
}

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

}  // namespace ac3cli
