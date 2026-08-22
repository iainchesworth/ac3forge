#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration>

#include <optional>

#include "ac3/io/wav.hpp"

// The QObject facade for the TrueHD (MLP) lossless path - the workbench's
// second codec FAMILY, not a third codecIndex: EncoderController's whole
// pipeline (ac3::plan, float samples, bitrate ladders, layouts, containers)
// is the perceptual codecs' shape, while TrueHD is integer-exact end to end
// and has no bitrate to choose. Wedging it into that controller would
// couple two things that share nothing below the WAV reader - the same
// "parallel sibling, thin dispatch only where genuinely needed" call the
// library itself made (ac3::mlp beside ac3::plan, docs/concepts/
// truehd-mlp.md), and the same its-own-controller reasoning QcController
// and ObjectDecodeController already document for their workflows.
//
// Two modes, one switch: a plain lossless encode (every source channel
// carried as-is), or Atmos objects (every source channel a dynamic object
// as its own discrete lossless channel, 16ch_channel_meaning() announcing
// the roles and OAMD-in-EMDF positions riding EXTRA_DATA(), via
// ac3::mlp::AtmosEncoder - the TrueHD counterpart of the E-AC-3 path's
// oba::AtmosEncoder). After writing the file the worker decodes it back
// and diffs against the source, so the result line's "verified bit-exact"
// is a measurement, not a claim.
class TruehdController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourceChanged)
    // "2 ch · 48,000 Hz · 24-bit · 4,813 samples" - or empty until a source
    // loads. Integer PCM16/PCM24 only: the float-normalized formats the
    // perceptual path accepts would forfeit the bit-exactness this whole
    // dialog exists to demonstrate.
    Q_PROPERTY(QString sourceInfo READ sourceInfo NOTIFY sourceChanged)
    Q_PROPERTY(bool sourceReady READ sourceReady NOTIFY sourceChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY outputChanged)
    // false = plain lossless carry; true = every channel a dynamic object.
    Q_PROPERTY(bool objectsMode READ objectsMode WRITE setObjectsMode NOTIFY modeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    // "121 access units · 20,892 bytes · 71.9% of PCM · peak 2,094 kbit/s ·
    // verified bit-exact"
    Q_PROPERTY(QString resultInfo READ resultInfo NOTIFY resultChanged)

   public:
    explicit TruehdController(QObject* parent = nullptr);
    ~TruehdController() override;

    [[nodiscard]] QString sourcePath() const { return source_path_; }
    [[nodiscard]] QString sourceInfo() const;
    [[nodiscard]] bool sourceReady() const { return source_.has_value(); }
    [[nodiscard]] QString outputPath() const { return output_path_; }
    [[nodiscard]] bool objectsMode() const { return objects_mode_; }
    void setObjectsMode(bool value);
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] bool hasResult() const { return !result_info_.isEmpty(); }
    [[nodiscard]] QString resultInfo() const { return result_info_; }

    // Reads and probes `url` off the GUI thread (integer PCM16/PCM24 WAV
    // only), keeping the sample words in memory for encode(). Suggests
    // <source>.mlp as the output path. Refused while busy.
    Q_INVOKABLE void setSource(const QUrl& url);
    Q_INVOKABLE void setOutput(const QUrl& url);

    // Encodes the loaded source to outputPath off the GUI thread - the
    // library's own two-pass peak_data_rate treatment, terminators carrying
    // the tail-fill count - then decodes the written bytes back and diffs
    // them against the source before reporting. Refused while busy or
    // without a source.
    Q_INVOKABLE void encode();

   signals:
    void sourceChanged();
    void outputChanged();
    void modeChanged();
    void busyChanged();
    void errorChanged();
    void resultChanged();

   private:
    void setError(const QString& message);

    QString source_path_;
    QString output_path_;
    std::optional<ac3::io::WavPcmData> source_;
    bool objects_mode_ = false;
    bool busy_ = false;
    QString error_;
    QString result_info_;
};
