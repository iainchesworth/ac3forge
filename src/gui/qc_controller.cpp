#include "qc_controller.hpp"

#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/qc.hpp"

using qc_detail::RawProgramme;
using qc_detail::RawResult;

namespace {

QString to_qstring(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

// A human name for each preset - qc_preset_name() (the CLI's kebab-case
// token) is what programmes()'s "id" field reports instead, matched against
// QML's objectName-keying convention (see SegmentedControl.qml's own
// comment on why a stable per-value id matters for Qt Quick Test).
QString preset_display_name(ac3::meta::QcPresetId id) {
    switch (id) {
        case ac3::meta::QcPresetId::kEbuR128S2: return QStringLiteral("EBU R 128 s2");
        case ac3::meta::QcPresetId::kAtscA85: return QStringLiteral("ATSC A/85");
        case ac3::meta::QcPresetId::kNetflix: return QStringLiteral("Netflix");
    }
    return QString();
}

// The outcome of one measureFile() attempt, handed back from the worker
// thread to the GUI thread in one QMetaObject::invokeMethod call - see
// QcController::measureFile.
struct MeasureOutcome {
    QString error = QString();  // empty on success
    RawResult result = RawResult();
};

// AC-3 (bsid <= 8): mirrors apps/cli/main.cpp's measure_qc_ac3 exactly -
// same per-frame decode loop, same dual-mono split, feeding
// ac3::meta::LoudnessMeter instead of printing anything. Kept in step with
// the CLI's own reference implementation deliberately, per this change's own
// brief: run_qc is "exactly what to measure" and "how to read embedded
// metadata off a decoded stream".
std::optional<RawResult> measure_ac3(std::span<const std::byte> stream, QString& error) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        error = QStringLiteral("Not a valid AC-3 stream.");
        return std::nullopt;
    }
    ac3::FrameDecoder decoder;
    RawResult result;
    result.codec_label = QStringLiteral("AC-3");
    result.unit_label = QStringLiteral("frame(s)");
    result.unit_count = frames->size();

    bool have_first = false;
    bool dual_mono = false;
    std::optional<ac3::meta::LoudnessMeter> meter;      // whole programme
    std::optional<ac3::meta::LoudnessMeter> meter_ch1;  // dual mono only
    std::optional<ac3::meta::LoudnessMeter> meter_ch2;

    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            error = QStringLiteral("Decode failed: %1").arg(to_qstring(ac3::describe(decoded.error())));
            return std::nullopt;
        }
        if (!have_first) {
            have_first = true;
            dual_mono = decoded->acmod == ac3::Acmod::kDualMono;
            result.sample_rate_hz = sample_rate_hz(decoded->sample_rate);
            if (dual_mono) {
                result.layout_label = QStringLiteral("1+1 dual mono");
                meter_ch1.emplace(decoded->sample_rate, ac3::Acmod::k1_0, false);
                meter_ch2.emplace(decoded->sample_rate, ac3::Acmod::k1_0, false);
                result.programmes.push_back(RawProgramme{.label = QStringLiteral("Ch1"),
                                                          .dialnorm = decoded->dialnorm,
                                                          .compr = decoded->compr});
                result.programmes.push_back(
                    RawProgramme{.label = QStringLiteral("Ch2"),
                                .dialnorm = decoded->dialnorm2.value_or(31),
                                .compr = decoded->compr2});
            } else {
                result.layout_label = to_qstring(ac3::analysis::layout_name(decoded->acmod, decoded->lfe));
                meter.emplace(decoded->sample_rate, decoded->acmod, decoded->lfe);
                result.programmes.push_back(
                    RawProgramme{.dialnorm = decoded->dialnorm, .compr = decoded->compr});
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

// E-AC-3 (bsid 11-16): mirrors measure_qc_eac3 - measures the INDEPENDENT
// substream's own bed audio only, same scope reasoning that function's own
// comment gives (matching measured_dialnorm's pre-encode pass and, unlike a
// plain decode-and-play, never a dependent's channels).
std::optional<RawResult> measure_eac3(std::span<const std::byte> stream, QString& error) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        error = QStringLiteral("Not a valid E-AC-3 stream.");
        return std::nullopt;
    }
    ac3::Eac3Decoder decoder;
    RawResult result;
    result.codec_label = QStringLiteral("E-AC-3");
    result.unit_label = QStringLiteral("access unit(s)");

    bool have_first = false;
    bool dual_mono = false;
    std::optional<ac3::meta::LoudnessMeter> meter;
    std::optional<ac3::meta::LoudnessMeter> meter_ch1;
    std::optional<ac3::meta::LoudnessMeter> meter_ch2;

    const auto ingest = [&](const ac3::DecodedSubstream& sub) {
        if (sub.strmtyp == ac3::eac3::StreamType::kDependent) {
            return;
        }
        if (!have_first) {
            have_first = true;
            dual_mono = sub.acmod == ac3::Acmod::kDualMono;
            result.sample_rate_hz = sample_rate_hz(sub.sample_rate);
            if (dual_mono) {
                result.layout_label = QStringLiteral("1+1 dual mono");
                meter_ch1.emplace(sub.sample_rate, ac3::Acmod::k1_0, false);
                meter_ch2.emplace(sub.sample_rate, ac3::Acmod::k1_0, false);
                result.programmes.push_back(RawProgramme{
                    .label = QStringLiteral("Ch1"), .dialnorm = sub.dialnorm, .compr = sub.compr});
                result.programmes.push_back(
                    RawProgramme{.label = QStringLiteral("Ch2"),
                                .dialnorm = sub.dialnorm2.value_or(31),
                                .compr = sub.compr2});
            } else {
                result.layout_label = to_qstring(ac3::analysis::layout_name(sub.acmod, sub.lfe));
                meter.emplace(sub.sample_rate, sub.acmod, sub.lfe);
                result.programmes.push_back(
                    RawProgramme{.dialnorm = sub.dialnorm, .compr = sub.compr});
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
            error = QStringLiteral("Decode failed (code %1).")
                        .arg(static_cast<int>(decoded.error()));
            return std::nullopt;
        }
        if (decoded->has_value()) {
            ingest(**decoded);
        }
    }
    for (const auto& sub : decoder.flush()) {
        ingest(sub);
    }

    if (!have_first) {
        error = QStringLiteral("Stream carried no independent substream.");
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

// Reads the whole file, dispatches on bsid (same convention run_qc uses:
// bsid > 8 is E-AC-3) and measures it. Runs entirely on the calling thread -
// measureFile() below is what moves this off the GUI thread.
MeasureOutcome measure_file(const QString& path) {
    MeasureOutcome outcome;
    std::ifstream in{path.toStdString(), std::ios::binary};
    if (!in) {
        outcome.error = QStringLiteral("Could not open %1.").arg(path);
        return outcome;
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (raw.empty()) {
        outcome.error = QStringLiteral("%1 is empty.").arg(path);
        return outcome;
    }
    std::vector<std::byte> stream(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        stream[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }

    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        outcome.error = QStringLiteral("%1 is too short to hold a syncframe.").arg(path);
        return outcome;
    }

    QString error;
    auto measured = *bsid > 8 ? measure_eac3(stream, error) : measure_ac3(stream, error);
    if (!measured) {
        outcome.error = error;
        return outcome;
    }
    outcome.result = std::move(*measured);
    return outcome;
}

}  // namespace

QcController::QcController(QObject* parent) : QObject(parent) {}

QString QcController::summaryLine() const {
    if (!result_) {
        return QString();
    }
    return QStringLiteral("%1 · %2 · %3 Hz · %4 %5 · %6 s")
        .arg(result_->codec_label, result_->layout_label)
        .arg(result_->sample_rate_hz)
        .arg(result_->unit_count)
        .arg(result_->unit_label)
        .arg(result_->seconds, 0, 'f', 2);
}

QStringList QcController::presetNames() const {
    QStringList names{QStringLiteral("All presets")};
    for (const auto id : ac3::meta::kQcPresetIds) {
        names.append(preset_display_name(id));
    }
    return names;
}

void QcController::setPresetIndex(int index) {
    const int clamped = std::clamp(index, 0, static_cast<int>(ac3::meta::kQcPresetIds.size()));
    if (clamped == preset_index_) {
        return;
    }
    preset_index_ = clamped;
    emit presetChanged();
    emit resultChanged();  // programmes()'s preset filtering depends on this
}

QVariantList QcController::programmes() const {
    QVariantList out;
    if (!result_) {
        return out;
    }
    for (const auto& p : result_->programmes) {
        QVariantMap row;
        row[QStringLiteral("label")] = p.label;
        const bool has_loudness = p.integrated_lkfs.has_value();
        row[QStringLiteral("hasLoudness")] = has_loudness;
        row[QStringLiteral("integratedLkfs")] = p.integrated_lkfs.value_or(0.0);
        const bool has_lra = p.lra_lu.has_value();
        row[QStringLiteral("hasLra")] = has_lra;
        row[QStringLiteral("lra")] = p.lra_lu.value_or(0.0);
        const bool has_true_peak = p.true_peak_dbtp.has_value();
        row[QStringLiteral("hasTruePeak")] = has_true_peak;
        row[QStringLiteral("truePeakDbtp")] = p.true_peak_dbtp.value_or(0.0);
        row[QStringLiteral("dialnorm")] = p.dialnorm;
        const double claimed_lkfs = -static_cast<double>(p.dialnorm);
        row[QStringLiteral("claimedLkfs")] = claimed_lkfs;
        row[QStringLiteral("hasCompr")] = p.compr.has_value();
        row[QStringLiteral("comprDb")] =
            p.compr ? ac3::meta::to_db(ac3::meta::compr_gain(*p.compr)) : 0.0;
        if (has_loudness) {
            const int implied = ac3::meta::dialnorm_from_lkfs(*p.integrated_lkfs);
            row[QStringLiteral("deltaDb")] = *p.integrated_lkfs - claimed_lkfs;
            row[QStringLiteral("impliedDialnorm")] = implied;
            row[QStringLiteral("dialnormMatches")] = implied == p.dialnorm;
        } else {
            row[QStringLiteral("deltaDb")] = 0.0;
            row[QStringLiteral("impliedDialnorm")] = p.dialnorm;
            row[QStringLiteral("dialnormMatches")] = false;
        }

        QVariantList presets;
        const auto add_preset = [&](ac3::meta::QcPresetId id) {
            const auto preset = ac3::meta::qc_preset(id);
            const auto verdict =
                ac3::meta::evaluate_qc_gate(preset, p.integrated_lkfs, p.true_peak_dbtp);
            QVariantMap preset_row;
            preset_row[QStringLiteral("id")] = to_qstring(ac3::meta::qc_preset_name(id));
            preset_row[QStringLiteral("name")] = preset_display_name(id);
            preset_row[QStringLiteral("targetLkfs")] = preset.target_lkfs;
            preset_row[QStringLiteral("toleranceLu")] = preset.tolerance_lu;
            preset_row[QStringLiteral("maxTruePeakDbtp")] = preset.max_true_peak_dbtp;
            preset_row[QStringLiteral("loudnessDelta")] = verdict.loudness_delta_lu.value_or(0.0);
            preset_row[QStringLiteral("loudnessPass")] = verdict.loudness_pass;
            preset_row[QStringLiteral("truePeakMargin")] =
                verdict.true_peak_margin_dbtp.value_or(0.0);
            preset_row[QStringLiteral("truePeakPass")] = verdict.true_peak_pass;
            preset_row[QStringLiteral("pass")] = verdict.pass();
            presets.append(preset_row);
        };
        if (preset_index_ == 0) {
            for (const auto id : ac3::meta::kQcPresetIds) {
                add_preset(id);
            }
        } else {
            add_preset(ac3::meta::kQcPresetIds[static_cast<std::size_t>(preset_index_ - 1)]);
        }
        row[QStringLiteral("presets")] = presets;

        out.append(row);
    }
    return out;
}

void QcController::measureFile(const QUrl& url) {
    if (busy_) {
        return;
    }
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    file_path_ = path;
    emit filePathChanged();
    busy_ = true;
    emit busyChanged();

    std::ignore = QtConcurrent::run([this, path] {
        auto outcome = measure_file(path);
        QMetaObject::invokeMethod(this, [this, outcome = std::move(outcome)]() mutable {
            busy_ = false;
            error_ = outcome.error;
            result_ = outcome.error.isEmpty() ? std::make_optional(std::move(outcome.result))
                                              : std::nullopt;
            emit busyChanged();
            emit resultChanged();
        });
    });
}
