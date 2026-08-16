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
#include "ac3/capture/capture.hpp"
#include "ac3/capture/resampler.hpp"
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
#include "ac3/platform/audio_backend.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/monitor.hpp"
#include "ac3/sinks/passthrough.hpp"
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

// Everything a command accepts after its positional arguments, in any order.
// The metadata group is ac3::plan::Metadata verbatim; drc_scale is decode-
// side local, because nothing an encoder is configured with corresponds to
// it; sources/map_spec describe routing rather than metadata, but share this
// same trailing-options surface (parse_options) the way dialnorm2= already
// shares it despite being layout-1+1-specific - a command that has no use
// for a field simply never sets it.
struct Options {
    plan::Metadata p{};
    // Decoder side, for 'decode'.
    double drc_scale = 0.0;
    // Each src= occurrence, in order given - additional input sources beyond
    // the primary positional argument. encode/eac3-encode only; empty unless
    // multi-source input is in play.
    std::vector<std::string> sources;
    // The raw map= text, if given - parsed into a plan::Assignment once the
    // sources are loaded and their channel counts are known, which
    // parse_options itself cannot do (it only sees command-line text, not
    // opened files).
    std::optional<std::string> map_spec;
    // Each offset= occurrence: (sourceIndex, seconds) - leading silence ahead
    // of that source's own audio, in the same 0-based numbering src=
    // establishes (0 = the primary positional argument, 1..N = each src= in
    // order). encode/eac3-encode only, including the classic single-file
    // path, where source 0 is the only source there is. A given sourceIndex
    // may appear more than once; the last occurrence wins (see
    // offset_samples_for).
    std::vector<std::pair<std::size_t, double>> offsets;
    // Atmos object signing (atmos/atmos-path/atmos-encode). Off unless the
    // operator both asks (sign-objects) and provides a key - either
    // signing-key=<path> here, or the AC3FORGE_SIGNING_KEY[_FILE] env vars
    // load_signing_key() falls back to. The key is never stored by this tool;
    // see docs/concepts/object-signing.md.
    bool sign_objects = false;
    std::optional<std::string> signing_key;
    // 'live' only: a second ("slave") capture device index, same numbering
    // ac3::capture::enumerate_devices()/'devices' uses and the capture_device
    // positional already reads. Unset means the classic single-device
    // session, unchanged from before this option existed.
    std::optional<int> capture2 = std::nullopt;
    // 'record'/'live' only: write straight to Matroska instead of the bare
    // elementary stream they write by default - the same shape of choice the
    // GUI's own Container combo offers (EncoderController::containerIndex ==
    // kContainerMatroska), see write_frames_or_mux. Off by default, matching
    // every bare-token/off-by-default field here: a plain invocation writes
    // exactly the .ac3/.ec3 it always has.
    bool matroska_container = false;
    // Off by default, matching every bare token here - keep whatever frames
    // a failed encode already produced, written beside the intended output
    // as <name>.partial.<ext> instead of discarded outright. The same
    // "named and kept, never silently discarded" behaviour the GUI's own
    // keepPartialOutput preference gives EncoderController's file encodes
    // (see gui/encoder_controller.cpp's partial_output_path), offered here
    // per invocation rather than as a standing preference - see
    // write_partial_output.
    bool keep_partial = false;
    // The §7.9.4 fast forward MDCT (plan::Tools::fast_mdct), on by default
    // like the library configs it feeds; fast-mdct=off forces the direct
    // §8.2.3.2 reference form wherever this command encodes (encode/sine and
    // the atmos/record/live session builders), the same key=off shape
    // surmixlev=/lfemix= already use. E-AC-3's own tools= string reaches the
    // same field with its own tokens ("nofastmdct" to force direct, matching
    // "noatten"; the old opt-in "fastmdct" parses as a no-op) - AC-3 has no
    // tools= string to extend, so this option is its equivalent, the same
    // relationship 'couple' has to cpl/cpl:N. The bare word 'fast-mdct'
    // (the opt-in spelling from when this defaulted off) stays accepted and
    // now names what already happens.
    bool fast_mdct = true;
    // 'qc' only: which delivery gate(s) to check the measurement against -
    // one of ac3::meta::kQcPresetNames, or "all" to check every preset.
    // Unset (measure-only, no gate) is the default - a plain
    // 'ac3cli qc <file>' just reports the numbers, no pass/fail verdict.
    std::optional<std::string> qc_preset;
};

bool parse_double(std::string_view text, double& out) {
    // from_chars for floating point needs the locale-independent form, which
    // is what a command line gives.
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

// Returns false and prints the offending token on anything unrecognised: a
// silently ignored metadata flag looks exactly like metadata that did not work.
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
        if (token == "sign-objects") {
            out.sign_objects = true;
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

// Reads a loudness measurement someone else already pushed every sample
// into, reports it the same way every dialnorm=auto path does, and returns
// the dialnorm it implies. Factored out of measured_dialnorm/
// measured_dialnorm_channel below so a measurement built incrementally
// across many frames (the src=/map= routed-programme pre-pass) reports
// itself identically to one built from a single whole-buffer push - same
// text, same rounding, one place either could go wrong. `programme` is the
// println's leading label ("Ch1"/"Ch2"), empty for a whole-programme
// measurement that is not about one dual-mono channel; `field` is the
// bitstream field this measurement feeds ("dialnorm"/"dialnorm2").
std::optional<int> finish_measurement(const ac3::meta::LoudnessMeter& meter,
                                      std::string_view programme, std::string_view field) {
    const auto lkfs = meter.integrated_lkfs();
    if (!lkfs) {
        return std::nullopt;
    }
    const int dialnorm = ac3::meta::dialnorm_from_lkfs(*lkfs);
    if (programme.empty()) {
        std::println("measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", *lkfs, field, dialnorm);
    } else {
        std::println("{} measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", programme, *lkfs,
                     field, dialnorm);
    }
    return dialnorm;
}

// BS.1770 integrated loudness of a whole WAV, and the dialnorm it implies.
// Never meaningful for a dual-mono (1+1) target - Ch1 and Ch2 are two
// unrelated programmes sharing one syncframe (§E1.3, no downmix between
// them), so a single BS.1770 pass across both channels would measure a
// blend of two different things rather than either programme's own level;
// callers route dual mono through measured_dialnorm_channel on each
// programme's own channel alone instead.
std::optional<int> measured_dialnorm(const ac3::io::WavData& wav, ac3::SampleRate rate,
                                     ac3::Acmod acmod, bool lfe) {
    ac3::meta::LoudnessMeter meter{rate, acmod, lfe};
    std::vector<std::span<const float>> views;
    views.reserve(wav.channels.size());
    for (const auto& channel : wav.channels) {
        views.emplace_back(channel);
    }
    meter.push(views);
    return finish_measurement(meter, {}, "dialnorm");
}

// Same measurement, for one dual-mono programme's own channel alone - never a
// programme's worth of BS.1770 surround weighting, since a 1+1 channel is not
// part of a soundfield. `programme`/`field` are finish_measurement's own
// labels above - "Ch1"/"dialnorm" or "Ch2"/"dialnorm2", the two programmes
// sharing this one function since the measurement itself does not differ.
std::optional<int> measured_dialnorm_channel(std::span<const float> channel, ac3::SampleRate rate,
                                             std::string_view programme, std::string_view field) {
    ac3::meta::LoudnessMeter meter{rate, ac3::Acmod::k1_0, false};
    const std::array<std::span<const float>, 1> views{channel};
    meter.push(views);
    return finish_measurement(meter, programme, field);
}

// Dual mono's Ch1/Ch2 arrive as either one two-channel file or two mono ones;
// this settles which shape `wav` is in and merges a second file's channel in
// when there is one, so everything downstream sees a plain two-channel source
// the same way it always has - `plan::route`'s own 1+1 handling only ever
// looks at the channel count, never how many files it came from.
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

// The conventional Unix "-" file argument: a lone dash means stdin for an
// input path or stdout for an output path, the same convention ffmpeg, sox
// and most other Unix tools use for pipe-based workflows (e.g.
// `ac3cli encode - - 448 couple < in.wav > out.ac3`). Checked by exact
// string match only - a path that merely starts with '-' is an ordinary
// (if oddly named) filename, not this convention.
bool is_stdio_path(std::string_view path) { return path == "-"; }

// Where a command's human-readable status report goes, once out_path's own
// destination is settled: stdout as always, unless out_path IS "-" - the
// binary payload itself is going to stdout then, and a status line like
// "encoded N frames..." landing in the middle of that stream would corrupt
// whatever is reading it downstream. The same split ffmpeg and friends make
// between their progress/log output and the media they actually pipe.
FILE* status_stream(std::string_view out_path) { return is_stdio_path(out_path) ? stderr : stdout; }

std::vector<std::byte> to_bytes(std::span<const char> raw) {
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

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

// Writes `frames` either as a bare elementary stream (write_frames above) or,
// when `matroska` is set, muxed into Matroska - the choice 'record'/'live's
// own container= token (and the GUI's Container combo) offer. `track` is
// built by the caller from what it already knows about the session (codec,
// sample rate, coded channel count) rather than scanned off the bitstream
// the way 'mkv' reads an arbitrary already-encoded file: record/live just
// finished constructing the encoder themselves, so there is nothing to
// rediscover. Kept beside write_frames rather than folded into it - most
// callers have no AudioTrack to give it, and 'mkv' itself stays separate too,
// since ITS track comes from ac3::io::scan(), not a caller-supplied one.
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

// Where a failed encode's frames land when keep-partial is given: ".partial"
// spliced in before the suffix, so "out.ec3" keeps its half-finished take as
// "out.partial.ec3" - the same naming EncoderController::partial_output_path
// gives the GUI's own keepPartialOutput preference (see gui/
// encoder_controller.cpp), so a file produced either way is named alike.
std::string partial_output_path(std::string_view path) {
    const auto dot = path.rfind('.');
    const auto slash = path.find_last_of("/\\");
    if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash)) {
        return std::string(path.substr(0, dot)) + ".partial" + std::string(path.substr(dot));
    }
    return std::string(path) + ".partial";
}

// Writes whatever frames a failed encode already produced to
// partial_output_path(out_path) when keep_partial asked for it and there is
// at least one - "named and kept, never silently discarded", the same rule
// the GUI's own keep-partial-output preference follows. A no-op (silently)
// when keep_partial is false or nothing was encoded yet; a write failure for
// the partial itself is reported but does not change the caller's own exit
// code, since the ORIGINAL error is still the one that matters.
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
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        const std::vector<char> raw{std::istreambuf_iterator<char>(std::cin),
                                    std::istreambuf_iterator<char>()};
        return to_bytes(raw);
    }
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return {};
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    return to_bytes(raw);
}

// Wraps ac3::io::read_wav to honor the "-" stdin convention (is_stdio_path
// above): "-" reads the WAV from stdin, binary mode set first, instead of
// opening a file with that literal name.
std::expected<ac3::io::WavData, ac3::io::WavError> read_wav_arg(std::string_view path) {
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        return ac3::io::read_wav(std::cin);
    }
    return ac3::io::read_wav(std::string{path});
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
// `out` defaults to stdout for every existing caller; the only ones that
// pass anything else are the "-" stdout-output commands (encode/eac3-encode/
// atmos-encode/decode), which redirect it to stderr so this human-readable
// report doesn't land in the middle of the binary stream those commands may
// be writing to the very same stdout - see status_stream()'s own comment.
void print_channel_summary(const ac3::analysis::LevelMeter& meter, FILE* out = stdout) {
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
    (void)std::fflush(stdout);  // best-effort: a live meter with nothing left to do on failure
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

// Sets `plan`'s channels from `name` and writes a human-readable label for
// it into `label`, reporting a bad token against the set the codec can
// actually carry (so asking AC-3 for 7.1.4 says which of the two things is
// wrong) or false on anything neither a named layout nor a channel list
// accepts. Tried in that order: a name recognised by parse_layout wins, so a
// custom list can never shadow one of the seven presets.
bool resolve_layout(std::string_view name, plan::Codec codec, plan::Plan& plan, std::string& label) {
    if (const auto id = ac3::plan::parse_layout(name)) {
        if (!ac3::plan::carries(codec, *id)) {
            std::println(stderr, "error: {} cannot carry {} - {}", ac3::plan::codec_label(codec),
                         ac3::plan::layout(*id).label,
                         ac3::plan::describe(ac3::plan::PlanError::kLayoutNeedsEac3));
            return false;
        }
        plan.layout = *id;
        plan.custom_locations = std::nullopt;
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
    plan.custom_locations = custom;
    label = ac3::plan::format_channels(*custom);
    return true;
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

// A WAV's rate as an fscod (or, for E-AC-3, fscod2), or a diagnosis. Shared
// because every encode path asks the same question. Classic AC-3 has only
// A/52 Table 5.6's three rates; E-AC-3 additionally accepts the three Annex E
// fscod2 half rates (24/22.05/16 kHz), which have no AC-3 counterpart at all.
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

// A source's channels routed onto a plan's coded channels, or a diagnosis.
std::optional<plan::Routing> routing_or_error(const plan::Plan& p, std::size_t channels) {
    auto routing = plan::route(plan::resolve(p), channels, p.meta.cmixlev, p.meta.surmixlev);
    if (!routing) {
        std::println(stderr, "error: {} channels - {}", channels,
                     plan::describe(plan::PlanError::kNoSourceLayout));
        return std::nullopt;
    }
    return routing;
}

// --- multi-source input (src=/map=) ------------------------------------------
//
// The primary positional file plus every src= path, opened and shaped -
// separate from the classic single-file/in2.wav path below, which stays
// completely untouched so a command line that never mentions src=/map=
// behaves exactly as it always has, byte for byte. This is deliberately not
// unified with that path: the two have genuinely different data shapes (one
// ac3::io::WavData vs several), and duplicating the small amount that does
// overlap costs far less than a shared abstraction would risk.

struct LoadedSources {
    std::vector<ac3::io::WavData> wavs;
    std::vector<plan::SourceShape> shapes;
    std::uint32_t sample_rate = 0;
    // Per-source leading silence, in samples at sample_rate - parallel to
    // wavs/shapes (index 0 = in_path, 1..N = each src= in load order, the
    // same numbering offset= addresses), computed from Options::offsets once
    // sample_rate is known. Applied in gather_frame, ahead of a source's own
    // samples.
    std::vector<std::size_t> offset_samples;
    // The longest source's frame count once ITS OWN offset is applied, not
    // the longest raw source alone - a run covers everything any loaded
    // source carries, including whatever offset= shifted it by. Each source
    // still holds its own last real sample past its own end (see
    // gather_frame), so a short source does not go silent early relative to
    // a long one.
    std::size_t total_frames = 0;
};

// The leading-silence sample count offset= asked for on source `index`
// (the same 0-based numbering src= establishes), from every offset= the
// operator gave - the last occurrence for that index wins, since
// parse_options does not dedupe. Shared by load_sources (every loaded
// source) and the classic single-file path (source index always 0).
std::size_t offset_samples_for(std::span<const std::pair<std::size_t, double>> offsets,
                               std::size_t index, std::uint32_t sample_rate) {
    std::size_t result = 0;
    for (const auto& [i, seconds] : offsets) {
        if (i == index) {
            result = static_cast<std::size_t>(std::lround(seconds * static_cast<double>(sample_rate)));
        }
    }
    return result;
}

// Opens `in_path` plus every path in `extra`, in that order, and checks they
// all share one sample rate - plan::render has no notion of resampling, and
// a silently mismatched pair would drift apart rather than error.
std::optional<LoadedSources> load_sources(
    std::string_view in_path, std::span<const std::string> extra,
    std::span<const std::pair<std::size_t, double>> offsets) {
    auto primary = ac3::io::read_wav(std::string{in_path});
    if (!primary) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(primary.error()));
        return std::nullopt;
    }
    LoadedSources out;
    out.sample_rate = primary->sample_rate;
    out.shapes.push_back({.channels = primary->channels.size(), .label = std::string{in_path}});
    out.wavs.push_back(std::move(*primary));

    for (const auto& path : extra) {
        auto wav = ac3::io::read_wav(path);
        if (!wav) {
            std::println(stderr, "error: {}: {}", path, ac3::io::describe(wav.error()));
            return std::nullopt;
        }
        if (wav->sample_rate != out.sample_rate) {
            std::println(stderr,
                         "error: {} is {} Hz, but {} is {} Hz - every source must share a "
                         "sample rate",
                         path, wav->sample_rate, in_path, out.sample_rate);
            return std::nullopt;
        }
        out.shapes.push_back({.channels = wav->channels.size(), .label = path});
        out.wavs.push_back(std::move(*wav));
    }

    // A sourceIndex offset= names beyond how many sources actually loaded has
    // nothing to shift - ignored rather than an error, the same way an unused
    // trailing option elsewhere in this file is simply inert.
    out.offset_samples.resize(out.wavs.size());
    for (std::size_t i = 0; i < out.wavs.size(); ++i) {
        out.offset_samples[i] = offset_samples_for(offsets, i, out.sample_rate);
    }
    out.total_frames = 0;
    for (std::size_t i = 0; i < out.wavs.size(); ++i) {
        out.total_frames =
            std::max(out.total_frames, out.offset_samples[i] + out.wavs[i].frame_count());
    }
    return out;
}

// The routing for a loaded, possibly multi-source run: explicit assignment
// when map= was given, else exactly routing_or_error's single-source
// automatic panning - map= is opt-in, so omitting it is defined to behave
// exactly as if src=/map= did not exist at all. Dual mono is routed through
// dual_mono_routing rather than the general location-based route(): a 1+1
// target has no soundstage for a location token to mean anything on, so
// map= for it names programmes (p1/p2), not locations.
std::optional<plan::Routing> routing_for_sources(const plan::Plan& p, const LoadedSources& sources,
                                                 const std::optional<std::string>& map_spec) {
    if (!map_spec) {
        if (sources.shapes.size() > 1) {
            std::println(stderr,
                         "error: more than one source needs map= to say where each channel "
                         "goes ({})",
                         plan::kAssignmentSyntax);
            return std::nullopt;
        }
        return routing_or_error(p, sources.shapes.front().channels);
    }
    plan::Assignment assignment;
    if (!plan::parse_assignment(*map_spec, sources.shapes, assignment)) {
        std::println(stderr, "error: bad map= spec ({})", plan::kAssignmentSyntax);
        return std::nullopt;
    }
    const auto target = plan::resolve(p);
    const bool dual_mono = target.bed_acmod == ac3::Acmod::kDualMono;
    if (!dual_mono) {
        // route() (below) only carries kLocation rows into the output - see
        // its own comment. obj/objm reach it here because this CLI has no
        // object-assembly path of its own (that is the GUI's, see
        // encoder_controller.cpp's encodeObjects); p1/p2 reach it only if a
        // caller wrote them for a target that isn't dual mono, so route()
        // would drop those too, for lack of anywhere to route them to. Either
        // way, a channel silently contributing nothing is worth a warning
        // rather than a surprise in the output.
        for (const auto kind : {plan::DestinationKind::kObject, plan::DestinationKind::kObjectMono,
                                plan::DestinationKind::kProgramme1,
                                plan::DestinationKind::kProgramme2}) {
            for (const auto& [s, c] : assignment.rows_of(kind)) {
                std::println(stderr,
                             "warning: {}.{} maps to '{}', which this command has no way to "
                             "carry - that channel contributes nothing to the output",
                             s, c, plan::format_destination(assignment.at(s, c)));
            }
        }
    }
    auto routing = dual_mono ? plan::dual_mono_routing(sources.shapes, assignment)
                             : plan::route(target, sources.shapes, assignment);
    if (!routing) {
        std::println(stderr, "error: map= does not resolve to a valid routing for this format");
        return std::nullopt;
    }
    return routing;
}

// Fills `dest` (one entry per flattened source channel, source 0 first) with
// samples [start, start + kSamplesPerFrame) from `sources`, applying each
// source's own offset= leading silence ahead of its real samples, then
// holding its own last real sample past its own end - independently per
// source, the same tail padding the classic path already applies to its one
// file, so a short source loaded alongside a long one goes silent-by-
// holding at its own end rather than at whichever source happens to be
// shortest overall.
void gather_frame(const LoadedSources& sources, std::size_t start,
                  std::vector<std::vector<float>>& dest) {
    std::size_t flat = 0;
    for (std::size_t s = 0; s < sources.wavs.size(); ++s) {
        const auto& wav = sources.wavs[s];
        const std::size_t total = wav.frame_count();
        const std::size_t offset = sources.offset_samples[s];
        for (const auto& channel : wav.channels) {
            const float hold = total > 0 ? channel[total - 1] : 0.0f;
            auto& out = dest[flat];
            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                if (at < offset) {
                    out[static_cast<std::size_t>(i)] = 0.0f;
                    continue;
                }
                const std::size_t shifted = at - offset;
                out[static_cast<std::size_t>(i)] = shifted < total ? channel[shifted] : hold;
            }
            ++flat;
        }
    }
}

// Says what the routing did, so a run that quietly left half a layout silent
// is visible rather than something to be discovered later on the meters.
// `label` is whatever resolve_layout printed for this plan - a named
// layout's label, or the channel list a custom selection was parsed from.
// `out` defaults to stdout; see print_channel_summary's comment just above -
// the same reasoning applies here.
void print_routing(const plan::Plan& p, const plan::Routing& routing, std::string_view label,
                   FILE* out = stdout) {
    if (routing.is_permutation()) {
        std::println(out, "  source carried directly into {}", label);
        return;
    }
    const auto names = plan::coded_channel_names(plan::resolve(p));
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
    std::println(out, "  {} source channels rendered onto {}", routing.source_channels, label);
    if (!silent.empty()) {
        std::println(out, "  silent (the source carries nothing that belongs there): {}", silent);
    }
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
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm")
                                            : finish_measurement(*whole, {}, "dialnorm");
            if (!measured) {
                std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2");
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
        std::println("encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), plan::format_vbr(p.vbr), sources->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
        std::println("  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     min_bytes, max_bytes, mean_bytes, mean_kbps);
    } else {
        std::println("encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), bitrate, sources->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label);
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
    auto wav = read_wav_arg(in_path);
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
        return 1;
    }
    const auto sr = wav_sample_rate(wav->sample_rate, "E-AC-3", true);
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
        const auto id = plan::layout_for_source(wav->channels.size());
        if (!id) {
            std::println(stderr, "error: {} channels - {}", wav->channels.size(),
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
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm")
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe);
        if (!measured) {
            std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 = measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2");
        if (!measured2) {
            std::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, wav->channels.size());
    if (!routing) {
        return 1;
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    assert(static_cast<int>(nchans) == encoder.channel_count());
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, wav->sample_rate);
    const std::size_t frame_count = wav->frame_count();
    const std::size_t total = offset + frame_count;

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
        // Hold the last real sample past end-of-file rather than dropping to
        // hard zero - see run_encode's identical padding for why: a sudden
        // drop to silence is itself a transient the encoder would (correctly)
        // spend a block-switch on, for a discontinuity that only exists
        // because this frame ends mid-buffer. Ahead of the source's own
        // samples, offset= silence is real silence, not padding.
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
    // See run_encode's identical status_stream() comment: out_path == "-"
    // means the E-AC-3 bytes just written own stdout, so this report goes to
    // stderr instead.
    const auto status = status_stream(out_path);
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
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(wav->sample_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        std::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), plan::format_vbr(p.vbr), wav->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
        std::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     min_bytes, max_bytes, mean_bytes, mean_kbps);
    } else {
        std::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     frames.size(), bitrate, wav->sample_rate, label, nchans,
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
    const auto wav = read_wav_arg(in_path);
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    const auto sr = wav_sample_rate(wav->sample_rate, "E-AC-3", true);
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
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct},
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
        // With paths_path, the object placement moves - evaluated at the
        // frame's END time, the same convention run_atmos_path and the GUI's
        // encodeObjects use. Without it, every frame reuses the one static
        // placement computed above, byte-identical to before this argument
        // existed.
        auto unit = paths ? encoder.encode_frame(
                                views, ac3::oba::evaluate_placements(
                                           *paths, static_cast<double>(start + ac3::kSamplesPerFrame) /
                                                       static_cast<double>(wav->sample_rate)))
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
    // See run_encode's identical status_stream() comment: out_path == "-"
    // means the E-AC-3 bytes just written own stdout, so this report goes to
    // stderr instead.
    const auto status = status_stream(out_path);
    std::println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) to {}", out.size(),
                bitrate, wav->sample_rate, out_path);
    std::println(status,
                 "  {} objects from {} source channels + the bed's LFE = {} objects, "
                 "JOC over a 5.1 downmix",
                 count, wav->channels.size(), ac3::oba::object_count(encoder.program()));
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
               int device_index, const Options& meta) {
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
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm")
                                            : finish_measurement(*whole, {}, "dialnorm");
            if (!measured) {
                std::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2");
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
    std::println("encoded {} frames ({} kbps, {} Hz, {}) to {}", frames.size(), bitrate,
                 sources->sample_rate, ac3::analysis::layout_name(config.acmod, config.lfe),
                 out_path);
    print_routing(p, *routing, label);
    print_channel_summary(meter);
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
    auto wav = read_wav_arg(in_path);
    if (!wav) {
        std::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
        return 1;
    }
    if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
        return 1;
    }
    const auto sr = wav_sample_rate(wav->sample_rate, "AC-3", false);
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
        const auto id = plan::layout_for_source(wav->channels.size());
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            std::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         wav->channels.size());
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
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm")
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe);
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
        const auto measured2 = measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2");
        if (!measured2) {
            std::println(stderr,
                         "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, wav->channels.size());
    if (!routing) {
        return 1;
    }

    const auto config = plan::ac3_config(p);
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, wav->sample_rate};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, wav->sample_rate);
    const std::size_t frame_count = wav->frame_count();
    const std::size_t total = offset + frame_count;

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
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes just written above already own stdout in that case,
    // and this report must not land in the middle of them.
    const auto status = status_stream(out_path);
    std::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", frames.size(), bitrate,
                wav->sample_rate, ac3::analysis::layout_name(config.acmod, config.lfe), out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
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
        if (!decoded->has_value()) {
            // §3.7: this access unit's frame(s) are being held back pending
            // transient pre-noise processing (Eac3Decoder::decode_access_unit's
            // own doc comment) - nothing new to append yet, not an error.
            continue;
        }
        const auto& out = **decoded;
        if (pcm.empty()) {
            first = out;
            pcm.resize(out.channels.size());
        }
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), out.channels[ch].begin(), out.channels[ch].end());
        }
    }
    // Whatever transient pre-noise processing was still holding back at
    // end-of-stream. flush() returns raw per-substream results rather than
    // assembled access units (see its own doc comment); appended directly
    // here, which is exactly right for the common case this covers - a
    // single independent substream with no dependents, where a substream's
    // own channel order already matches the access unit's.
    for (auto& substream : decoder.flush()) {
        if (pcm.empty()) {
            ac3::DecodedAccessUnit synthesized;
            synthesized.sample_rate = substream.sample_rate;
            synthesized.acmod = substream.acmod;
            synthesized.dialnorm = substream.dialnorm;
            synthesized.substream_count = 1;
            synthesized.layout = ac3::eac3::chanmap::expand(substream.location_map());
            first = synthesized;
            pcm.resize(substream.channels.size());
        }
        for (std::size_t ch = 0; ch < substream.channels.size(); ++ch) {
            pcm[ch].insert(pcm[ch].end(), substream.channels[ch].begin(),
                           substream.channels[ch].end());
        }
    }
    if (pcm.empty()) {
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
    if (first.acmod == ac3::Acmod::kDualMono) {
        const auto written = write_wav_f32_arg(out_path, pcm, sample_rate_hz(first.sample_rate));
        if (!written) {
            std::println(stderr, "error: {}", ac3::io::describe(written.error()));
            return 1;
        }
        std::println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                     units->size(), first.substream_count, out_path);
        std::println(status,
                     "  {} channels, {} Hz: Ch1 Ch2 (1+1 dual mono - two programmes, not a "
                     "soundfield)",
                     pcm.size(), sample_rate_hz(first.sample_rate));
        return 0;
    }
    // The same WAV speaker order the encode side reads a file in, so a stream
    // decoded here and re-encoded lands every channel back where it started.
    const auto map = plan::wav_order(
        std::span{first.layout.items}.first(static_cast<std::size_t>(first.layout.count)));
    const auto written = write_wav_f32_arg(out_path, pcm, sample_rate_hz(first.sample_rate), map);
    if (!written) {
        std::println(stderr, "error: {}", ac3::io::describe(written.error()));
        return 1;
    }
    std::string speakers;
    for (const auto index : map) {
        speakers += ac3::eac3::chanmap::name(first.layout[static_cast<int>(index)]);
        speakers += ' ';
    }
    std::println(status, "decoded {} E-AC-3 access units ({} substreams each) -> {}",
                 units->size(), first.substream_count, out_path);
    std::println(status, "  {} channels, {} Hz: {}", map.size(), sample_rate_hz(first.sample_rate),
                 speakers);
    return 0;
}

int run_decode(std::string_view in_path, std::string_view out_path,
               const Options& meta) {
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
    const auto map = ac3::io::wav_channel_order(first.acmod, first.lfe);
    const auto written = write_wav_f32_arg(out_path, pcm, sample_rate_hz(first.sample_rate), map);
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
    const std::vector<std::vector<std::byte>> frames(static_cast<std::size_t>(count),
                                                     unit->bytes);
    if (!write_frames(out_path, frames)) {
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
                    std::println(stderr, "error: {}", ac3::sinks::describe(started.error()));
                    return 1;
                }
                std::println("monitoring {} ({} channels, {} Hz) on \"{}\"…", in_path,
                             order.size(), sample_rate_hz(out.sample_rate), device_name);
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
            std::string_view mode, const Options& meta) {
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
    const ac3::capture::DeviceInfo* device2 =
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

    ac3::capture::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        std::println(stderr, "error: {}", ac3::capture::describe(started.error()));
        return 1;
    }
    const auto channels = capture.channels();
    const auto rate_hz = capture.sample_rate();

    // Clock-master model: capture paces the session exactly as before -
    // nothing about its own timing changes below. capture2, when present, is
    // a second, independently-clocked device whose stream gets resampled
    // into lockstep with capture's pacing every frame, then appended after
    // capture's own channels.
    ac3::capture::Capture capture2;
    std::size_t capture2_channels = 0;
    std::optional<ac3::capture::DriftResampler> slave_resampler;
    std::optional<ac3::capture::ClockDriftEstimator> slave_drift;
    std::vector<float> slave_scratch;
    std::size_t slave_scratch_valid_frames = 0;
    std::vector<float> slave_out;
    if (device2) {
        const auto started2 = capture2.start(device2->id, device2->kind);
        if (!started2) {
            std::println(stderr, "error: {}", ac3::capture::describe(started2.error()));
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
enum class Needs : std::uint8_t { kNothing, kCapture, kPassthrough, kMonitor };

// The unmet requirement, or nullptr when the platform can satisfy it.
//
// Note what this is not: an OS test. main.cpp never asks whether it is on
// Windows - it asks the one translation unit CMake compiled from
// src/audio/src/platform/<os>/ what that platform can do, and prints the answer
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

constexpr std::array<Command, 25> kCommands{{
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
    {"decode", 3, "<in.ac3|in.ec3> <out.wav>", "AC-3 or E-AC-3; bsid decides", Needs::kNothing,
     [](const Args& x) { return run_decode(x.str(1), x.str(2), x.meta); }},
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
     [](const Args& x) { return run_monitor(x.str(1), x.i32(2, -1)); }},
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
    std::println("monitor/live --monitor play the 5.1 BED of an Atmos-mode stream: the in-repo");
    std::println("       decoder's E-AC-3 scope is A/52 Annex E syntax, not TS 103 420's object");
    std::println("       layer, so this is what a legacy decoder hears, not unmixed objects.");
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
                               token == "keep-partial" || token == "fast-mdct";
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
