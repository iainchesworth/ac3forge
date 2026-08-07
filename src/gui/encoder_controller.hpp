#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration>

#include <atomic>
#include <memory>
#include <vector>

#include "ac3/capture/capture.hpp"
#include "ac3/sinks/passthrough.hpp"

// The QObject facade the QML layer talks to. All codec and capture work
// happens in ac3::forge; this type owns nothing but the presentation state
// and the workers that keep encoding off the GUI thread.

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
    Q_PROPERTY(bool captureSupported READ captureSupported NOTIFY captureDevicesChanged)
    Q_PROPERTY(QStringList outputDevices READ outputDevices NOTIFY outputDevicesChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY outputChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(double recordedSeconds READ recordedSeconds NOTIFY recordedSecondsChanged)
    Q_PROPERTY(double captureLevel READ captureLevel NOTIFY recordedSecondsChanged)

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
    [[nodiscard]] bool captureSupported() const { return !capture_devices_.isEmpty(); }
    [[nodiscard]] QStringList outputDevices() const { return output_devices_; }
    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] bool canPlay() const { return !output_path_.isEmpty(); }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] double recordedSeconds() const { return recorded_seconds_; }
    [[nodiscard]] double captureLevel() const { return capture_level_; }

    void setBitrateKbps(int kbps);

    Q_INVOKABLE void loadSourceFile(const QUrl& url);
    Q_INVOKABLE void encodeTo(const QUrl& url);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE [[nodiscard]] QString suggestedOutputName() const;
    Q_INVOKABLE void refreshCaptureDevices();
    Q_INVOKABLE void startRecording(int deviceIndex, const QUrl& url);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void refreshOutputDevices();
    Q_INVOKABLE void playToReceiver(int deviceIndex);

signals:
    void sourceChanged();
    void outputChanged();
    void statusChanged();
    void busyChanged();
    void progressChanged();
    void bitrateChanged();
    void captureDevicesChanged();
    void outputDevicesChanged();
    void playingChanged();
    void recordingChanged();
    void recordedSecondsChanged();
    void encodeFinished(bool ok, const QString& message);

private:
    struct Source;

    void setStatus(const QString& text);
    void setBusy(bool busy);
    void setProgress(double value);
    void setRecording(bool recording);

    QString source_path_;
    QString source_info_;
    QString output_path_;
    QString status_ = QStringLiteral("Choose a WAV file, or record from a capture device.");
    bool source_ready_ = false;
    bool busy_ = false;
    bool recording_ = false;
    double progress_ = 0.0;
    double recorded_seconds_ = 0.0;
    double capture_level_ = 0.0;
    int bitrate_kbps_ = 192;
    bool playing_ = false;
    QStringList capture_devices_;
    QStringList output_devices_;
    std::vector<ac3::capture::DeviceInfo> devices_;
    std::vector<ac3::sinks::RenderDeviceInfo> outputs_;

    std::unique_ptr<Source> source_;
    std::unique_ptr<ac3::capture::Capture> capture_;
    std::atomic_bool cancel_requested_{false};
    std::atomic_bool stop_recording_{false};
};
