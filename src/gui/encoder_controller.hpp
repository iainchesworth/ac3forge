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
#include "ac3/core/tables.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/sinks/passthrough.hpp"

// The QObject facade the QML layer talks to. All codec and capture work
// happens in ac3::forge; this type owns nothing but the presentation state
// and the workers that keep encoding off the GUI thread.
//
// Every choice a user makes here ends up in one ac3::plan::Plan, which is the
// same value ac3cli builds from its command line. Nothing about layouts,
// coding tools or metadata is decided in this file - if it were, the two front
// ends could disagree about what "5.1.4" or "all" means and neither would be
// wrong on its own terms.

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
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY planChanged)
    Q_PROPERTY(QVariantList bitrates READ bitrates NOTIFY planChanged)
    Q_PROPERTY(QStringList captureDevices READ captureDevices NOTIFY captureDevicesChanged)
    Q_PROPERTY(bool captureSupported READ captureSupported NOTIFY captureDevicesChanged)
    Q_PROPERTY(QStringList outputDevices READ outputDevices NOTIFY outputDevicesChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY outputChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(double recordedSeconds READ recordedSeconds NOTIFY recordedSecondsChanged)

    // ---- format -----------------------------------------------------------
    // AC-3 (bsid 8) or E-AC-3 (bsid 16). The codec gates almost everything
    // else: AC-3 has no substream layer, so no layout wider than 5.1, and no
    // Annex E coding tools or mixmdate group.
    Q_PROPERTY(int codecIndex READ codecIndex WRITE setCodecIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList codecNames READ codecNames CONSTANT)
    Q_PROPERTY(int layoutIndex READ layoutIndex WRITE setLayoutIndex NOTIFY planChanged)
    // Only the layouts the current codec can carry, so an unreachable choice
    // is never offered rather than offered and then refused.
    Q_PROPERTY(QStringList layoutNames READ layoutNames NOTIFY planChanged)
    Q_PROPERTY(QString layoutDetail READ layoutDetail NOTIFY planChanged)
    Q_PROPERTY(int containerIndex READ containerIndex WRITE setContainerIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList containerNames READ containerNames CONSTANT)

    // ---- Annex E coding tools ---------------------------------------------
    Q_PROPERTY(bool toolsAvailable READ toolsAvailable NOTIFY planChanged)
    Q_PROPERTY(bool coupling READ coupling WRITE setCoupling NOTIFY planChanged)
    Q_PROPERTY(bool spx READ spx WRITE setSpx NOTIFY planChanged)
    Q_PROPERTY(bool aht READ aht WRITE setAht NOTIFY planChanged)
    // Band edges and the GAQ mode, -1 meaning "let the encoder choose from the
    // bit rate", which is the useful default for all three.
    Q_PROPERTY(int cplBegf READ cplBegf WRITE setCplBegf NOTIFY planChanged)
    Q_PROPERTY(int spxBegf READ spxBegf WRITE setSpxBegf NOTIFY planChanged)
    Q_PROPERTY(int gaqMode READ gaqMode WRITE setGaqMode NOTIFY planChanged)
    Q_PROPERTY(bool spxAtten READ spxAtten WRITE setSpxAtten NOTIFY planChanged)
    // The same selection written the way ac3cli takes it, so a setting found
    // here can be reproduced on the command line without translating it.
    Q_PROPERTY(QString toolsToken READ toolsToken NOTIFY planChanged)

    // ---- dynamic range, loudness and downmix metadata ---------------------
    Q_PROPERTY(int drcIndex READ drcIndex WRITE setDrcIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList drcNames READ drcNames CONSTANT)
    Q_PROPERTY(bool heavy READ heavy WRITE setHeavy NOTIFY planChanged)
    Q_PROPERTY(double ceilingDb READ ceilingDb WRITE setCeilingDb NOTIFY planChanged)
    Q_PROPERTY(double dialogueDb READ dialogueDb WRITE setDialogueDb NOTIFY planChanged)
    Q_PROPERTY(int dialnorm READ dialnorm WRITE setDialnorm NOTIFY planChanged)
    Q_PROPERTY(bool measureDialnorm READ measureDialnorm WRITE setMeasureDialnorm NOTIFY planChanged)
    Q_PROPERTY(int cmixIndex READ cmixIndex WRITE setCmixIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList cmixNames READ cmixNames CONSTANT)
    Q_PROPERTY(int surmixIndex READ surmixIndex WRITE setSurmixIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList surmixNames READ surmixNames CONSTANT)
    // The mixmdate group is E-AC-3 only: AC-3 carries its two coarse levels in
    // bsi and has nowhere to put the rest (§E2.3.1).
    Q_PROPERTY(bool mixmetaAvailable READ mixmetaAvailable NOTIFY planChanged)
    Q_PROPERTY(bool mixmeta READ mixmeta WRITE setMixmeta NOTIFY planChanged)
    Q_PROPERTY(int lfeMix READ lfeMix WRITE setLfeMix NOTIFY planChanged)
    Q_PROPERTY(int dmixIndex READ dmixIndex WRITE setDmixIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList dmixNames READ dmixNames CONSTANT)

    // ---- what the plan will actually do to this source --------------------
    // Answered before the encode rather than after: a layout the source cannot
    // fill leaves speakers silent, and that is worth knowing in advance.
    Q_PROPERTY(QString routingSummary READ routingSummary NOTIFY routingChanged)

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

    // ---- objects ----------------------------------------------------------
    // Object mode. Each source channel becomes an object placed around one
    // point in the room, encoded as a 5.1 E-AC-3 bed with JOC and OAMD beside
    // it (TS 103 420) rather than as channels.
    Q_PROPERTY(bool atmosEnabled READ atmosEnabled WRITE setAtmosEnabled NOTIFY planChanged)
    // Room-anchored per §4.2.1: x 0 at the left wall to 1 at the right, y 0 at
    // the front wall to 1 at the back, z -1 at the floor to +1 at the ceiling.
    Q_PROPERTY(double objectX READ objectX WRITE setObjectX NOTIFY objectsChanged)
    Q_PROPERTY(double objectY READ objectY WRITE setObjectY NOTIFY objectsChanged)
    Q_PROPERTY(double objectZ READ objectZ WRITE setObjectZ NOTIFY objectsChanged)
    // How far apart the source's channels are spread either side of that
    // point. Objects that reach the bed by the same route are exactly the ones
    // JOC cannot separate again, so this is what makes them recoverable.
    Q_PROPERTY(double objectSpread READ objectSpread WRITE setObjectSpread NOTIFY objectsChanged)
    // Objects never reach the LFE by panning — there is no direction that
    // points at it — so this send is the only route, and without it the bed's
    // LFE is silent however the objects are placed.
    Q_PROPERTY(double objectLfeSend READ objectLfeSend WRITE setObjectLfeSend NOTIFY objectsChanged)
    Q_PROPERTY(int objectCount READ objectCount NOTIFY sourceChanged)

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
    [[nodiscard]] QVariantList bitrates() const;
    [[nodiscard]] QStringList captureDevices() const { return capture_devices_; }
    [[nodiscard]] bool captureSupported() const { return !capture_devices_.isEmpty(); }
    [[nodiscard]] QStringList outputDevices() const { return output_devices_; }
    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] bool canPlay() const { return !output_path_.isEmpty(); }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] double recordedSeconds() const { return recorded_seconds_; }

    [[nodiscard]] int codecIndex() const { return static_cast<int>(codec_); }
    [[nodiscard]] QStringList codecNames() const;
    [[nodiscard]] int layoutIndex() const;
    [[nodiscard]] QStringList layoutNames() const;
    [[nodiscard]] QString layoutDetail() const;
    [[nodiscard]] int containerIndex() const { return container_index_; }
    [[nodiscard]] QStringList containerNames() const;

    [[nodiscard]] bool toolsAvailable() const {
        return codec_ == ac3::plan::Codec::kEac3 && !atmos_enabled_;
    }
    [[nodiscard]] bool coupling() const { return tools_.coupling; }
    [[nodiscard]] bool spx() const { return tools_.spx; }
    [[nodiscard]] bool aht() const { return tools_.aht; }
    [[nodiscard]] int cplBegf() const { return tools_.cplbegf; }
    [[nodiscard]] int spxBegf() const { return tools_.spxbegf; }
    [[nodiscard]] int gaqMode() const { return tools_.gaqmod; }
    [[nodiscard]] bool spxAtten() const { return tools_.spx_atten; }
    [[nodiscard]] QString toolsToken() const;

    [[nodiscard]] int drcIndex() const { return drc_index_; }
    [[nodiscard]] QStringList drcNames() const;
    [[nodiscard]] bool heavy() const { return meta_.heavy.has_value(); }
    [[nodiscard]] double ceilingDb() const { return ceiling_db_; }
    [[nodiscard]] double dialogueDb() const { return dialogue_db_; }
    [[nodiscard]] int dialnorm() const { return meta_.dialnorm; }
    [[nodiscard]] bool measureDialnorm() const { return meta_.measure_dialnorm; }
    [[nodiscard]] int cmixIndex() const { return static_cast<int>(meta_.cmixlev); }
    [[nodiscard]] QStringList cmixNames() const;
    [[nodiscard]] int surmixIndex() const { return static_cast<int>(meta_.surmixlev); }
    [[nodiscard]] QStringList surmixNames() const;
    [[nodiscard]] bool mixmetaAvailable() const { return codec_ == ac3::plan::Codec::kEac3; }
    [[nodiscard]] bool mixmeta() const { return meta_.mixmeta; }
    [[nodiscard]] int lfeMix() const { return meta_.lfemix.value_or(-1); }
    [[nodiscard]] int dmixIndex() const { return static_cast<int>(meta_.dmixmod); }
    [[nodiscard]] QStringList dmixNames() const;

    [[nodiscard]] QString routingSummary() const { return routing_summary_; }

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

    [[nodiscard]] bool atmosEnabled() const { return atmos_enabled_; }
    [[nodiscard]] double objectX() const { return object_x_; }
    [[nodiscard]] double objectY() const { return object_y_; }
    [[nodiscard]] double objectZ() const { return object_z_; }
    [[nodiscard]] double objectSpread() const { return object_spread_; }
    [[nodiscard]] double objectLfeSend() const { return object_lfe_send_; }
    [[nodiscard]] int objectCount() const { return object_count_; }

    void setBitrateKbps(int kbps);
    void setCodecIndex(int index);
    void setLayoutIndex(int index);
    void setContainerIndex(int index);
    void setCoupling(bool on);
    void setSpx(bool on);
    void setAht(bool on);
    void setCplBegf(int value);
    void setSpxBegf(int value);
    void setGaqMode(int value);
    void setSpxAtten(bool on);
    void setDrcIndex(int index);
    void setHeavy(bool on);
    void setCeilingDb(double db);
    void setDialogueDb(double db);
    void setDialnorm(int value);
    void setMeasureDialnorm(bool on);
    void setCmixIndex(int index);
    void setSurmixIndex(int index);
    void setMixmeta(bool on);
    void setLfeMix(int value);
    void setDmixIndex(int index);
    void setAtmosEnabled(bool enabled);
    void setObjectX(double value);
    void setObjectY(double value);
    void setObjectZ(double value);
    void setObjectSpread(double value);
    void setObjectLfeSend(double value);

    Q_INVOKABLE void loadSourceFile(const QUrl& url);
    Q_INVOKABLE void encodeTo(const QUrl& url);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE [[nodiscard]] QString suggestedOutputName() const;
    // The extension the current format and container imply, for the save
    // dialog. Derived rather than typed, so a .ac3 file can never hold E-AC-3.
    Q_INVOKABLE [[nodiscard]] QString outputSuffix() const;
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
    // One signal for every encoding decision. They are read together by the
    // summary lines and gate each other besides - the codec decides which
    // layouts exist, which decides whether the tools apply - so splitting them
    // would only invite a binding that misses the change it depended on.
    void planChanged();
    void objectsChanged();
    void routingChanged();
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

    // Everything the user has chosen, as the one value ac3cli also builds.
    [[nodiscard]] ac3::plan::Plan currentPlan() const;
    // Object mode always codes a 5.1 bed, so the layout it reports is that
    // bed rather than whatever the layout box last showed.
    [[nodiscard]] ac3::plan::LayoutId effectiveLayout() const;

    // Channels through the plan and out as AC-3 or E-AC-3. One worker for
    // both: they differ only in which encoder object runs, and everything
    // around it - routing, metering, progress, the container - is identical.
    void encodeChannels(const QString& path, std::vector<std::vector<float>> planes,
                        std::uint32_t sample_rate);
    // One object per source channel, over a 5.1 bed. `planes` is the source's
    // own channels; unlike the channel path they are not routed anywhere,
    // because an object is not a speaker feed.
    void encodeObjects(const QString& path, std::vector<std::vector<float>> planes,
                       std::uint32_t sample_rate);

    // Writes an elementary stream, or muxes Matroska, according to the chosen
    // container. Returns an empty string on success and the reason otherwise.
    [[nodiscard]] QString writeOutput(const QString& path,
                                      const std::vector<std::vector<std::byte>>& frames,
                                      std::uint32_t sample_rate, int channels) const;

    void setStatus(const QString& text);
    void setBusy(bool busy);
    void setProgress(double value);
    void setRecording(bool recording);
    void setMetering(bool metering);
    // Recomputes routingSummary and the meter labels from the current plan and
    // source. Called whenever either moves.
    void refreshRouting();

    // Re-labels the meters and clears them to the floor. GUI thread only, and
    // always before a worker that will publish into them starts. `names` may
    // be wider than the acmod, for a layout built from dependent substreams.
    //
    // `fed` says which of those channels the routing actually puts audio into.
    // A channel the source cannot fill reads -inf for a legitimate reason, and
    // that is a different thing from a meter wired to nothing; an empty vector
    // means every channel is fed.
    void setLayout(ac3::Acmod acmod, bool lfe, const QStringList& names, const QString& label,
                   const std::vector<bool>& fed = {});
    // Which coded channels the current plan feeds, sized to the layout.
    [[nodiscard]] std::vector<bool> fedChannels() const;
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
    QString routing_summary_;
    bool source_ready_ = false;
    bool busy_ = false;
    bool recording_ = false;
    double progress_ = 0.0;
    double recorded_seconds_ = 0.0;
    int bitrate_kbps_ = 192;

    ac3::plan::Codec codec_ = ac3::plan::Codec::kAc3;
    ac3::plan::LayoutId layout_ = ac3::plan::LayoutId::kStereo;
    ac3::plan::Tools tools_{};
    ac3::plan::Metadata meta_{};
    int container_index_ = 0;
    // Held apart from meta_.drc because the combo box's "none" entry has no
    // Profile to point at, and apart from meta_.heavy because the two level
    // fields survive the switch being turned off and on again.
    int drc_index_ = 0;
    double ceiling_db_ = -0.5;
    double dialogue_db_ = -20.0;

    bool atmos_enabled_ = false;
    // Straight ahead at ear height, which is where a stereo pair already is.
    double object_x_ = 0.5;
    double object_y_ = 0.0;
    double object_z_ = 0.0;
    double object_spread_ = 0.15;
    // Enough that the bed's LFE carries something without the low end of the
    // programme arriving twice.
    double object_lfe_send_ = 0.15;
    int object_count_ = 0;

    bool playing_ = false;
    QStringList capture_devices_;
    QStringList output_devices_;
    std::vector<ac3::capture::DeviceInfo> devices_;
    std::vector<ac3::sinks::RenderDeviceInfo> outputs_;

    ac3::Acmod acmod_ = ac3::Acmod::k2_0;
    bool lfe_ = false;
    bool metering_ = false;
    QStringList channel_names_;
    std::vector<bool> channel_fed_;
    QString layout_name_;
    QVariantList channel_levels_;
    QVariantMap soundfield_;

    std::unique_ptr<Source> source_;
    std::unique_ptr<ac3::capture::Capture> capture_;
    std::atomic_bool cancel_requested_{false};
    std::atomic_bool stop_recording_{false};
};
