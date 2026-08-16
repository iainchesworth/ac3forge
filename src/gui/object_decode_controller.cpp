#include "object_decode_controller.hpp"

#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/sinks/monitor.hpp"

using objdec_detail::RawFrame;
using objdec_detail::RawResult;

namespace {

QString to_qstring(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

// The outcome of one inspectFile() attempt, handed back from the worker
// thread to the GUI thread in one QMetaObject::invokeMethod call - mirrors
// qc_controller.cpp's own MeasureOutcome.
struct InspectOutcome {
    QString error = QString();  // empty on success
    RawResult result = RawResult();
};

// Every access unit's own independent substream, whether or not it carried
// OAMD - object_metadata is std::nullopt for any frame arriving before the
// EMDF container is fully assembled or for a plain (non-Atmos) stream, and
// `ingest` simply contributes no RawFrame for those, exactly the way
// qc_controller.cpp's own dual-mono split contributes to only the
// programme(s) actually present.
std::optional<RawResult> measure_eac3_objects(std::span<const std::byte> stream, QString& error) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        error = QStringLiteral("Not a valid E-AC-3 stream.");
        return std::nullopt;
    }
    ac3::Eac3Decoder decoder;
    RawResult result;
    result.codec_label = QStringLiteral("E-AC-3");

    bool have_first = false;
    double time_s = 0.0;

    const auto ingest = [&](const ac3::DecodedSubstream& sub) {
        if (sub.strmtyp == ac3::eac3::StreamType::kDependent) {
            return;  // object audio only ever rides in the independent bed
        }
        if (!have_first) {
            have_first = true;
            result.sample_rate_hz = sample_rate_hz(sub.sample_rate);
        }
        if (result.sample_rate_hz > 0) {
            time_s += static_cast<double>(ac3::kSamplesPerFrame) /
                      static_cast<double>(result.sample_rate_hz);
        }
        if (!sub.object_metadata) {
            return;
        }
        const auto& program = sub.object_metadata->program;
        const auto& objects = sub.object_metadata->objects;

        RawFrame f;
        f.time_s = time_s;
        f.x.reserve(objects.size());
        f.y.reserve(objects.size());
        f.z.reserve(objects.size());
        f.gain_db.reserve(objects.size());
        for (const auto& object : objects) {
            f.x.push_back(object.position.x);
            f.y.push_back(object.position.y);
            f.z.push_back(object.position.z);
            f.gain_db.push_back(object.gain_db);
        }
        result.frames.push_back(std::move(f));

        result.dynamic_object_count = static_cast<int>(objects.size());
        result.dynamic_only = program.dynamic_only;
        result.has_lfe = ac3::oba::has_lfe(program);

        if (result.object_audio.size() != objects.size()) {
            result.object_audio.assign(objects.size(), {});
        }
        if (sub.object_audio.size() == objects.size()) {
            for (std::size_t i = 0; i < objects.size(); ++i) {
                auto& dst = result.object_audio[i];
                dst.insert(dst.end(), sub.object_audio[i].begin(), sub.object_audio[i].end());
            }
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
    if (result.frames.empty()) {
        error = QStringLiteral(
            "No Dolby Atmos object metadata (OAMD) found — this is a plain E-AC-3 stream.");
        return std::nullopt;
    }

    if (result.dynamic_only) {
        result.layout_label = result.has_lfe
                                   ? QStringLiteral("%1 dynamic object(s) + LFE")
                                         .arg(result.dynamic_object_count)
                                   : QStringLiteral("%1 dynamic object(s)")
                                         .arg(result.dynamic_object_count);
    } else {
        result.layout_label =
            QStringLiteral("bed + %1 dynamic object(s)").arg(result.dynamic_object_count);
    }
    result.duration_seconds = time_s;
    return result;
}

// Reads the whole file, dispatches on bsid (AC-3's bsid <= 8 never carries
// OAMD - it is an Annex E / E-AC-3-only tool) and decodes it. Runs entirely
// on the calling thread - inspectFile() below is what moves this off the GUI
// thread, mirroring qc_controller.cpp's own measure_file().
InspectOutcome inspect_file(const QString& path) {
    InspectOutcome outcome;
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
    if (*bsid <= 8) {
        outcome.error = QStringLiteral(
            "AC-3 (bsid %1) carries no object metadata — Dolby Atmos rides only in E-AC-3.")
                             .arg(*bsid);
        return outcome;
    }

    QString error;
    auto measured = measure_eac3_objects(stream, error);
    if (!measured) {
        outcome.error = error;
        return outcome;
    }
    outcome.result = std::move(*measured);
    return outcome;
}

}  // namespace

ObjectDecodeController::ObjectDecodeController(QObject* parent) : QObject(parent) {}

// Out-of-line even though it is just = default: audition_sink_ is a
// unique_ptr<ac3::sinks::MonitorSink>, and MonitorSink is only
// forward-declared in the header (see its own comment on why) - the
// destructor needs the complete type, which this translation unit's
// #include "ac3/sinks/monitor.hpp" above provides. Same shape as
// EncoderController's own out-of-line ~EncoderController() = default for its
// analogous motion_preview_monitor_sink_.
ObjectDecodeController::~ObjectDecodeController() = default;

QString ObjectDecodeController::summaryLine() const {
    if (!result_) {
        return QString();
    }
    return QStringLiteral("%1 · %2 · %3 Hz · %4 frame(s) · %5 s")
        .arg(result_->codec_label, result_->layout_label)
        .arg(result_->sample_rate_hz)
        .arg(result_->frames.size())
        .arg(result_->duration_seconds, 0, 'f', 2);
}

QVariantList ObjectDecodeController::frames() const {
    QVariantList out;
    if (!result_) {
        return out;
    }
    out.reserve(static_cast<qsizetype>(result_->frames.size()));
    for (const auto& f : result_->frames) {
        QVariantList objects;
        objects.reserve(static_cast<qsizetype>(f.x.size()));
        for (std::size_t i = 0; i < f.x.size(); ++i) {
            QVariantMap obj;
            obj[QStringLiteral("x")] = f.x[i];
            obj[QStringLiteral("y")] = f.y[i];
            obj[QStringLiteral("z")] = f.z[i];
            obj[QStringLiteral("gainDb")] = f.gain_db[i];
            objects.append(obj);
        }
        QVariantMap row;
        row[QStringLiteral("time")] = f.time_s;
        row[QStringLiteral("objects")] = objects;
        out.append(row);
    }
    return out;
}

int ObjectDecodeController::frameCount() const {
    return result_ ? static_cast<int>(result_->frames.size()) : 0;
}

void ObjectDecodeController::inspectFile(const QUrl& url) {
    if (busy_) {
        return;
    }
    stopAudition();
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    file_path_ = path;
    emit filePathChanged();
    busy_ = true;
    emit busyChanged();

    std::ignore = QtConcurrent::run([this, path] {
        auto outcome = inspect_file(path);
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

void ObjectDecodeController::auditionObject(int index) {
    if (index == auditioning_index_) {
        stopAudition();
        return;
    }
    if (auditioning_index_ != -1 || busy_ || !result_) {
        return;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= result_->object_audio.size()) {
        return;
    }
    const auto& samples = result_->object_audio[static_cast<std::size_t>(index)];
    if (samples.empty()) {
        return;
    }

    audition_sink_ = std::make_unique<ac3::sinks::MonitorSink>();
    const auto started =
        audition_sink_->start(std::string{}, result_->sample_rate_hz, /*channels=*/1);
    if (!started) {
        const auto why = ac3::sinks::describe(started.error());
        audition_sink_.reset();
        error_ = QStringLiteral("Could not open the audition output: %1").arg(to_qstring(why));
        emit resultChanged();
        return;
    }

    stop_audition_.store(false, std::memory_order_relaxed);
    auditioning_index_ = index;
    emit auditionChanged();

    std::ignore = QtConcurrent::run([this, samples] {
        std::size_t at = 0;
        while (at < samples.size()) {
            if (stop_audition_.load(std::memory_order_relaxed)) {
                break;
            }
            const auto chunk_len = std::min<std::size_t>(2048, samples.size() - at);
            const auto chunk = std::span{samples}.subspan(at, chunk_len);
            // submit()'s own non-blocking contract (see monitor.hpp): a full
            // queue means this is running ahead of real time, so wait
            // rather than spin - identical pacing to EncoderController's
            // own motion-preview submit loop.
            if (audition_sink_->submit(chunk)) {
                at += chunk_len;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
        QMetaObject::invokeMethod(this, [this] {
            if (audition_sink_) {
                audition_sink_->stop();
                audition_sink_.reset();
            }
            auditioning_index_ = -1;
            emit auditionChanged();
        });
    });
}

void ObjectDecodeController::stopAudition() {
    stop_audition_.store(true, std::memory_order_relaxed);
}
