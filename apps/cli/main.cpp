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
#include "commands/atmos.hpp"
#include "commands/audio_io.hpp"
#include "commands/containers.hpp"
#include "commands/decode.hpp"
#include "commands/encode.hpp"
#include "commands/live_audio.hpp"
#include "commands/synth.hpp"
#include "commands/truehd.hpp"
#include "multi_source.hpp"
#include "support.hpp"

namespace {

namespace plan = ac3::plan;

using namespace ac3cli;
using namespace ac3cli::commands;

void print_usage();


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
// Feeds each formed burst to `push` rather than accumulating them: the
// E-AC-3 carrier runs at 4x the content rate (~0.7 MB per second), which
// made the whole-payload form the largest O(duration) term the CLI had
// left. `rate_out` is set before the first push, so a caller may open its
// destination lazily from inside `push`. False means the stream is not a
// valid frame sequence - or that `push` itself said stop, which the
// caller can tell apart because it was its own push that failed.
template <typename Push>
bool wrap_ac3_stream(std::span<const std::byte> stream, std::uint32_t& rate_out, Push&& push) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        return false;
    }
    const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
    rate_out = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));

    for (const auto& frame : *frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        if (!burst) {
            return false;
        }
        if (!push(std::span<const std::byte>{*burst})) {
            return false;
        }
    }
    return true;
}

template <typename Push>
bool wrap_eac3_stream(std::span<const std::byte> stream, std::uint32_t& rate_out, Push&& push) {
    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        return false;
    }
    const auto byte4 = std::to_integer<std::uint32_t>((*units)[0][4]);
    rate_out = sample_rate_hz(static_cast<ac3::SampleRate>(byte4 >> 6));

    ac3::iec61937::Eac3BurstPacker packer;
    for (const auto& unit : *units) {
        const auto burst = packer.push(unit);
        if (!burst) {
            return false;
        }
        if (*burst && !push(std::span<const std::byte>{**burst})) {
            return false;
        }
    }
    return true;
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

    // The WAV carrier itself runs at 4x the content rate for E-AC-3 (Dolby
    // Digital Plus over IEC 60958/61937 - Microsoft's "Representing Formats
    // for IEC 61937 Transmissions"), matching WASAPI's make_eac3_format.
    // The sink opens lazily on the first burst - the rate is only known
    // once the wrapper has parsed the first frame, and a stream the
    // wrapper rejects must leave no file, exactly as the whole-payload
    // write it replaces never ran at all on failure.
    std::uint32_t content_rate = 0;
    Pcm16RawWavSink sink;
    bool sink_failed = false;
    const auto push = [&](std::span<const std::byte> burst) {
        if (!sink.is_open() &&
            !sink.open(out_path, eac3 ? content_rate * 4 : content_rate, 2)) {
            sink_failed = true;
            return false;
        }
        if (!sink.push(burst)) {
            sink_failed = true;
            return false;
        }
        return true;
    };
    const auto ok = eac3 ? wrap_eac3_stream(stream, content_rate, push)
                         : wrap_ac3_stream(stream, content_rate, push);
    if (!ok) {
        sink.abort();
        if (!sink_failed) {
            std::println(stderr, "error: {} is not a valid {} stream", in_path,
                         eac3 ? "E-AC-3" : "AC-3");
        }
        return 1;
    }
    const auto carrier_rate = eac3 ? content_rate * 4 : content_rate;
    // A valid stream whose units never completed a burst (an E-AC-3 input
    // shorter than one burst set) still produced a header-only WAV before,
    // so the never-opened sink opens for exactly that here.
    if (!sink.is_open() && !sink.open(out_path, carrier_rate, 2)) {
        return 1;
    }
    if (!sink.close()) {
        return 1;
    }
    std::println("wrapped {} into IEC 61937 bursts -> {} ({} Hz carrier)",
                 eac3 ? "E-AC-3 access units" : "AC-3 frames", out_path, carrier_rate);
    std::println("play bit-exactly (100% volume, exclusive/passthrough output) to light up");
    std::println("a receiver's Dolby Digital{} indicator.", eac3 ? " Plus" : "");
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

// 29 commands, always - including atmos-adm, whether or not AC3FORGE_BUILD_ADM linked
// ac3adm::ac3adm/ac3::admbridge into this particular build (see Needs::kAdm/unmet() above and
// run_atmos_adm's own comment): a command this build cannot run is listed with Needs gating it,
// never sized out of the table entirely - the identical "listed, not hidden" treatment
// kCapture/kPassthrough/kMonitor commands already get (see print_usage()'s own comment below on
// why hiding would be a lie about a command that exists and would work elsewhere).
constexpr std::array<Command, 29> kCommands{{
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
    {"truehd-encode", 3, "<in.wav> <out.mlp>", "lossless MLP; 16/24-bit integer PCM in",
     Needs::kNothing, [](const Args& x) { return run_truehd_encode(x.str(1), x.str(2)); }},
    {"truehd-decode", 3, "<in.mlp> <out.wav>", "bit-exact PCM out", Needs::kNothing,
     [](const Args& x) { return run_truehd_decode(x.str(1), x.str(2)); }},
    {"truehd-atmos", 3, "<in.wav> <out.mlp> [objects] [paths.txt]",
     "every source channel a dynamic object as its own lossless channel; optional keyframe "
     "motion (same file format as atmos-path)",
     Needs::kNothing,
     [](const Args& x) { return run_truehd_atmos(x.str(1), x.str(2), x.u32(3, 0), x.str(4)); }},
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
        std::string line = std::format("  ac3cli {:<14}{}", c.name, c.spec);
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
                               token == "fast-mdct" || token == "fast-imdct";
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
