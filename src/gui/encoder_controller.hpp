#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/capture/capture.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/sinks/monitor.hpp"
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
    // Multi-source input: the primary source (loadSourceFile) plus whatever
    // addSourceFile has added since, one row per loaded source -
    // {index, label, path, channels, primary}. Index 0 is always the primary
    // (removeSource(0) drops everything rather than promoting an extra,
    // since there is no honest way to guess which one should take its
    // place). With exactly one source loaded this is a single row and
    // nothing else here changes anything - see routingForSources().
    Q_PROPERTY(QVariantList sourceModel READ sourceModel NOTIFY sourceChanged)
    // One row per (source, channel) sourceModel declares -
    // {source, channel, sourceLabel, destToken} - destToken in
    // plan::parse_destination's own vocabulary ("L", "obj", "p1", "p2",
    // "none"), so setAssignment's argument is always something this list
    // itself already printed. Every row exists whether or not it has been
    // explicitly assigned yet (an unset one reads "none"), so a caller can
    // always render one row per channel rather than special-casing the gap.
    Q_PROPERTY(QVariantList assignmentRows READ assignmentRows NOTIFY sourceChanged)
    // "<source> ch <n> is loaded but goes nowhere" - plan::Assignment::
    // unassigned()'s inventory in prose. Empty only when automatic
    // single-source routing applies (see routingForSources) - every source
    // channel is accounted for by construction there; with more than one
    // source, every channel warns until it has actually been given a
    // destination, even before setAssignment has been called for the first
    // time.
    Q_PROPERTY(QStringList unassignedWarnings READ unassignedWarnings NOTIFY sourceChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY outputChanged)
    // Keep whatever frames a failed or cancelled run already produced,
    // written beside the intended output as <name>.partial.<ext> - partial
    // output is named and kept, not silently discarded (the handoff's error
    // state). Persisted as a preference by the GUI; on by default.
    Q_PROPERTY(bool keepPartialOutput READ keepPartialOutput WRITE setKeepPartialOutput
                   NOTIFY keepPartialOutputChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // Encoding is a job with a history, not a modal moment: one entry per
    // file encode (not a live recording, which already has its own elapsed-
    // time readout), newest first. Each is {id, filename, bitrateKbps,
    // rateText, durationText, status ("encoding"|"done"|"failed"|
    // "cancelled"), sizeText, detail}. rateText is what the run strip
    // actually displays - "384 kbps" for CBR, or, once a VBR run finishes,
    // "VBR q75 · avg 512 kbps (384-704)": a VBR run has no target rate to
    // show while "encoding" (only the quality it is aiming for), and a real
    // one to report once its actual frame sizes are known (see
    // encodeChannels' completion callback and finishRun()). There is at
    // most one "encoding" entry at a time (busy_ gates a new run), and its
    // live progress is read off the existing `progress` property rather
    // than duplicated per entry.
    Q_PROPERTY(QVariantList runs READ runs NOTIFY runsChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY planChanged)
    Q_PROPERTY(QVariantList bitrates READ bitrates NOTIFY planChanged)
    // ---- variable bit rate (E-AC-3, file output only) ---------------------
    // A quality target (with optional independent min/max kbps bounds)
    // replaces bitrate_kbps-driven CBR sizing - eac3-encode's own [vbr]
    // positional, in exactly plan::parse_vbr's grammar (kVbrSyntax), so the
    // command bar's line is always something ac3cli would actually parse.
    // Not available for AC-3 (validate() rejects it, PlanError::
    // kVbrNeedsEac3), object mode (a fixed 5.1 bed with no [vbr] argument of
    // its own), or a live session (IEC 61937 passthrough bursts are
    // fixed-size per access unit and nothing here renegotiates burst framing
    // mid-stream, so a live session always runs CBR regardless of what this
    // holds - see runLiveSession()). bitrate_kbps above still matters in VBR
    // mode: it keeps feeding the coupling/spx begin-frequency defaults, the
    // same job it always had, not a target rate.
    Q_PROPERTY(bool vbrAvailable READ vbrAvailable NOTIFY planChanged)
    Q_PROPERTY(bool vbrEnabled READ vbrEnabled WRITE setVbrEnabled NOTIFY planChanged)
    // 0-100 (default 75): linearly maps onto VbrConfig::quality's own [0,1]
    // range. A preference does not need two decimals of precision, so this
    // is an int rather than the raw double the library takes.
    Q_PROPERTY(int vbrQuality READ vbrQuality WRITE setVbrQuality NOTIFY planChanged)
    // Presence lives on the checkbox, never a sentinel value: unticked means
    // no bound at all, not a default one - matching VbrConfig::min_kbps/
    // max_kbps's own optional<> shape exactly rather than smuggling "off"
    // into some number nobody would ever legitimately choose.
    Q_PROPERTY(bool vbrMinEnabled READ vbrMinEnabled WRITE setVbrMinEnabled NOTIFY planChanged)
    Q_PROPERTY(int vbrMinKbps READ vbrMinKbps WRITE setVbrMinKbps NOTIFY planChanged)
    Q_PROPERTY(bool vbrMaxEnabled READ vbrMaxEnabled WRITE setVbrMaxEnabled NOTIFY planChanged)
    Q_PROPERTY(int vbrMaxKbps READ vbrMaxKbps WRITE setVbrMaxKbps NOTIFY planChanged)
    // plan::format_vbr() of the settings above - the exact [vbr] token
    // ac3cli's eac3-encode takes, so the command bar can paste it verbatim.
    Q_PROPERTY(QString vbrToken READ vbrToken NOTIFY planChanged)
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
    Q_PROPERTY(QString layoutDetail READ layoutDetail NOTIFY planChanged)
    Q_PROPERTY(int containerIndex READ containerIndex WRITE setContainerIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList containerNames READ containerNames CONSTANT)

    // ---- the channel model --------------------------------------------------
    // Tier 1: exactly one bed, always - one of Table 5.8's seven speaker
    // shapes - plus an independent LFE toggle. Tier 2: additive "extras"
    // pairs/singles on top. Replaces layoutNames() as a UI concept entirely;
    // every combination resolves through the same ac3::eac3::chanmap::allocate()
    // a hand-typed comma list already did, so the picker can never express
    // something the encoder would then refuse.
    Q_PROPERTY(int bedIndex READ bedIndex WRITE setBedIndex NOTIFY planChanged)
    // Seven rows {id, label, channels}, always all seven regardless of codec:
    // AC-3 disables only the extras, never the bed (plan::carries() already
    // offers AC-3 mono and stereo, and this must not remove that).
    Q_PROPERTY(QVariantList bedChoices READ bedChoices CONSTANT)
    Q_PROPERTY(bool bedLfe READ bedLfe WRITE setBedLfe NOTIFY planChanged)
    // 1+1 is a bed, not a layout (the handoff's own framing): two
    // independent programmes sharing one syncframe, not a spatial pair -
    // drawn first among the bed buttons and, unlike every other bed, with
    // nothing else able to sit alongside it. QML's hook for styling it and
    // the Programme 2 metadata block distinctly, rather than every reader
    // re-deriving "bedIndex 0" == dual mono for themselves.
    Q_PROPERTY(bool dualMono READ dualMono NOTIFY planChanged)
    // True for object mode (as extrasLocked already was) OR dual mono - an
    // independent LFE has no meaning once the bed is two mono programmes
    // instead of a soundfield, so selecting 1+1 clears and locks it exactly
    // as it locks the extras below.
    Q_PROPERTY(bool bedLfeLocked READ bedLfeLocked NOTIFY planChanged)
    // Five rows {id, label, channels, checked, enabled, reason}: `enabled` is
    // false when ticking (or, for an already-ticked row, UNticking) would
    // leave chanmap::allocate() unable to satisfy the result - over the
    // 16-channel ceiling (A/52 §E3.8.2), no Table 5.8 bed fits, or an LFE2
    // left with no full-bandwidth companion once its last co-selected extra
    // is removed. `reason` names which, or the lock reason, for the row to
    // print next to itself.
    Q_PROPERTY(QVariantList extrasModel READ extrasModel NOTIFY planChanged)
    // AC-3 has no dependent substreams at all (Table 5.8 tops out at 3/2 +
    // LFE), so it leaves every bed shape and the LFE toggle live and disables
    // only the extras - never the reverse. Object mode locks everything,
    // including the bed, at a fixed 5.1.
    Q_PROPERTY(bool extrasLocked READ extrasLocked NOTIFY planChanged)
    // "<ear-level count>.<LFE count>[.<ceiling count>]", read off the actual
    // location mask so an unnamed combination still reads honestly - 3/2 +
    // LFE + LFE2 is "5.2", 3/2 + LFE + rear + both ceiling pairs is "7.1.4".
    Q_PROPERTY(QString channelShapeName READ channelShapeName NOTIFY planChanged)
    Q_PROPERTY(int channelBudgetUsed READ channelBudgetUsed NOTIFY planChanged)
    Q_PROPERTY(int channelBudgetMax READ channelBudgetMax CONSTANT)
    // plan::format_channels() of the current bed+LFE+extras mask - the
    // comma-separated Table E2.5 list ac3cli's own [layout] argument takes,
    // so the command bar can generate a line that actually runs rather than
    // a friendly name ac3cli has no preset for.
    Q_PROPERTY(QString channelLocationsText READ channelLocationsText NOTIFY planChanged)

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
    // Programme 2's own dialnorm (§5.4.2.16) - meaningless outside dual mono,
    // where it exists because §5.4.2.8's dialnorm is program 1's and the two
    // never share a downmix to average across. Same shape as dialnorm/
    // measureDialnorm; QML gates visibility on dualMono rather than these
    // hiding themselves, matching every other metadata field here.
    Q_PROPERTY(int dialnorm2 READ dialnorm2 WRITE setDialnorm2 NOTIFY planChanged)
    Q_PROPERTY(bool measureDialnorm2 READ measureDialnorm2 WRITE setMeasureDialnorm2 NOTIFY planChanged)
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
    // The coded plan as a per-channel list - one entry per coded channel of
    // the CURRENT plan (not of whatever layout the meters happen to be
    // showing): {name, token, azimuthDeg, directional, ceiling, replaced,
    // fed}. This is what the Format tab's channel map, the soundfield's
    // solid/hollow dots and the "N of M positions fed" lines read, so they
    // all count the same fed set. Dual mono lists its two programmes;
    // object mode lists the 5.1 bed with fed answered by panning the
    // objects. Notifies on routingChanged because fed is a routing fact:
    // every bed/extras/assignment/source edit ends in refreshRouting().
    Q_PROPERTY(QVariantList plannedChannels READ plannedChannels NOTIFY routingChanged)

    // ---- metering ---------------------------------------------------------
    // channelNames changes only when the layout does, so it — not the level
    // list — is what a Repeater should bind to: rebuilding six delegates
    // thirty times a second would throw away every animation mid-flight.
    Q_PROPERTY(QStringList channelNames READ channelNames NOTIFY layoutChanged)
    // Everything about a meter row that does NOT move per tick - {name,
    // azimuthDeg, directional, ceiling, replaced, fed} - so the meter and
    // soundfield Repeaters have a model that only changes when the layout
    // does. The per-tick values (peak/rms/hold/clipped) stay in
    // channelLevels; a delegate reads its own entry by index. Binding a
    // Repeater's model to channelLevels instead tears every delegate down
    // ~30 times a second, which is exactly the jank this exists to prevent.
    Q_PROPERTY(QVariantList channelMeta READ channelMeta NOTIFY layoutChanged)
    Q_PROPERTY(QString layoutName READ layoutName NOTIFY layoutChanged)
    Q_PROPERTY(bool hasLevels READ hasLevels NOTIFY layoutChanged)
    Q_PROPERTY(bool surround READ surround NOTIFY layoutChanged)
    // Each entry also carries "ceiling" (a height-type location, for the
    // second soundfield ring) and "replaced" (a bed channel a dependent
    // substream supersedes - Coded mode groups it behind a rule; Rendered
    // mode hides it) alongside the existing peak/rms/hold/fed/directional
    // fields, so a Repeater filtering by meter mode never needs a second
    // array to look anything up in.
    Q_PROPERTY(QVariantList channelLevels READ channelLevels NOTIFY levelsChanged)
    Q_PROPERTY(QVariantMap soundfield READ soundfield NOTIFY levelsChanged)
    Q_PROPERTY(bool metering READ metering NOTIFY meteringChanged)
    Q_PROPERTY(double meterFloorDb READ meterFloorDb CONSTANT)

    // ---- objects ----------------------------------------------------------
    // Object mode. Each source channel becomes an object, encoded as a 5.1
    // E-AC-3 bed with JOC and OAMD beside it (TS 103 420) rather than as
    // channels. Placement is per object now, not one shared point plus a
    // spread fan-out (§6 Q5): spread was standing in for that and is retired.
    Q_PROPERTY(bool atmosEnabled READ atmosEnabled WRITE setAtmosEnabled NOTIFY planChanged)
    Q_PROPERTY(int objectCount READ objectCount NOTIFY sourceChanged)
    // Which object the room plan, the sliders and the timeline all edit.
    Q_PROPERTY(int selectedObjectIndex READ selectedObjectIndex WRITE setSelectedObjectIndex NOTIFY objectsChanged)
    // One row per object: {index, sourceLabel, x, y, z, lfeSend, hasPath,
    // keyCount} - room-anchored per §4.2.1 (x 0 at the left wall to 1 at the
    // right, y 0 at the front wall to 1 at the back, z -1 at the floor to +1
    // at the ceiling). Backs both the room plan's markers and the object
    // list table, so the two can never disagree about a position.
    Q_PROPERTY(QVariantList objectModel READ objectModel NOTIFY objectsChanged)

    // ---- live session -------------------------------------------------------
    // Capture, encode and (optionally) monitor+passthrough all running at
    // once, as opposed to startRecording (capture+encode+file only) or
    // playToReceiver (an already-encoded file's bytes, no live capture at
    // all). Distinct from `busy` even though it also sets busy_ - `busy`
    // gates every other operation the way it always has, and `liveActive`
    // is what the Live session tab itself needs to know.
    Q_PROPERTY(bool liveActive READ liveActive NOTIFY liveActiveChanged)
    Q_PROPERTY(bool liveMonitoring READ liveMonitoring NOTIFY liveActiveChanged)
    Q_PROPERTY(bool livePassthrough READ livePassthrough NOTIFY liveActiveChanged)
    Q_PROPERTY(bool liveWritingToDisk READ liveWritingToDisk NOTIFY liveActiveChanged)
    // What the receiver leg can actually carry, and whether that is less than
    // the main encode plan. Object mode is always a gap: TS 103 420's JOC
    // layer plays as its 5.1 bed on any real decoder we have tried, ours
    // included (see docs - the decoder's object gate is keyed, and forging
    // that key is deliberately not done), so a live Atmos session is never
    // heard as Atmos on the other end, independent of what the receiver
    // device itself supports.
    Q_PROPERTY(QString liveReceiverPlanText READ liveReceiverPlanText NOTIFY liveActiveChanged)
    Q_PROPERTY(bool liveGap READ liveGap NOTIFY liveActiveChanged)
    // Set for a couple of seconds right after the passthrough endpoint opens
    // - a real exclusive-mode stream open, which is exactly when a physical
    // receiver drops its lock and re-negotiates.
    Q_PROPERTY(bool liveReconnecting READ liveReconnecting NOTIFY liveReconnectingChanged)
    Q_PROPERTY(double liveRunningSeconds READ liveRunningSeconds NOTIFY liveStatsChanged)
    Q_PROPERTY(qint64 liveFramesEncoded READ liveFramesEncoded NOTIFY liveStatsChanged)
    Q_PROPERTY(qint64 liveFramesDropped READ liveFramesDropped NOTIFY liveStatsChanged)
    Q_PROPERTY(quint64 liveUnderruns READ liveUnderruns NOTIFY liveStatsChanged)
    // A computed lower bound (two frame periods: one to fill the capture
    // buffer, one to encode and hand off), not a measurement - neither sink
    // reports an end-to-end figure. Zero when nothing is running, since there
    // is nothing to estimate yet.
    Q_PROPERTY(double liveLatencyMs READ liveLatencyMs NOTIFY liveActiveChanged)

public:
    explicit EncoderController(QObject* parent = nullptr);
    ~EncoderController() override;

    [[nodiscard]] QString sourcePath() const { return source_path_; }
    [[nodiscard]] QString sourceInfo() const { return source_info_; }
    [[nodiscard]] bool sourceReady() const { return source_ready_; }
    [[nodiscard]] QVariantList sourceModel() const;
    [[nodiscard]] QVariantList assignmentRows() const;
    [[nodiscard]] QStringList unassignedWarnings() const;
    [[nodiscard]] QString outputPath() const { return output_path_; }
    [[nodiscard]] bool keepPartialOutput() const { return keep_partial_output_; }
    void setKeepPartialOutput(bool keep);
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] double progress() const { return progress_; }
    [[nodiscard]] QVariantList runs() const { return runs_; }
    [[nodiscard]] int bitrateKbps() const { return bitrate_kbps_; }
    [[nodiscard]] QVariantList bitrates() const;
    [[nodiscard]] bool vbrAvailable() const {
        return codec_ == ac3::plan::Codec::kEac3 && !atmos_enabled_ && !live_active_;
    }
    [[nodiscard]] bool vbrEnabled() const { return vbr_enabled_; }
    [[nodiscard]] int vbrQuality() const { return vbr_quality_; }
    [[nodiscard]] bool vbrMinEnabled() const { return vbr_min_enabled_; }
    [[nodiscard]] int vbrMinKbps() const { return static_cast<int>(vbr_min_kbps_); }
    [[nodiscard]] bool vbrMaxEnabled() const { return vbr_max_enabled_; }
    [[nodiscard]] int vbrMaxKbps() const { return static_cast<int>(vbr_max_kbps_); }
    [[nodiscard]] QString vbrToken() const;
    [[nodiscard]] QStringList captureDevices() const { return capture_devices_; }
    [[nodiscard]] bool captureSupported() const { return !capture_devices_.isEmpty(); }
    [[nodiscard]] QStringList outputDevices() const { return output_devices_; }
    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] bool canPlay() const { return !output_path_.isEmpty(); }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] double recordedSeconds() const { return recorded_seconds_; }

    [[nodiscard]] int codecIndex() const { return static_cast<int>(codec_); }
    [[nodiscard]] QStringList codecNames() const;
    [[nodiscard]] QString layoutDetail() const;
    [[nodiscard]] int containerIndex() const { return container_index_; }
    [[nodiscard]] QStringList containerNames() const;

    [[nodiscard]] int bedIndex() const;
    [[nodiscard]] QVariantList bedChoices() const;
    [[nodiscard]] bool bedLfe() const { return bed_lfe_; }
    [[nodiscard]] bool dualMono() const { return isDualMono(); }
    [[nodiscard]] bool bedLfeLocked() const { return atmos_enabled_ || isDualMono(); }
    [[nodiscard]] QVariantList extrasModel() const;
    // Object mode and dual mono lock the extras; plain AC-3 deliberately
    // does NOT - ticking an extra under AC-3 PROMOTES the codec to E-AC-3
    // (see toggleExtra), because extras must never be gated by a codec the
    // extras themselves change. That circularity was a real bug during
    // design and the handoff calls it out by name.
    [[nodiscard]] bool extrasLocked() const {
        return atmos_enabled_ || isDualMono();
    }
    [[nodiscard]] QString channelShapeName() const;
    [[nodiscard]] int channelBudgetUsed() const;
    [[nodiscard]] int channelBudgetMax() const { return 16; }
    [[nodiscard]] QString channelLocationsText() const;

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
    [[nodiscard]] int dialnorm2() const { return meta_.dialnorm2; }
    [[nodiscard]] bool measureDialnorm2() const { return meta_.measure_dialnorm2; }
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
    [[nodiscard]] QVariantList plannedChannels() const;

    [[nodiscard]] QStringList channelNames() const { return channel_names_; }
    [[nodiscard]] QVariantList channelMeta() const;
    [[nodiscard]] QString layoutName() const { return layout_name_; }
    [[nodiscard]] bool hasLevels() const { return !channel_names_.isEmpty(); }
    // Two or more full-bandwidth channels make a soundfield worth drawing;
    // mono, and no source at all, do not. Dual mono's Table 5.8 entry
    // reuses nfchans=2 (the same "not a layout" placeholder acmod_map's own
    // comment names), but Ch1/Ch2 are unrelated programmes with no
    // soundstage between them - fullbw_channel_count alone would say
    // otherwise, so this checks acmod_ directly rather than trust it here.
    [[nodiscard]] bool surround() const {
        return hasLevels() && acmod_ != ac3::Acmod::kDualMono &&
              ac3::fullbw_channel_count(acmod_) >= 2;
    }
    [[nodiscard]] QVariantList channelLevels() const { return channel_levels_; }
    [[nodiscard]] QVariantMap soundfield() const { return soundfield_; }
    [[nodiscard]] bool metering() const { return metering_; }
    [[nodiscard]] double meterFloorDb() const { return kMeterFloorDb; }

    [[nodiscard]] bool atmosEnabled() const { return atmos_enabled_; }
    [[nodiscard]] int objectCount() const { return object_count_; }
    [[nodiscard]] int selectedObjectIndex() const { return selected_object_index_; }
    [[nodiscard]] QVariantList objectModel() const;

    [[nodiscard]] bool liveActive() const { return live_active_; }
    [[nodiscard]] bool liveMonitoring() const { return live_monitoring_; }
    [[nodiscard]] bool livePassthrough() const { return live_passthrough_; }
    [[nodiscard]] bool liveWritingToDisk() const { return live_writing_to_disk_; }
    [[nodiscard]] QString liveReceiverPlanText() const { return live_receiver_plan_text_; }
    [[nodiscard]] bool liveGap() const { return live_gap_; }
    [[nodiscard]] bool liveReconnecting() const { return live_reconnecting_; }
    [[nodiscard]] double liveRunningSeconds() const { return live_running_seconds_; }
    [[nodiscard]] qint64 liveFramesEncoded() const { return live_frames_encoded_; }
    [[nodiscard]] qint64 liveFramesDropped() const { return live_frames_dropped_; }
    [[nodiscard]] quint64 liveUnderruns() const { return live_underruns_; }
    [[nodiscard]] double liveLatencyMs() const { return live_latency_ms_; }

    void setBitrateKbps(int kbps);
    void setVbrEnabled(bool on);
    void setVbrQuality(int value);
    void setVbrMinEnabled(bool on);
    void setVbrMinKbps(int value);
    void setVbrMaxEnabled(bool on);
    void setVbrMaxKbps(int value);
    void setCodecIndex(int index);
    void setBedIndex(int index);
    void setBedLfe(bool on);
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
    void setDialnorm2(int value);
    void setMeasureDialnorm2(bool on);
    void setCmixIndex(int index);
    void setSurmixIndex(int index);
    void setMixmeta(bool on);
    void setLfeMix(int value);
    void setDmixIndex(int index);
    void setAtmosEnabled(bool enabled);
    void setSelectedObjectIndex(int index);

    // Refused (silently, same as a bed button or LFE toggle) when locked or
    // when the result would leave chanmap::allocate() unable to satisfy it.
    Q_INVOKABLE void toggleExtra(const QString& id);
    // The Live session tab's layout switcher: stops the running session,
    // applies the named preset (applyChannelPreset's vocabulary) and starts
    // a new session with the same capture/monitor/receiver choices. A
    // deliberate, visible act - the stream stops, the receiver re-locks and
    // about a second of audio is lost, exactly as the handoff frames it.
    // Refused while nothing is live, while object mode fixes the layout, and
    // while the take is being written to disk (a restart would clobber the
    // first half of the file; stopping and starting a new take is honest).
    Q_INVOKABLE void switchLiveLayout(const QString& presetName);
    // Sets bed + LFE + extras together - "stereo", "5.1", "7.1", "5.1.4",
    // "7.1.4", "5.2" or "7.2.4" - the starting points the Format tab's
    // preset buttons offer.
    // Upgrades AC-3 to E-AC-3 first if the preset needs a dependent substream,
    // the same way a manual extras tick would otherwise be refused outright.
    Q_INVOKABLE void applyChannelPreset(const QString& name);
    // The minimal authoring hook for genuine per-object motion: an object
    // with authored keyframes here moves along them during encodeObjects
    // instead of sitting at its static position. Each entry of `keyframes`
    // is a map with "time", "x", "y", "z", "gain" and "lfeSend" (the latter
    // two optional). An empty list clears the object's path, returning it to
    // the static fallback.
    Q_INVOKABLE void setObjectPathKeyframes(int objectIndex, const QVariantList& keyframes);
    Q_INVOKABLE void clearObjectPath(int objectIndex);
    // The room plan's drag target and the object list's editable cells - the
    // static position a path-less object holds for the whole file, or that a
    // keyframe is captured from (see addObjectKeyframe).
    Q_INVOKABLE void setObjectPosition(int objectIndex, double x, double y, double z);
    Q_INVOKABLE void setObjectLfeSend(int objectIndex, double value);
    // Sorted by time, each {time, x, y, z, gain, lfeSend} - what the motion
    // timeline draws one lane of. Empty for an object with no authored path.
    Q_INVOKABLE [[nodiscard]] QVariantList objectKeyframes(int objectIndex) const;
    // Captures the object's CURRENT static position as a keyframe at time_s,
    // replacing one already there within 1/100s (float-equality has no
    // business deciding whether two cues are "the same moment"). The first
    // keyframe on a path-less object starts the path; setObjectPathKeyframes
    // is what actually holds it, so this and clearObjectPath are the only two
    // ways a path's contents change.
    Q_INVOKABLE void addObjectKeyframe(int objectIndex, double timeS);
    Q_INVOKABLE void removeObjectKeyframe(int objectIndex, double timeS);
    // Where an object sits at timeS: along its authored path if it has one,
    // else its static position, unmoving. What the motion timeline's preview
    // playhead reads so the room plan animates exactly what encodeObjects()
    // will actually place - the same ac3::oba::KeyframePath, not a second
    // interpolation that could disagree with it.
    Q_INVOKABLE [[nodiscard]] QVariantMap evaluateObjectPath(int objectIndex, double timeS) const;

    Q_INVOKABLE void loadSourceFile(const QUrl& url);
    // The first-run screen's third path in: synthesises an eight-second 5.1
    // test signal (a distinct tone per channel, WAV speaker order) into the
    // temp directory and loads it like any other file, so a user with no
    // multichannel WAV to hand still gets a working session to explore.
    Q_INVOKABLE void loadBundledTestSignal();
    // Adds another source alongside whatever is already loaded - or, if
    // nothing is loaded yet, is exactly loadSourceFile (so a caller offering
    // one "add a source" affordance never has to know which entry point to
    // use first). Refuses (with a status message, the same convention
    // loadSourceFile's own failures use) a sample rate that does not match
    // the sources already loaded - plan::render has no notion of
    // resampling, and a silent mismatch would drift rather than error.
    Q_INVOKABLE void addSourceFile(const QUrl& url);
    // index 0 (the primary) drops every loaded source and the assignment
    // with it - see sourceModel's own doc comment on why. Removing any
    // other index clears the assignment table rather than trying to shift
    // its rows down: they addressed positions by index, every later
    // source's index just changed, and guessing which old row survives is
    // exactly the kind of silently-maybe-wrong behaviour this surface
    // exists to avoid (plan::Assignment's own doc comment makes the same
    // call for an unassigned channel).
    Q_INVOKABLE void removeSource(int index);
    // destToken is whatever assignmentRows already printed for a row, or
    // any token plan::parse_destination accepts - the same vocabulary
    // ac3cli's map= takes, so a GUI selection and a hand-typed command line
    // can never disagree about what a token means. Silently ignored if it
    // does not parse, same convention as toggleExtra/applyChannelPreset.
    Q_INVOKABLE void setAssignment(int sourceIndex, int channel, const QString& destToken);
    // Fills every still-unassigned channel whose SOURCE has a natural AC-3
    // layout (mono, stereo, 5.1, ...) with the bed position that channel
    // holds in that layout - "assign by name": a 5.1 file's third WAV
    // channel is its centre, so it goes to C. Positions the current plan
    // does not carry are left unassigned (and keep their warning) rather
    // than silently invented; rows already assigned - or deliberately set
    // to nothing - are never overwritten.
    Q_INVOKABLE void autoAssignByName();
    // Back to automatic routing - only meaningful with exactly one source
    // loaded (see routingForSources); with more than one, clearing merely
    // empties the table, since automatic panning has no defined meaning
    // across several sources.
    Q_INVOKABLE void clearAssignment();
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
    // Starts a continuous capture -> encode session: unlike startRecording,
    // frames never wait for a stop to reach a sink - each is optionally
    // handed to a MonitorSink (decoded back for an honest preview of what
    // was just encoded, not the raw input) and a PassthroughSink (bitstreamed
    // to the receiver) as it is produced, and only optionally also
    // accumulated for a `writeToDisk` file at the end. `receiverDeviceIndex`
    // indexes outputDevices(); -1 means no passthrough this run. Refused
    // (silently, same convention as every other start-a-thing entry point)
    // while anything else is busy.
    Q_INVOKABLE void startLiveSession(int captureDeviceIndex, bool monitor,
                                      int receiverDeviceIndex, bool writeToDisk,
                                      const QUrl& fileUrl);
    Q_INVOKABLE void stopLiveSession();
    // The reconnection banner's Skip: stop announcing the receiver's
    // re-lock window early. Purely presentational - the pulse is advisory,
    // and a user who can hear the receiver has settled knows better than
    // the timer does.
    Q_INVOKABLE void settleReconnect();
    // Where a level sits on the meter scale, for the QML that draws the
    // gridline labels. The bars themselves get their positions in
    // channelLevels; this exists so the ticks cannot disagree with them.
    Q_INVOKABLE [[nodiscard]] double meterFraction(double db) const {
        return ac3::analysis::meter_fraction(db, kMeterFloorDb);
    }

signals:
    void sourceChanged();
    void outputChanged();
    void keepPartialOutputChanged();
    void statusChanged();
    void busyChanged();
    void progressChanged();
    void runsChanged();
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
    void liveActiveChanged();
    void liveStatsChanged();
    void liveReconnectingChanged();

private:
    struct Source;

    // The meters read -60 dBFS at the bottom: far enough down to show room
    // tone, close enough up that programme material uses most of the bar.
    static constexpr double kMeterFloorDb = -60.0;

    // Everything the user has chosen, as the one value ac3cli also builds.
    [[nodiscard]] ac3::plan::Plan currentPlan() const;
    // The bed's own acmod/lfeon plus every selected extra's bits, OR'd
    // together - what a request to chanmap::allocate() looks like from here.
    // Object mode overrides this entirely (see currentPlan()), so this never
    // needs to know about atmos_enabled_ itself.
    [[nodiscard]] std::uint16_t currentLocationMask() const;
    // currentPlan() resolved to its actual channels - what every display and
    // routing computation below should read. Assumes currentPlan() validates,
    // the way ac3cli's own resolve() does.
    [[nodiscard]] ac3::plan::ChannelPlan effectiveChannelPlan() const;
    // What the routing summary calls this plan: "5.1 bed" for object mode,
    // else the derived shape name (channelShapeName()).
    [[nodiscard]] QString effectiveLabel() const;
    // bed_acmod_ == kDualMono - checked often enough (currentPlan(),
    // channelShapeName(), extrasLocked()...) to name once. 1+1 is a bed, not
    // a location mask (see kBeds' own comment and acmod_map's "not a
    // layout" one in eac3_tables.hpp), so every one of those call sites has
    // to branch on this rather than run the general chanmap path.
    [[nodiscard]] bool isDualMono() const { return bed_acmod_ == ac3::Acmod::kDualMono; }

    // The primary source plus every extra, in load order - the same
    // concatenation order encodeTo() builds `planes` in, and what
    // ac3::plan::Assignment's (source, channel) addressing means here.
    // Empty when nothing is loaded.
    [[nodiscard]] std::vector<ac3::plan::SourceShape> sourceShapes() const;
    // objectModel()'s own sourceLabel for object `flatIndex` (a concatenated
    // index into sourceShapes(), the same addressing planes/Assignment use):
    // "Ch <n>" with exactly one source loaded - unchanged from what this has
    // always shown - else "<file> ch <n>", the same "<source> ch <n>"
    // phrasing assignmentRows()/unassignedWarnings() already use, so an
    // object and the channel it came from are named the same way everywhere
    // this app names one.
    [[nodiscard]] QString objectSourceLabel(std::size_t flatIndex) const;
    // The routing a run should actually use: automatic single-source panning
    // when exactly one source is loaded and nothing has been assigned
    // explicitly (byte-identical to what this controller has always done),
    // else the explicit Assignment - dual mono routed through
    // dual_mono_routing() rather than the general location-based route(),
    // for the same reason ac3cli's own routing_for_sources() picks between
    // them (see main.cpp). Returns nullopt if nothing is loaded, if more
    // than one source is loaded with no explicit assignment (automatic
    // panning has no defined meaning there), or if the assignment/automatic
    // routing itself cannot be built.
    [[nodiscard]] std::optional<ac3::plan::Routing> routingForSources(
        const ac3::plan::ChannelPlan& target, const ac3::plan::Plan& p) const;
    // The object-count/meter-preview/status bookkeeping addSourceFile and
    // removeSource both need after the source list changes - loadSourceFile
    // keeps its own equivalent tail untouched (see its own comments) rather
    // than sharing this, so replacing the primary source can never behave
    // differently because of a refactor here.
    void refreshAfterSourceListChange();

    // Channels through the plan and out as AC-3 or E-AC-3. One worker for
    // both: they differ only in which encoder object runs, and everything
    // around it - routing, metering, progress, the container - is identical.
    // `routing` is already built and validated by the caller (encodeTo, via
    // routingForSources) rather than recomputed here, so this function
    // cannot silently disagree with what the pre-encode preview already
    // showed.
    void encodeChannels(const QString& path, std::vector<std::vector<float>> planes,
                        const ac3::plan::Routing& routing, std::uint32_t sample_rate);
    // Objects over a 5.1 bed. `planes` is every loaded channel in flat order;
    // which of them ride as dynamic objects, which pin to a bed position as
    // static objects, and which are dropped follows the assignment table
    // (dynamicObjectChannels / pinnedObjectChannels) - with nothing assigned,
    // every channel is a dynamic object, which is what this always did.
    void encodeObjects(const QString& path, std::vector<std::vector<float>> planes,
                       std::uint32_t sample_rate);
    // Resizes object_configs_ to object_count_, preserving any object index
    // that survives the change and spreading newly-added ones out along x
    // instead of defaulting them all onto the same overlapping point (the
    // design brief's own complaint about the single-point-plus-spread model).
    // Called wherever object_count_ is set. Preserving by INDEX is only
    // honest when indices themselves have not shifted underneath - true for
    // a grown/shrunk primary (loadSourceFile always starts fresh anyway) and
    // for addSourceFile (a new source's channels only ever append past the
    // old count) - so removeSource() clears object_configs_/
    // object_keyframes_ itself first for a non-primary removal, the same
    // "clear rather than risk silently reattaching to the wrong channel"
    // call assignment_ already makes there, before this ever runs.
    void refreshObjectConfigs();
    // The shared lookup addObjectKeyframe/removeObjectKeyframe/
    // objectKeyframes/evaluateObjectPath all build on: object_keyframes_'s
    // entry for this index, sorted by time, or empty if it has none.
    [[nodiscard]] std::vector<ac3::oba::Keyframe> sortedKeyframes(int objectIndex) const;

    struct ObjectConfig;
    // The live session worker. One function for both channel and object mode
    // (mirrors ac3cli's own `live` command, which combines them the same way)
    // rather than split like encodeChannels/encodeObjects: almost everything
    // here - capture, monitor, passthrough, the disk-write, the live counters
    // - is identical between the two, and only the "turn source samples into
    // one encoded unit" step differs.
    void runLiveSession(ac3::capture::DeviceInfo device, bool monitor, bool passthrough,
                        bool write_to_disk, QString file_path);
    // A snapshot of object_configs_ that setObjectPosition/setObjectLfeSend
    // also keep current, guarded by live_object_mutex_ - the one piece of
    // state the live worker thread and the GUI thread genuinely touch
    // concurrently (dragging the room while a live Atmos session runs).
    [[nodiscard]] std::vector<ObjectConfig> liveObjectSnapshot() const;

    // Writes an elementary stream, or muxes Matroska, according to the chosen
    // container. Returns an empty string on success and the reason otherwise.
    [[nodiscard]] QString writeOutput(const QString& path,
                                      const std::vector<std::vector<std::byte>>& frames,
                                      std::uint32_t sample_rate, int channels) const;

    void setStatus(const QString& text);
    void setBusy(bool busy);
    // Adds a new "encoding" entry to runs_ and remembers its id, so the
    // encodeFinished this run eventually emits (there are several call
    // sites; a run is always started right after setBusy(true) rather than
    // duplicated at each one) knows which entry to settle.
    void startRun(const QString& path);
    // Connected to encodeFinished in the constructor. A run whose message
    // mentions cancellation reads "cancelled" rather than "failed" - the
    // same text setStatus() already shows, not a second judgement of it.
    void finishRun(bool ok, const QString& message);
    void setProgress(double value);
    void setRecording(bool recording);
    void setMetering(bool metering);
    // Recomputes routingSummary from the current plan and source, then hands
    // the meters to previewPlanMeters(). Called whenever either moves.
    void refreshRouting();
    // The routingSummary half of refreshRouting - the prose only, split out
    // so refreshRouting can always follow it with the meter preview without
    // every early return in here having to remember to.
    void refreshRoutingSummary();
    // Points the meters at the CODED plan while nothing is running: labels,
    // locations and fed flags immediately (cheap - no audio is touched), then
    // a background pass that renders the loaded sources through the actual
    // routing and publishes the whole-programme levels when it lands. This is
    // what makes the meters follow the picker and the assignment table - the
    // handoff's "the meters on the left follow these choices". A run starting
    // before the pass lands invalidates it (preview_generation_), and busy_
    // suppresses the whole thing: a live worker owns the meters then.
    void previewPlanMeters();
    // Flat channel indices (the sourceShapes()/Assignment addressing) that
    // ride as DYNAMIC objects in object mode: every loaded channel while
    // nothing is explicitly assigned, else exactly the channels assigned
    // "obj". Order is flat order, which is object-index order.
    [[nodiscard]] std::vector<std::size_t> dynamicObjectChannels() const;
    // Flat channels assigned to a bed position in object mode. Each becomes a
    // static object pinned at its speaker's azimuth - in a JOC stream the bed
    // IS the panned objects, so "carried as a channel" and "an object that
    // never moves off the L speaker" are the same coded thing. The LFE
    // position pins as a pure lfe_send object (no direction points at it).
    [[nodiscard]] std::vector<std::pair<std::size_t, ac3::eac3::chanmap::Location>>
    pinnedObjectChannels() const;
    // object_count_ from dynamicObjectChannels(), then refreshObjectConfigs().
    // Called wherever the source list or the assignment changes.
    void recomputeObjectCount();
    // Coalesces objectsChanged for the drag paths (setObjectPosition /
    // setObjectLfeSend): the first move in a gesture notifies immediately,
    // further ones inside ~16 ms ride a trailing single-shot. Four Repeaters
    // re-read objectModel on every emission, so per-mouse-move emission made
    // dragging the room a delegate-rebuild storm.
    void notifyObjectsChangedSoon();

    // Re-labels the meters and clears them to the floor. GUI thread only, and
    // always before a worker that will publish into them starts. `names` and
    // `coded` may be wider than the acmod, for a layout built from dependent
    // substreams; `coded` carries each entry's actual Table E2.5 location and
    // whether it is a bed channel a dependent replaces, which is what lets
    // publishLevels() place a channel on the right soundfield ring (or the
    // right one of the two, ear-level vs ceiling) without asking the acmod
    // alone, which only ever knew about the bed's own five positions.
    //
    // `fed` says which of those channels the routing actually puts audio into.
    // A channel the source cannot fill reads -inf for a legitimate reason, and
    // that is a different thing from a meter wired to nothing; an empty vector
    // means every channel is fed.
    void setLayout(ac3::Acmod acmod, bool lfe, const QStringList& names, const QString& label,
                   const std::vector<ac3::plan::CodedChannel>& coded,
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
    // The handoff's "partial output is named and kept" behaviour - see the
    // keepPartialOutput property. Snapshotted into each encode worker at
    // start, so mid-run preference edits apply to the NEXT run.
    bool keep_partial_output_ = true;
    QString status_ = QStringLiteral("Choose a WAV file, or record from a capture device.");
    QString routing_summary_;
    bool source_ready_ = false;
    bool busy_ = false;
    bool recording_ = false;
    double progress_ = 0.0;
    double recorded_seconds_ = 0.0;
    int bitrate_kbps_ = 192;
    bool vbr_enabled_ = false;
    int vbr_quality_ = 75;
    bool vbr_min_enabled_ = false;
    std::uint32_t vbr_min_kbps_ = 192;
    bool vbr_max_enabled_ = false;
    std::uint32_t vbr_max_kbps_ = 640;

    ac3::plan::Codec codec_ = ac3::plan::Codec::kAc3;
    // Tier 1: the bed and its independent LFE. Defaults to stereo, matching
    // what a freshly opened window always used to call itself; loading a
    // source or picking a preset moves it.
    ac3::Acmod bed_acmod_ = ac3::Acmod::k2_0;
    bool bed_lfe_ = false;
    // Tier 2: OR of the selected extras' Table E2.5 bits (kLwRw, kLrsRrs,
    // kVhlVhr, kLtsRts, kLfe2 - see kExtras in the .cpp).
    std::uint16_t extras_mask_ = 0;
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
    int object_count_ = 0;
    int selected_object_index_ = 0;
    // One static position per object - independent now, not a shared point
    // plus a spread fan-out. Resized (and freshly spread out, so a loaded
    // file's objects do not all default onto the same overlapping point) in
    // refreshObjectConfigs() whenever object_count_ changes.
    struct ObjectConfig {
        double x = 0.5;
        double y = 0.0;
        double z = 0.0;
        // Objects never reach the LFE by panning - there is no direction
        // that points at it - so this send is the only route, and without
        // it the bed's LFE is silent however the objects are placed.
        double lfe_send = 0.15;
    };
    std::vector<ObjectConfig> object_configs_;
    // Authored motion, keyed by object index. An index absent here (the
    // common case) falls back to the object's static ObjectConfig placement
    // in encodeObjects, held constant for the whole file.
    QHash<int, std::vector<ac3::oba::Keyframe>> object_keyframes_;
    // A snapshot of object_configs_/object_keyframes_/selected_object_index_
    // as they stood before a live Atmos session resized them to the CAPTURE
    // DEVICE's channel count instead of a loaded file's (see
    // startLiveSession's own comment on why that resize happens at all).
    // Restored once the session ends (runLiveSession's completion callback),
    // so an unrelated live excursion can never permanently clobber authored
    // object placements/motion a loaded file already had. nullopt means
    // nothing needs restoring - no session has resized anything yet, or the
    // device's channel count already matched and nothing was touched.
    struct LiveObjectBackup {
        int count = 0;
        std::vector<ObjectConfig> configs;
        QHash<int, std::vector<ac3::oba::Keyframe>> keyframes;
        int selected_index = 0;
    };
    std::optional<LiveObjectBackup> live_object_backup_;

    QVariantList runs_;
    int current_run_id_ = -1;
    int next_run_id_ = 1;
    // Set right before encodeChannels' completion callback emits
    // encodeFinished, consumed once by finishRun() and cleared - the "NNN
    // kbps" or, for a VBR run, "VBR q75 · avg 512 kbps (384-704)" text the
    // run strip shows once a run is no longer "encoding". startRun() already
    // wrote a live-appropriate placeholder into the same run's rateText;
    // this is what replaces it once the real per-frame sizes are known.
    QString pending_rate_text_;

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
    // Parallel to channel_names_/channel_fed_: each entry's Table E2.5
    // location (for soundfield placement) and whether it is a bed channel a
    // dependent substream replaces (for the Coded/Rendered meter split).
    std::vector<ac3::eac3::chanmap::Location> channel_locations_;
    std::vector<bool> channel_replaced_;
    QString layout_name_;
    QVariantList channel_levels_;
    QVariantMap soundfield_;

    // shared_ptr rather than unique_ptr for exactly one reason: the meter
    // preview worker (previewPlanMeters) reads the WAV data off the GUI
    // thread, and loadSourceFile may replace the source before that read
    // finishes. WavData is immutable once loaded, so shared ownership is the
    // whole synchronisation story; the stale worker's publish is dropped by
    // its generation check instead.
    std::shared_ptr<Source> source_;
    // Everything beyond the primary, in load order - source index (n+1) in
    // sourceShapes()/Assignment addressing. Always empty with the single-
    // source behaviour every existing call site (still) assumes.
    std::vector<std::shared_ptr<Source>> extra_sources_;
    // Invalidates in-flight meter previews: bumped by every new preview and
    // by setBusy(true), checked (against busy_ too) before a preview's
    // result is published.
    std::atomic<int> preview_generation_{0};
    // notifyObjectsChangedSoon()'s state - see its declaration.
    QTimer object_notify_timer_;
    QElapsedTimer object_notify_elapsed_;
    // Empty (every row implicitly kUnassigned) until setAssignment is
    // called at least once; see routingForSources for what that means for
    // which routing actually gets used.
    ac3::plan::Assignment assignment_;
    bool has_explicit_assignment_ = false;
    // Every (source, channel) setAssignment has ever been called for, "none"
    // included - Assignment itself cannot tell an explicit "none" apart from
    // a channel nobody has visited yet (see assignment.hpp's own doc
    // comment; parse_assignment works around the same gap differently, by
    // tracking coverage locally while it still has a token to blame). Reset
    // everywhere assignment_ itself is reset, so unassignedWarnings can
    // subtract this from Assignment::unassigned()'s raw inventory and stop
    // nagging about a channel the user deliberately silenced.
    std::set<std::pair<std::size_t, std::size_t>> touched_channels_;
    std::unique_ptr<ac3::capture::Capture> capture_;
    std::atomic_bool cancel_requested_{false};
    std::atomic_bool stop_recording_{false};

    // ---- live session --------------------------------------------------
    // What startLiveSession was asked for, kept so switchLiveLayout can
    // restart the session under a new preset without the QML having to
    // re-supply choices it made minutes ago. Write-to-disk is deliberately
    // NOT restartable - see switchLiveLayout's declaration.
    struct LiveSessionRequest {
        int capture_index = -1;
        bool monitor = false;
        int receiver_index = -1;
    };
    std::optional<LiveSessionRequest> live_request_;
    // Set by switchLiveLayout, consumed once by the session-completion
    // callback: apply this preset, then restart from live_request_.
    std::optional<QString> pending_live_relayout_;
    // stop_live_ is the only piece of this state the worker thread reads;
    // everything else it only ever touches through a QMetaObject::invokeMethod
    // back onto the GUI thread (the same discipline startRecording's worker
    // already follows), so plain members are safe even though a background
    // thread is what makes them change.
    std::atomic_bool stop_live_{false};
    bool live_active_ = false;
    bool live_monitoring_ = false;
    bool live_passthrough_ = false;
    bool live_writing_to_disk_ = false;
    QString live_receiver_plan_text_;
    bool live_gap_ = false;
    bool live_reconnecting_ = false;
    double live_running_seconds_ = 0.0;
    qint64 live_frames_encoded_ = 0;
    qint64 live_frames_dropped_ = 0;
    quint64 live_underruns_ = 0;
    double live_latency_ms_ = 0.0;
    // Genuinely shared with the live worker thread (dragging the Live
    // session's room, or the Objects tab's, while a live Atmos session is
    // running): every read and write goes through live_object_mutex_.
    mutable std::mutex live_object_mutex_;
    std::vector<ObjectConfig> live_object_snapshot_;
    // Opened and (via the worker's final invokeMethod) closed on the GUI
    // thread, matching capture_'s own convention - only buffer()/submit()/
    // stats() are called from the worker while a session runs.
    std::unique_ptr<ac3::capture::Capture> live_capture_;
    std::unique_ptr<ac3::sinks::MonitorSink> live_monitor_sink_;
    std::unique_ptr<ac3::sinks::PassthroughSink> live_passthrough_sink_;
};
