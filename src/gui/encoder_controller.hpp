#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration>

#include <atomic>
#include <memory>

// The QObject facade the QML layer talks to. All codec work happens in
// ac3::forge; this type owns nothing but the presentation state and the
// worker that drives an encode off the GUI thread.

class EncoderController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceInfo READ sourceInfo NOTIFY sourceChanged)
    Q_PROPERTY(bool sourceReady READ sourceReady NOTIFY sourceChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY outputChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY bitrateChanged)
    Q_PROPERTY(QStringList captureDevices READ captureDevices NOTIFY captureDevicesChanged)
    Q_PROPERTY(bool captureSupported READ captureSupported CONSTANT)

public:
    explicit EncoderController(QObject* parent = nullptr);
    ~EncoderController() override;

    [[nodiscard]] QString sourcePath() const { return source_path_; }
    [[nodiscard]] QString sourceInfo() const { return source_info_; }
    [[nodiscard]] bool sourceReady() const { return source_ready_; }
    [[nodiscard]] QString outputPath() const { return output_path_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] double progress() const { return progress_; }
    [[nodiscard]] int bitrateKbps() const { return bitrate_kbps_; }
    [[nodiscard]] QStringList captureDevices() const { return capture_devices_; }
    [[nodiscard]] bool captureSupported() const;

    void setBitrateKbps(int kbps);

    Q_INVOKABLE void loadSourceFile(const QUrl& url);
    Q_INVOKABLE void encodeTo(const QUrl& url);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE [[nodiscard]] QString suggestedOutputName() const;
    Q_INVOKABLE void refreshCaptureDevices();

signals:
    void sourceChanged();
    void outputChanged();
    void statusChanged();
    void busyChanged();
    void progressChanged();
    void bitrateChanged();
    void captureDevicesChanged();
    void encodeFinished(bool ok, const QString& message);

private:
    struct Source;

    void setStatus(const QString& text);
    void setBusy(bool busy);
    void setProgress(double value);

    QString source_path_;
    QString source_info_;
    QString output_path_;
    QString status_ = QStringLiteral("Choose a WAV file to encode.");
    bool source_ready_ = false;
    bool busy_ = false;
    double progress_ = 0.0;
    int bitrate_kbps_ = 192;
    QStringList capture_devices_;

    std::unique_ptr<Source> source_;
    std::atomic_bool cancel_requested_{false};
};
