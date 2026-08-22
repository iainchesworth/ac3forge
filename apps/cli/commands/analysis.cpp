#include "analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/sinks/iec61937.hpp"

namespace ac3cli::commands {

namespace {

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

}  // namespace

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

}  // namespace ac3cli::commands
