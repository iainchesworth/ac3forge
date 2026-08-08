#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "ac3/analysis/levels.hpp"
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

    // ---- metering ---------------------------------------------------------
    // channelNames changes only when the layout does, so it — not the level
    // list — is what a Repeater should bind to: rebuilding six delegates
    // thirty times a second would throw away every animation mid-flight.
    Q_PROPERTY(QStringList channelNames READ channelNames NOTIFY layoutChanged)
    Q_PROPERTY(QString layoutName READ layoutName NOTIFY layoutChanged)
    Q_PROPERTY(bool hasLevels READ hasLevels NOTIFY layoutChanged)
    Q_PROPERTY(bool surround READ surround NOTIFY layoutChanged)
    Q_PROPERTY(QVariantList channelLevels READ channelLevels NOTIFY levelsChanged)
    Q_PROPERTY(QVariantMap soundfield READ soundfield NOTIFY levelsChanged)
    Q_PROPERTY(bool metering READ metering NOTIFY meteringChanged)
    Q_PROPERTY(double meterFloorDb READ meterFloorDb CONSTANT)

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

    [[nodiscard]] QStringList channelNames() const { return channel_names_; }
    [[nodiscard]] QString layoutName() const { return layout_name_; }
    [[nodiscard]] bool hasLevels() const { return !channel_names_.isEmpty(); }
    // Two or more full-bandwidth channels make a soundfield worth drawing;
    // mono, and no source at all, do not.
    [[nodiscard]] bool surround() const {
        return hasLevels() && ac3::fullbw_channel_count(acmod_) >= 2;
    }
    [[nodiscard]] QVariantList channelLevels() const { return channel_levels_; }
    [[nodiscard]] QVariantMap soundfield() const { return soundfield_; }
    [[nodiscard]] bool metering() const { return metering_; }
    [[nodiscard]] double meterFloorDb() const { return kMeterFloorDb; }

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
    // Where a level sits on the meter scale, for the QML that draws the
    // gridline labels. The bars themselves get their positions in
    // channelLevels; this exists so the ticks cannot disagree with them.
    Q_INVOKABLE [[nodiscard]] double meterFraction(double db) const {
        return ac3::analysis::meter_fraction(db, kMeterFloorDb);
    }

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
    void layoutChanged();
    void levelsChanged();
    void meteringChanged();
    void encodeFinished(bool ok, const QString& message);

private:
    struct Source;

    // The meters read -60 dBFS at the bottom: far enough down to show room
    // tone, close enough up that programme material uses most of the bar.
    static constexpr double kMeterFloorDb = -60.0;

    void setStatus(const QString& text);
    void setBusy(bool busy);
    void setProgress(double value);
    void setRecording(bool recording);
    void setMetering(bool metering);

    // Re-labels the meters and clears them to the floor. GUI thread only, and
    // always before a worker that will publish into them starts.
    void setLayout(ac3::Acmod acmod, bool lfe);
    void clearLayout();
    // Publishes one snapshot. Workers reach this through a queued call, so
    // the level state itself never crosses a thread boundary unguarded.
    void publishLevels(std::span<const ac3::analysis::ChannelLevel> levels);
    // The same, built from a meter's exact whole-run statistics rather than
    // its ballistics: what a finished encode or a freshly loaded file should
    // leave on the display.
    void publishSummary(const ac3::analysis::LevelMeter& meter);

    QString source_path_;
    QString source_info_;
    QString output_path_;
    QString status_ = QStringLiteral("Choose a WAV file, or record from a capture device.");
    bool source_ready_ = false;
    bool busy_ = false;
    bool recording_ = false;
    double progress_ = 0.0;
    double recorded_seconds_ = 0.0;
    int bitrate_kbps_ = 192;
    bool playing_ = false;
    QStringList capture_devices_;
    QStringList output_devices_;
    std::vector<ac3::capture::DeviceInfo> devices_;
    std::vector<ac3::sinks::RenderDeviceInfo> outputs_;

    ac3::Acmod acmod_ = ac3::Acmod::k2_0;
    bool lfe_ = false;
    bool metering_ = false;
    QStringList channel_names_;
    QString layout_name_;
    QVariantList channel_levels_;
    QVariantMap soundfield_;

    std::unique_ptr<Source> source_;
    std::unique_ptr<ac3::capture::Capture> capture_;
    std::atomic_bool cancel_requested_{false};
    std::atomic_bool stop_recording_{false};
};
