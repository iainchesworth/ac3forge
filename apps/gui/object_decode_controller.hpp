#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ac3::audio {
class MonitorSink;
}

// The raw measurement, in the units the library reports them - kept OUTSIDE
// ObjectDecodeController itself for the same reason qc_controller.hpp's own
// qc_detail::RawResult is: the decode helpers in object_decode_controller.cpp
// (an anonymous namespace mirroring QcController's own measure_eac3) can
// build one without needing member access, and presentation (QVariantList
// shaping) happens only in ObjectDecodeController::frames()/objects() itself.
namespace objdec_detail {

// One access unit's worth of dynamic-object positions, in
// oba::DecodedProgram::objects order - the same order object_audio is
// parallel to (see decoder.hpp's own comment on DecodedSubstream::
// object_audio). Only access units that actually carried OAMD ever produce
// one of these; a stream where the very first frame or two arrive before the
// container is fully assembled simply contributes no entry for them.
struct RawFrame {
    double time_s = 0.0;
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> z;
    std::vector<double> gain_db;
};

struct RawResult {
    QString codec_label = QString();
    QString layout_label = QString();
    std::uint32_t sample_rate_hz = 0;
    int dynamic_object_count = 0;
    bool has_lfe = false;
    bool dynamic_only = true;
    double duration_seconds = 0.0;
    std::vector<RawFrame> frames = {};
    // Concatenated per-object audio across every frame that carried both
    // object_metadata and a matching object_audio, parallel to `frames`'
    // own object indexing - audition playback only, never shown as a
    // QVariantList property.
    std::vector<std::vector<float>> object_audio = {};
};

}  // namespace objdec_detail

// The QObject facade for the decode-side half of the Atmos object gap: now
// that Eac3Decoder genuinely parses OAMD positions and reconstructs JOC
// object audio (see decoder.hpp's own DecodedSubstream::object_metadata/
// object_audio comments), this is the GUI surface that shows what it found -
// the read-only counterpart to the Objects tab's authoring room view
// (Main.qml), reusing that view's plan/elevation visual language for
// PLAYBACK of already-decoded metadata instead of editing a plan still to
// come.
//
// Deliberately its own controller rather than a QcController addition or an
// EncoderController one: QcController's whole shape is "decode, measure
// against BS.1770/dialnorm, report a verdict" (see its own header comment),
// and object positions/audio are neither a measurement nor a verdict, just a
// second, unrelated thing this project's own decoder happens to also be able
// to read out of the same kind of file. EncoderController is ruled out for
// the same "already-encoded file, no plan, no source" reason QcController
// itself is not a member of it.
class ObjectDecodeController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The file last handed to inspectFile(), even while busy decoding it or
    // after a failed attempt - so the dialog can always say what it is
    // reporting on.
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True only once a decode has actually completed and found object
    // metadata - false before the first attempt, after a failed one, and
    // for a plain stream that decoded fine but carried no OAMD at all.
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    // "E-AC-3 · 3 dynamic objects + LFE · 48000 Hz · 62 frame(s) · 1.98 s"
    Q_PROPERTY(QString summaryLine READ summaryLine NOTIFY resultChanged)
    // One entry per access unit that carried OAMD - {time, objects: [{x, y,
    // z, gainDb}]}, in oba::DecodedProgram::objects order. Empty until a
    // decode succeeds. Scrubbing/playback of the room view is driven from
    // QML by indexing into this list; there is no interpolation between
    // entries the way the authoring side's evaluateObjectPath() offers,
    // because this is literal decoded data, not an authored path.
    Q_PROPERTY(QVariantList frames READ frames NOTIFY resultChanged)
    // How many frames actually carried OAMD - frames.length, exposed
    // separately so QML's slider range binding doesn't re-walk the whole
    // list just to find it.
    Q_PROPERTY(int frameCount READ frameCount NOTIFY resultChanged)
    // Which dynamic object's decoded audio is currently being auditioned
    // through the monitor sink, -1 when none. Only one plays at a time -
    // see auditionObject()'s own comment.
    Q_PROPERTY(int auditioningIndex READ auditioningIndex NOTIFY auditionChanged)

   public:
    explicit ObjectDecodeController(QObject* parent = nullptr);
    ~ObjectDecodeController() override;

    [[nodiscard]] QString filePath() const { return file_path_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool hasResult() const { return result_.has_value(); }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString summaryLine() const;
    [[nodiscard]] QVariantList frames() const;
    [[nodiscard]] int frameCount() const;
    [[nodiscard]] int auditioningIndex() const { return auditioning_index_; }

    // Reads `url`, decodes it as E-AC-3 (AC-3's bsid <= 8 never carries OAMD
    // - Annex E only) and collects every access unit's object metadata/
    // audio - off the GUI thread via QtConcurrent, the same worker pattern
    // QcController::measureFile already uses. Refused while already busy.
    Q_INVOKABLE void inspectFile(const QUrl& url);

    // Plays dynamic object `index`'s decoded audio through an ordinary
    // shared-mode output (ac3::audio::MonitorSink), the same playback path
    // EncoderController's own motion-preview uses. Calling it again for the
    // object already playing stops it (a toggle); calling it for a
    // DIFFERENT object while one is already playing is ignored - one
    // audition at a time keeps this simple and matches what the dialog's
    // own UI offers (Stop appears only on the playing row).
    Q_INVOKABLE void auditionObject(int index);
    Q_INVOKABLE void stopAudition();

   signals:
    void filePathChanged();
    void busyChanged();
    void resultChanged();
    void auditionChanged();

   private:
    QString file_path_;
    bool busy_ = false;
    QString error_;
    std::optional<objdec_detail::RawResult> result_;

    std::unique_ptr<ac3::audio::MonitorSink> audition_sink_;
    std::atomic<bool> stop_audition_{false};
    int auditioning_index_ = -1;
};
