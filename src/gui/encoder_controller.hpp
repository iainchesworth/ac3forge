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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/capture/capture.hpp"
#include "ac3/capture/resampler.hpp"
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
    // {index, label, path, channels, primary, rate, seconds, duration,
    // offsetSeconds}. Index 0 is always the primary (removeSource(0) drops
    // everything rather than promoting an extra, since there is no honest
    // way to guess which one should take its place). With exactly one
    // source loaded this is a single row and nothing else here changes
    // anything - see routingForSources(). offsetSeconds is that source's
    // start offset (see setSourceOffset) - the Objects tab's derived
    // timeline length is max(offsetSeconds + seconds) over every row.
    // resampleLabel is "44.1→48 k" when addSourceFile had to resample this
    // source to the primary's rate (empty otherwise, including for the
    // primary itself, which is never resampled) - see addSourceFile's own
    // comment on why a mismatch is fixed up rather than refused now.
    Q_PROPERTY(QVariantList sourceModel READ sourceModel NOTIFY sourceChanged)
    // Whole-programme peak/RMS PRE-ROUTING, one entry per sourceModel row in
    // the same order ({peakDb, rmsDb}) - the rail's per-source level pip.
    // Deliberately a SEPARATE property from sourceModel rather than a field
    // on it: sourceModel is what a Repeater should bind to for row identity
    // (see its own doc comment above), and folding a value that moves with
    // every meter tick into it would rebuild every rail delegate the same
    // way binding a Repeater to channelLevels instead of channelMeta would -
    // see channelMeta's doc comment for the identical reasoning. A rail row
    // looks its own entry up BY INDEX, the same lookup-by-index pattern
    // ChannelMeter's delegate already uses against channelLevels. Computed
    // in the same background pass previewPlanMeters() already runs
    // (file sources only - see previewPlanMeters' own comment on why object
    // mode and live capture rows are out of scope here), so it lags exactly
    // as far behind an edit as channelLevels already does.
    Q_PROPERTY(QVariantList sourceLevels READ sourceLevels NOTIFY sourceLevelsChanged)
    // One row per (source, channel) sourceModel declares -
    // {source, channel, sourceLabel, destToken, touched, trimDb} -
    // destToken in plan::parse_destination's own vocabulary ("L", "obj",
    // "objm", "p1", "p2", "none"), so setAssignment's argument is always
    // something this list itself already printed. Every row exists whether
    // or not it has been explicitly assigned yet (an unset one reads
    // "none"), so a caller can always render one row per channel rather
    // than special-casing the gap. trimDb is that row's current
    // Destination::trim_db (0 for an untrimmed or unassigned row) - the
    // trim control's starting value; setAssignmentTrim is its write side.
    Q_PROPERTY(QVariantList assignmentRows READ assignmentRows NOTIFY sourceChanged)
    // plan::format_assignment() of the explicit assignment, prefixed "map=" -
    // the exact token ac3cli's encode/eac3-encode take, so the command bar
    // can append it verbatim and a GUI assignment is always reproducible on
    // the command line. Empty while automatic single-source routing applies:
    // there is no map= to print when nothing has been mapped.
    Q_PROPERTY(QString mapToken READ mapToken NOTIFY sourceChanged)
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
    // See loudnessTouchedChanged()'s own comment. Written by LoudnessGroup.qml's
    // interactive handlers only - never by the setters those handlers call,
    // and never by guided's own contract application.
    Q_PROPERTY(bool loudnessTouched READ loudnessTouched WRITE setLoudnessTouched NOTIFY
                   loudnessTouchedChanged)
    // The loudnessTouched pattern, generalised to Preferences' OTHER
    // session-carrying defaults - container, rate mode, bit rate and VBR
    // quality (DRC profile and measure-loudness stay on loudnessTouched
    // itself; there is no reason to invent a second flag for fields that
    // mechanism already covers). Written only by the genuinely interactive
    // controls that edit those four fields (the Format tab's container/
    // bit-rate pickers, VbrPanel's rate-mode/quality controls, Guided's
    // quality-step rate cards, the Objects tab's "Set it" 384 kbps button) -
    // never by an automatic floor bump (AssignmentPanel's/Guided's own
    // object-mode 384 kbps correction), Preferences' own initial
    // Component.onCompleted application, or session restore. Read by
    // PreferencesDialog's onApplied handler before re-applying a changed
    // default on Save, the same guard loudnessTouched already gives DRC/
    // measure-loudness.
    Q_PROPERTY(bool formatDefaultsTouched READ formatDefaultsTouched WRITE setFormatDefaultsTouched
                   NOTIFY formatDefaultsTouchedChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // Encoding is a job with a history, not a modal moment: one entry per
    // file encode, recording, and real live session (one with a take on
    // disk or a receiver leg - monitor-only checks deliberately stay out),
    // newest first. Each is {id, filename, path, bitrateKbps, rateText,
    // durationText, status ("encoding"|"done"|"failed"|"cancelled"),
    // sizeText, detail, framesText, cliLine, eac3, playDeviceIndex}. cliLine
    // is the ac3cli command line SNAPSHOTTED when the run started (see
    // setPendingCliLine) - never recomputed later, so a popover opened long
    // after still shows what actually ran rather than whatever the command
    // bar currently reads. eac3 is whether THIS run's own output holds
    // E-AC-3 (captured at start, independent of the shared outputIsEac3
    // property a later run can overwrite) - what a run chip's own Play
    // action checks a receiver against, via outputDeviceSupportsFormat.
    // playDeviceIndex is the receiver device Guided's amp destination had
    // already picked when this run started (see setPendingPlayDevice), or
    // -1 for a run with no such pre-selection - Play falls back to the
    // Format tab's own passthrough picker for those. rateText is what the
    // run strip actually displays - "384 kbps" for CBR, or, once a VBR run
    // finishes, "VBR q75 · avg 512 kbps (384-704)": a VBR run has no target
    // rate to show while "encoding" (only the quality it is aiming for), and
    // a real one to report once its actual frame sizes are known (see
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
    // ---- live capture device selection (the rail's live branch) -----------
    // The devices a live session will use once started - not every device
    // captureDevices() lists, just the ones picked via addCaptureDevice.
    // Capped at two (the clock-master model's own limit): row 0 is always
    // the MASTER, whose delivery paces the session's frame loop exactly as a
    // single-device session always has; row 1, when present, is the SLAVE,
    // resampled to track the master (see docs/gui/live-session.md). One row
    // per selection - {slotIndex, deviceIndex, name, channels, rateText,
    // isMaster} - deviceIndex is captureDevices()'s own numbering, the same
    // ac3::capture::enumerate_devices() index startLiveSession/
    // addLiveObject already take. Monitor/Record (the rail's own buttons,
    // outside a real session) only ever use row 0 - see RailBlock's live
    // branch in Main.qml.
    Q_PROPERTY(QVariantList captureDeviceRows READ captureDeviceRows NOTIFY captureDeviceRowsChanged)
    Q_PROPERTY(int captureDeviceCount READ captureDeviceCount NOTIFY captureDeviceRowsChanged)
    // True at the two-device cap - what "Add input…" disables on, with the
    // rail's own "two devices per session" note explaining why.
    Q_PROPERTY(bool captureDeviceCapReached READ captureDeviceCapReached NOTIFY
                   captureDeviceRowsChanged)
    // "2 devices · 4 channels captured" - the rail's totals line. Empty with
    // nothing selected, the same convention sourceModel's own totals line
    // uses.
    Q_PROPERTY(QString captureDeviceTotals READ captureDeviceTotals NOTIFY
                   captureDeviceRowsChanged)
    // One label per flat capture-channel index across every selected device
    // ("Ch 1".."Ch N" for device 1, then "Dev2 Ch 1".."Dev2 Ch N") - what the
    // Live room's object-channel picker builds its model from once a second
    // device can contribute channels of its own. liveDeviceChannels is the
    // combined count these index into.
    Q_PROPERTY(QStringList liveCaptureChannelLabels READ liveCaptureChannelLabels NOTIFY
                   liveActiveChanged)
    Q_PROPERTY(QStringList outputDevices READ outputDevices NOTIFY outputDevicesChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY outputChanged)
    // Whether the file at outputPath holds E-AC-3 (object mode included) or
    // plain AC-3 - recorded when the encode/recording set outputPath, so the
    // Play button can gate on the SELECTED endpoint's ability to bitstream
    // this stream (outputDeviceCanBitstream) instead of failing after the
    // click.
    Q_PROPERTY(bool outputIsEac3 READ outputIsEac3 NOTIFY outputChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(double recordedSeconds READ recordedSeconds NOTIFY recordedSecondsChanged)

    // ---- format -----------------------------------------------------------
    // AC-3 (bsid 8) or E-AC-3 (bsid 16). The codec gates almost everything
    // else: AC-3 has no substream layer, so no layout wider than 5.1, and no
    // Annex E coding tools or mixmdate group.
    Q_PROPERTY(int codecIndex READ codecIndex WRITE setCodecIndex NOTIFY planChanged)
    Q_PROPERTY(QStringList codecNames READ codecNames CONSTANT)
    Q_PROPERTY(QString layoutDetail READ layoutDetail NOTIFY planChanged)
    // The two halves of the mockup's "8 coded · 6 spk" split: how many
    // channels the stream transmits, and how many speakers a receiver
    // actually drives once a dependent substream's replacements are folded
    // in. Dual mono answers its two programmes; object mode the fixed 5.1
    // bed. Derived from the same resolve() every other display reads.
    Q_PROPERTY(int codedChannelCount READ codedChannelCount NOTIFY planChanged)
    Q_PROPERTY(int renderedChannelCount READ renderedChannelCount NOTIFY planChanged)
    // codedChannelCount minus whichever LFE channel(s) it counts - the
    // advisory floor below is stated per full-bandwidth channel because that
    // is where the bits actually go (§7.2.2 never spends a bandwidth budget
    // on the LFE band the way it does on a full-bandwidth one). Only
    // meaningful outside object mode; see the getter.
    Q_PROPERTY(int fullBandwidthCodedChannelCount READ fullBandwidthCodedChannelCount NOTIFY
                   planChanged)
    // A field-level hint, not a hard floor - deliberate stress-test encodes
    // below this stay perfectly legal, and the frame's own hard minimum is a
    // separate, harder line enforced at encode time regardless of this
    // value. Derived from the existing object-mode precedent: 384 kbps is
    // judged right for a 5.1 bed carrying objects (objects-and-motion.md),
    // which is 384/5 ≈ 77 kbps per full-bandwidth channel - the same rate,
    // generalised from "objects over 5.1" to any layout.
    Q_PROPERTY(int kbpsPerChannelFloor READ kbpsPerChannelFloor CONSTANT)
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
    // Ch2's own DRC/heavy - dual mono only, and not a fallback from the
    // fields above (plan::Metadata::drc2's comment explains why): a plan
    // that wants both programmes compressed alike sets both explicitly.
    Q_PROPERTY(int drc2Index READ drc2Index WRITE setDrc2Index NOTIFY planChanged)
    Q_PROPERTY(bool heavy2 READ heavy2 WRITE setHeavy2 NOTIFY planChanged)
    Q_PROPERTY(double ceiling2Db READ ceiling2Db WRITE setCeiling2Db NOTIFY planChanged)
    Q_PROPERTY(double dialogue2Db READ dialogue2Db WRITE setDialogue2Db NOTIFY planChanged)
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
    // Every non-default metadata choice as ac3cli's own trailing tokens
    // ("drc=film_standard dialnorm=auto heavy …"), space-joined and in
    // print_meta_usage()'s exact grammar; empty when everything is at its
    // default, so a plain encode's command line stays a plain line.
    Q_PROPERTY(QString metaTokens READ metaTokens NOTIFY planChanged)

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
    // One row per object: {index, sourceLabel, sourceIndex, x, y, z,
    // lfeSend, hasPath, keyCount, pathLabel} - room-anchored per §4.2.1
    // (x 0 at the left wall to 1 at the right, y 0 at the front wall to 1
    // at the back, z -1 at the floor to +1 at the ceiling). Backs both the
    // room plan's markers and the object list table, so the two can never
    // disagree about a position. sourceIndex is that object's channel's row
    // in sourceModel - what the timeline's clip-band drag uses to find
    // every object a given source owns (see shiftObjectKeyframes).
    Q_PROPERTY(QVariantList objectModel READ objectModel NOTIFY objectsChanged)
    // How many channels the assignment pins to bed positions in object mode
    // (each one a static object - see pinnedObjectChannels). The Objects
    // tab's "N of M" budget line subtracts these from the fifteen dynamic
    // slots rather than overstating what is left.
    Q_PROPERTY(int pinnedObjectCount READ pinnedObjectCount NOTIFY sourceChanged)
    // The Objects tab's audible motion preview: every object rendered
    // through ac3::oba::AtmosEncoder exactly as encodeObjects() would, its
    // 5.1 bed played back live through the same MonitorSink path a live
    // session uses, paced in real time rather than run flat-out - see
    // startMotionPreview(). motionPreviewActive is true only while that
    // worker is actually playing; motionPreviewTime is its audio clock, in
    // seconds since the preview started - what the timeline's playhead
    // follows while a preview plays, instead of a free-running visual-only
    // Timer, so the two can never drift apart.
    Q_PROPERTY(bool motionPreviewActive READ motionPreviewActive NOTIFY motionPreviewActiveChanged)
    Q_PROPERTY(double motionPreviewTime READ motionPreviewTime NOTIFY motionPreviewTimeChanged)
    // ---- live session's object slots ---------------------------------------
    // A live Atmos session pre-allocates a fixed budget of object slots (see
    // startLiveSession's own comment on why objectCount is a budget, not a
    // channel count, once a session is running) - these two answer which of
    // that budget's slots actually carry a capture channel right now, index-
    // aligned with objectModel(). Empty/zero outside a live Atmos session;
    // NOTIFY objectsChanged the same as objectModel itself, since a bind or
    // reassignment is exactly the kind of change that signal already covers.
    Q_PROPERTY(QVariantList liveObjectChannels READ liveObjectChannels NOTIFY objectsChanged)
    Q_PROPERTY(int liveObjectSlotsBound READ liveObjectSlotsBound NOTIFY objectsChanged)
    // The live device's own channel count - what the Live session tab's
    // channel picker for "Add an object"/reassignment builds its "Ch 1".."Ch
    // N" model from, and what addLiveObject/reassignLiveObjectSlot validate
    // a capture-channel argument against. 0 outside a live session.
    Q_PROPERTY(int liveDeviceChannels READ liveDeviceChannels NOTIFY liveActiveChanged)

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
    // The pre-flight "also write a raw-WAV safety copy" checkbox on the Live
    // session tab - a plain property (not a startLiveSession argument) so its
    // signature/semantics stay exactly what they were before this existed;
    // startLiveSession reads this the same way it already reads
    // keepPartialOutput. Meaningless without writeToDisk itself - the copy
    // rides "alongside the take", not in place of it - so it is only
    // consulted when a session is actually asked to write.
    Q_PROPERTY(bool liveWavSafetyCopy READ liveWavSafetyCopy WRITE setLiveWavSafetyCopy NOTIFY
                   liveWavSafetyCopyChanged)
    // What the receiver leg can actually carry, and whether that is less than
    // the main encode plan. Object mode is always a gap: TS 103 420's JOC
    // layer plays as its 5.1 bed on any real decoder we have tried, ours
    // included (see docs - the decoder's object gate is keyed, and forging
    // that key is deliberately not done), so a live Atmos session is never
    // heard as Atmos on the other end, independent of what the receiver
    // device itself supports.
    Q_PROPERTY(QString liveReceiverPlanText READ liveReceiverPlanText NOTIFY liveActiveChanged)
    Q_PROPERTY(bool liveGap READ liveGap NOTIFY liveActiveChanged)
    // The chain strip's capture sub-line ("2 ch · 48 000 Hz") and the pieces
    // the session tab needs to tell the receiver's own story: which endpoint
    // this session actually asked for (name), whether it was asked at all
    // (wanted - livePassthrough false + wanted true is "asked and refused",
    // a different banner from "never asked"), and whether that endpoint can
    // take E-AC-3 - what decides which layout-switcher entries genuinely
    // exceed the receiver leg rather than a hardcoded "anything past 5.1".
    Q_PROPERTY(QString liveCaptureDetail READ liveCaptureDetail NOTIFY liveActiveChanged)
    Q_PROPERTY(QString liveReceiverName READ liveReceiverName NOTIFY liveActiveChanged)
    Q_PROPERTY(bool liveWantedPassthrough READ liveWantedPassthrough NOTIFY liveActiveChanged)
    Q_PROPERTY(bool liveReceiverEac3 READ liveReceiverEac3 NOTIFY liveActiveChanged)
    // Set for a couple of seconds right after the passthrough endpoint opens
    // - a real exclusive-mode stream open, which is exactly when a physical
    // receiver drops its lock and re-negotiates.
    Q_PROPERTY(bool liveReconnecting READ liveReconnecting NOTIFY liveReconnectingChanged)
    Q_PROPERTY(double liveRunningSeconds READ liveRunningSeconds NOTIFY liveStatsChanged)
    Q_PROPERTY(qint64 liveFramesEncoded READ liveFramesEncoded NOTIFY liveStatsChanged)
    Q_PROPERTY(qint64 liveFramesDropped READ liveFramesDropped NOTIFY liveStatsChanged)
    Q_PROPERTY(quint64 liveUnderruns READ liveUnderruns NOTIFY liveStatsChanged)
    // A computed lower bound (two frame periods: one to fill the capture
    // buffer, one to encode and hand off) until the real thing lands - see
    // liveLatencyMeasured. Zero when nothing is running, since there is
    // nothing to estimate yet. NOTIFY is liveStatsChanged, not
    // liveActiveChanged: the one-shot measurement replaces this mid-session,
    // once per run, well after the value liveActiveChanged already covered
    // at session start.
    Q_PROPERTY(double liveLatencyMs READ liveLatencyMs NOTIFY liveStatsChanged)
    // Whether liveLatencyMs is currently the real capture->monitor round
    // trip (measured once, after the pipeline settles) or still the
    // two-frame estimate above - what decides the "measured" vs "est."
    // label the Live session tab prints next to it. Stays false for the
    // whole session when monitoring is off, since there is nothing to time
    // a round trip against.
    Q_PROPERTY(bool liveLatencyMeasured READ liveLatencyMeasured NOTIFY liveStatsChanged)
    // ---- two-device capture -------------------------------------------------
    // Whether this session actually opened a second capture device (as
    // opposed to one merely being selected in captureDeviceRows - a device
    // can vanish between selection and start, the same as the master
    // already can).
    Q_PROPERTY(bool liveSecondDeviceActive READ liveSecondDeviceActive NOTIFY liveActiveChanged)
    Q_PROPERTY(QString liveSecondDeviceName READ liveSecondDeviceName NOTIFY liveActiveChanged)
    // The slave's measured clock correction, in signed parts-per-million
    // relative to its nominal rate - positive means the slave is running
    // fast and the resampler is dropping extra input to keep pace. Zero
    // (and liveDriftText empty) until liveSecondDeviceActive and the
    // estimator's sliding window has data. Honest, not estimated ahead of
    // time: this is the correction the resampler is actually applying,
    // published with the same ~30 Hz cadence as the other live stats.
    Q_PROPERTY(double liveDriftPpm READ liveDriftPpm NOTIFY liveStatsChanged)
    // "slave -18 ppm" - what the chain's capture cell prints under the
    // device detail line. Empty outside a two-device session.
    Q_PROPERTY(QString liveDriftText READ liveDriftText NOTIFY liveStatsChanged)
    // ---- parallel downmix receiver leg --------------------------------------
    // True when the receiver leg is NOT bitstreaming the main plan but a
    // second, independent AC-3 encoder's 5.1 fold-down instead - because the
    // main plan needs E-AC-3 (a wide channel layout, or any object session)
    // and the chosen receiver cannot take it, but can take plain AC-3. Drives
    // the chain's receiver cell, the Receiver reports rows, the gap banner
    // text and the layout switcher's legend copy - see liveGap, which this
    // also makes true (the leg always carries less than the main encode).
    Q_PROPERTY(bool liveDownmixLeg READ liveDownmixLeg NOTIFY liveActiveChanged)

public:
    explicit EncoderController(QObject* parent = nullptr);
    ~EncoderController() override;

    [[nodiscard]] QString sourcePath() const { return source_path_; }
    [[nodiscard]] QString sourceInfo() const { return source_info_; }
    [[nodiscard]] bool sourceReady() const { return source_ready_; }
    [[nodiscard]] QVariantList sourceModel() const;
    [[nodiscard]] QVariantList sourceLevels() const { return source_levels_; }
    [[nodiscard]] QVariantList assignmentRows() const;
    [[nodiscard]] QString mapToken() const;
    [[nodiscard]] QString metaTokens() const;
    [[nodiscard]] QStringList unassignedWarnings() const;
    [[nodiscard]] QString outputPath() const { return output_path_; }
    [[nodiscard]] bool keepPartialOutput() const { return keep_partial_output_; }
    void setKeepPartialOutput(bool keep);
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool loudnessTouched() const { return loudness_touched_; }
    void setLoudnessTouched(bool touched);
    [[nodiscard]] bool formatDefaultsTouched() const { return format_defaults_touched_; }
    void setFormatDefaultsTouched(bool touched);
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
    [[nodiscard]] QVariantList captureDeviceRows() const;
    [[nodiscard]] int captureDeviceCount() const {
        return static_cast<int>(live_selected_devices_.size());
    }
    [[nodiscard]] bool captureDeviceCapReached() const { return live_selected_devices_.size() >= 2; }
    [[nodiscard]] QString captureDeviceTotals() const;
    [[nodiscard]] QStringList liveCaptureChannelLabels() const;
    [[nodiscard]] QStringList outputDevices() const { return output_devices_; }
    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] bool canPlay() const { return !output_path_.isEmpty(); }
    [[nodiscard]] bool outputIsEac3() const { return output_eac3_; }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] double recordedSeconds() const { return recorded_seconds_; }

    [[nodiscard]] int codecIndex() const { return static_cast<int>(codec_); }
    [[nodiscard]] QStringList codecNames() const;
    [[nodiscard]] QString layoutDetail() const;
    [[nodiscard]] int codedChannelCount() const;
    [[nodiscard]] int renderedChannelCount() const;
    [[nodiscard]] int fullBandwidthCodedChannelCount() const;
    [[nodiscard]] int kbpsPerChannelFloor() const;
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
    [[nodiscard]] int drc2Index() const { return drc2_index_; }
    [[nodiscard]] bool heavy2() const { return meta_.heavy2.has_value(); }
    [[nodiscard]] double ceiling2Db() const { return ceiling2_db_; }
    [[nodiscard]] double dialogue2Db() const { return dialogue2_db_; }
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
    [[nodiscard]] int pinnedObjectCount() const;
    [[nodiscard]] bool motionPreviewActive() const { return motion_preview_active_; }
    [[nodiscard]] double motionPreviewTime() const { return motion_preview_time_; }
    [[nodiscard]] QVariantList liveObjectChannels() const;
    [[nodiscard]] int liveObjectSlotsBound() const;
    [[nodiscard]] int liveDeviceChannels() const { return live_device_channels_; }

    [[nodiscard]] bool liveActive() const { return live_active_; }
    [[nodiscard]] bool liveMonitoring() const { return live_monitoring_; }
    [[nodiscard]] bool livePassthrough() const { return live_passthrough_; }
    [[nodiscard]] bool liveWritingToDisk() const { return live_writing_to_disk_; }
    [[nodiscard]] bool liveWavSafetyCopy() const { return live_wav_safety_copy_; }
    void setLiveWavSafetyCopy(bool on);
    [[nodiscard]] QString liveReceiverPlanText() const { return live_receiver_plan_text_; }
    [[nodiscard]] bool liveGap() const { return live_gap_; }
    [[nodiscard]] bool liveReconnecting() const { return live_reconnecting_; }
    [[nodiscard]] double liveRunningSeconds() const { return live_running_seconds_; }
    [[nodiscard]] qint64 liveFramesEncoded() const { return live_frames_encoded_; }
    [[nodiscard]] qint64 liveFramesDropped() const { return live_frames_dropped_; }
    [[nodiscard]] quint64 liveUnderruns() const { return live_underruns_; }
    [[nodiscard]] double liveLatencyMs() const { return live_latency_ms_; }
    [[nodiscard]] bool liveLatencyMeasured() const { return live_latency_measured_; }
    [[nodiscard]] bool liveSecondDeviceActive() const { return live_second_device_active_; }
    [[nodiscard]] QString liveSecondDeviceName() const { return live_second_device_name_; }
    [[nodiscard]] double liveDriftPpm() const { return live_drift_ppm_; }
    [[nodiscard]] QString liveDriftText() const;
    [[nodiscard]] bool liveDownmixLeg() const { return live_downmix_leg_; }
    [[nodiscard]] QString liveCaptureDetail() const { return live_capture_detail_; }
    [[nodiscard]] QString liveReceiverName() const { return live_receiver_name_; }
    [[nodiscard]] bool liveWantedPassthrough() const { return live_wanted_passthrough_; }
    [[nodiscard]] bool liveReceiverEac3() const { return live_receiver_eac3_; }

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
    void setDrc2Index(int index);
    void setHeavy2(bool on);
    void setCeiling2Db(double db);
    void setDialogue2Db(double db);
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
    // `label` names the shape a preset authored ("orbit", "lift") so the
    // object table can print it; hand edits (addObjectKeyframe and friends)
    // clear it - a path someone has nudged is no longer purely the preset.
    Q_INVOKABLE void setObjectPathKeyframes(int objectIndex, const QVariantList& keyframes,
                                            const QString& label = QString());
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
    // Retimes the keyframe at fromS (within the same 1/100 s window) to toS,
    // keeping its position/gain/send - the timeline's drag-to-retime. A key
    // already sitting at toS is replaced, the same same-moment rule
    // addObjectKeyframe applies, so a drag can never stack two cues on one
    // instant.
    Q_INVOKABLE void moveObjectKeyframe(int objectIndex, double fromS, double toS);
    // Shifts EVERY one of an object's keyframes by deltaSeconds at once,
    // clamped so none lands before 0 - the timeline clip-band's "move keys
    // with source" drag modifier (see Main.qml). Doing this as one atomic
    // rewrite rather than N calls to moveObjectKeyframe avoids the ordering
    // hazard a naive per-key shift has: moving keys one at a time in the
    // wrong order can have an early move land on (and replace) a key that
    // was about to move too. A no-op (not even a notify) if the object has
    // no authored path or the shift is zero.
    Q_INVOKABLE void shiftObjectKeyframes(int objectIndex, double deltaSeconds);
    // Where an object sits at timeS: along its authored path if it has one,
    // else its static position, unmoving. What the motion timeline's preview
    // playhead reads so the room plan animates exactly what encodeObjects()
    // will actually place - the same ac3::oba::KeyframePath, not a second
    // interpolation that could disagree with it.
    Q_INVOKABLE [[nodiscard]] QVariantMap evaluateObjectPath(int objectIndex, double timeS) const;
    // Writes every dynamic object's authored path - or, for a path-less
    // object, its static position as a single time-0 keyframe, under the
    // same inverse-root gain/lfe_send law encodeObjects' own fallback uses
    // - to `url` in parse_path_file's exact grammar (see ac3cli's main.cpp:
    // "object_index time_s x y z gain lfe_send" per line), keyed by each
    // object's FLAT channel index - the numbering a plain `atmos-encode`
    // run (every source channel its own object, no assignment concept)
    // addresses, so the file reproduces the GUI's dynamic objects exactly
    // when paired with the same source file on the command line. Bed-pinned
    // channels have no equivalent in atmos-encode's model and are not
    // written. Returns false if the file could not be opened for writing.
    Q_INVOKABLE bool exportObjectPaths(const QUrl& url) const;
    // Starts the audible motion preview: every current object rendered
    // through ac3::oba::AtmosEncoder the same way encodeObjects() would,
    // its 5.1 bed played back live and paced in real time (not run flat-
    // out) through the same MonitorSink path a live session already uses.
    // Refused (silently, the usual convention for a start-a-thing entry
    // point) while busy, outside object mode, or with no objects to play.
    // Reaching the end of the derived programme length stops it the same
    // as an explicit stopMotionPreview() would.
    Q_INVOKABLE void startMotionPreview();
    Q_INVOKABLE void stopMotionPreview();

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
    // A per-source start offset in seconds, clamped to zero or more - all
    // of that source's channels shift together. Encoded as leading silence
    // (a read-shift before the source's own first sample, mirroring the
    // zero-pad-past-its-end every encode loop already applies at the other
    // end - see encodeChannels/encodeObjects/previewPlanMeters), never as a
    // change to the audio itself. The derived programme length (see
    // sourceModel's own doc comment) is max(offset + duration) over every
    // loaded source. Silently ignored for an out-of-range index, the same
    // convention setAssignment uses.
    Q_INVOKABLE void setSourceOffset(int index, double seconds);
    // index 0 (the primary) drops every loaded source and the assignment
    // with it - see sourceModel's own doc comment on why. Removing any
    // other index clears the assignment table rather than trying to shift
    // its rows down: they addressed positions by index, every later
    // source's index just changed, and guessing which old row survives is
    // exactly the kind of silently-maybe-wrong behaviour this surface
    // exists to avoid (plan::Assignment's own doc comment makes the same
    // call for an unassigned channel). Object state is keyed by (source,
    // channel) rather than position, though (see ObjectKey), so it does not
    // share that problem: a non-primary removal drops only the departed
    // source's own objects/keyframes and renumbers every later source's
    // entries down by one, the same shift sourceShapes() itself applies -
    // a surviving source's authored motion is never touched.
    Q_INVOKABLE void removeSource(int index);
    // destToken is whatever assignmentRows already printed for a row, or
    // any token plan::parse_destination accepts - the same vocabulary
    // ac3cli's map= takes, so a GUI selection and a hand-typed command line
    // can never disagree about what a token means. Silently ignored if it
    // does not parse, same convention as toggleExtra/applyChannelPreset.
    Q_INVOKABLE void setAssignment(int sourceIndex, int channel, const QString& destToken);
    // AssignmentPanel's per-row dB trim control: rewrites ONLY that row's
    // Destination::trim_db, keeping whatever kind/location it already has -
    // a slider drag never needs to re-state the destination itself. Clamped
    // to [-24, +24] and snapped to the same tenth-of-a-dB grid parse_
    // destination's own "@" suffix snaps to (see assignment.cpp's
    // snap_trim's doc comment on why a fixed grid, not this function's own
    // copy of it - the two must compute the identical double for mapToken()
    // to always format what the GUI just set). A no-op, silently, on an
    // out-of-range index or a row that is still kUnassigned - "none" has no
    // destination for a trim to ride (Assignment::set erases it outright
    // regardless - see Destination::trim_db's own comment).
    Q_INVOKABLE void setAssignmentTrim(int sourceIndex, int channel, double dbTrim);
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
    // Empty when outputIsFolder() is true - fMP4/CMAF has no single extension.
    Q_INVOKABLE [[nodiscard]] QString outputSuffix() const;
    // True only for fMP4/CMAF: its output is a FOLDER (init segment, media
    // segments, HLS/DASH manifests), not one file. QML uses this to pick a
    // FolderDialog over a FileDialog for the save/record/live destination.
    Q_INVOKABLE [[nodiscard]] bool outputIsFolder() const;
    Q_INVOKABLE void refreshCaptureDevices();
    // Adds deviceIndex to captureDeviceRows (the rail's live device list) at
    // the next free slot - becomes the master if nothing was selected yet,
    // else the slave. A no-op (silently, the usual convention here) for an
    // out-of-range index, a device already selected, or once
    // captureDeviceCapReached is already true - "Add input…" is disabled at
    // that point, but a stale click should not reach around it either.
    Q_INVOKABLE void addCaptureDevice(int deviceIndex);
    // Removes captureDeviceRows[slotIndex]. Removing the master (slot 0)
    // while a slave is selected promotes the slave to master, the same
    // "there is no honest way to guess which one should take its place"
    // reasoning removeSource(0) already applies does NOT hold here (unlike
    // sourceModel, a live device selection carries no per-row authored state
    // to lose) - so unlike removeSource, slot 1 simply becomes slot 0. A
    // no-op for an out-of-range slotIndex.
    Q_INVOKABLE void removeCaptureDevice(int slotIndex);
    Q_INVOKABLE void startRecording(int deviceIndex, const QUrl& url);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void refreshOutputDevices();
    // Whether outputDevices()[index] can bitstream the stream outputPath
    // currently holds (outputIsEac3 decides which capability flag applies) -
    // what the Play button's enabled state reads, so an endpoint that would
    // refuse the stream is greyed out rather than failing after the click.
    Q_INVOKABLE [[nodiscard]] bool outputDeviceCanBitstream(int deviceIndex) const;
    // The general form outputDeviceCanBitstream reads output_eac3_ for:
    // whether outputDevices()[deviceIndex] can bitstream a stream of the
    // given format, independent of what the controller most recently
    // encoded. Two callers need exactly that independence - a run chip's
    // own Play action, checking a device against THAT run's own stored
    // "eac3" field rather than whatever the shared outputIsEac3 currently
    // holds (see runs' own doc comment), and Guided's amp-destination
    // auto-pick (item 30), checking against the PROSPECTIVE plan's format
    // before anything has been encoded at all.
    Q_INVOKABLE [[nodiscard]] bool outputDeviceSupportsFormat(int deviceIndex, bool eac3) const;
    Q_INVOKABLE void playToReceiver(int deviceIndex);
    // The run strip's own Play action (item 27): plays `path` - a FINISHED
    // run's own output, read straight from that run's `path` field rather
    // than output_path_, which only ever holds the MOST RECENT run's -
    // to outputDevices()[deviceIndex], otherwise identical to playToReceiver
    // (same busy_/playing_ gate, same IEC 61937 bursting). A no-op, silently,
    // while anything is already playing or busy, or for an empty path.
    Q_INVOKABLE void playFileToReceiver(const QString& path, int deviceIndex);
    // Snapshots the ac3cli command line QML's own window.cliLine computed
    // for the run about to start, read once by startRun() into that run's
    // "cliLine" field and cleared immediately after - see runs' own doc
    // comment on why this has to be a snapshot rather than something the
    // details popover recomputes live. Called from QML right before every
    // encodeTo/startRecording/startLiveSession call that might open a run
    // entry; harmless to call ahead of one that gets refused, since the
    // NEXT real startRun() always overwrites whatever this last set.
    Q_INVOKABLE void setPendingCliLine(const QString& text);
    // Guided's own amp-destination auto-pick (item 30), threaded through to
    // the run about to start so its finished chip's Play action can reuse
    // the SAME device rather than asking again (item 27) - read once by
    // startRun() into that run's "playDeviceIndex" field and reset to -1
    // immediately after, the same one-shot handoff setPendingCliLine uses.
    // -1 (the default) means "no guided pre-selection" - every entry point
    // other than Guided's own amp-destination encode leaves this alone.
    Q_INVOKABLE void setPendingPlayDevice(int deviceIndex);
    // Restores runs from a previous session - Main.qml's own "reopen the
    // last session" persistence (saveSession/restoreSession), the run
    // strip's counterpart to sourceModel/assignment restoration. `saved` is
    // exactly what runs() itself last printed, JSON round-tripped; a saved
    // "encoding" entry belonged to a process that never finished it (most
    // likely killed outright) and is dropped rather than resurrected as
    // though still running. Restored entries get fresh, THIS process' own
    // ids (continuing after next_run_id_, since their original ids could
    // collide with ones this process goes on to assign) and are appended
    // AFTER whatever runs_ already holds - normally empty this early in
    // startup, so in practice this simply becomes the run strip's initial
    // content, oldest last exactly like the list it was saved from.
    Q_INVOKABLE void restoreRuns(const QVariantList& saved);
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
    // Binds the next free slot in the current live Atmos session's object
    // budget to `captureChannel` (0-based, a device channel index - not an
    // object index). A no-op (silently, the usual convention here) outside a
    // live Atmos session, for a channel outside the device's own count, or
    // when every allocated slot is already bound - "Add an object" refuses
    // rather than growing the budget, since the budget is what JOC baked
    // into the stream at session start (see startLiveSession).
    Q_INVOKABLE void addLiveObject(int captureChannel);
    // Changes which capture channel feeds `slotIndex` - the Live session
    // tab's per-chip reassignment. `captureChannel` < 0 silences the slot
    // (the same "unbound" state an unused allocated slot already has); a
    // valid channel may be reused by more than one slot at once, since
    // nothing here stops two objects panned to different positions sharing
    // one microphone. A no-op outside a live Atmos session or for a
    // slotIndex/captureChannel outside range.
    Q_INVOKABLE void reassignLiveObjectSlot(int slotIndex, int captureChannel);
    // Closes whatever passthrough sink is open (if any) and opens a new one
    // for outputDevices()[receiverDeviceIndex] - or, for a negative index,
    // just closes the current one - WITHOUT restarting capture or encode.
    // Capture keeps running and the take on disk (if any) keeps growing
    // straight through the swap; only the receiver leg blinks, the same
    // "expect a second of silence" renegotiation a fresh session's own
    // first open already carries (liveReconnecting narrates it exactly the
    // same way). Refused with the same text startLiveSession's own
    // passthrough open already uses when the chosen endpoint cannot take
    // the current format. A no-op outside a live session.
    Q_INVOKABLE void switchLiveReceiver(int receiverDeviceIndex);
    // Where a level sits on the meter scale, for the QML that draws the
    // gridline labels. The bars themselves get their positions in
    // channelLevels; this exists so the ticks cannot disagree with them.
    Q_INVOKABLE [[nodiscard]] double meterFraction(double db) const {
        return ac3::analysis::meter_fraction(db, kMeterFloorDb);
    }
    // "48 000" / "7 891" - the mockup's space-grouped integers, offered here
    // so every readout groups digits the same way.
    Q_INVOKABLE [[nodiscard]] QString groupDigits(qint64 value) const;
    // Clears channel `channel`'s latched CLIP indicator - ChannelMeter's own
    // click handler, the only way a latch ever comes back off before the
    // next run-start (see clearClipLatches, the automatic reset every
    // transport-starting entry point already calls). A no-op for an out-of-
    // range channel. See publishLevels' own comment for how the latch itself
    // is set.
    Q_INVOKABLE void clearClipLatch(int channel);

signals:
    void sourceChanged();
    void outputChanged();
    void keepPartialOutputChanged();
    void statusChanged();
    void busyChanged();
    void progressChanged();
    void runsChanged();
    // Session-only, not part of the plan: whether the user has made a real,
    // interactive edit to Loudness/Metadata this session. Guided's loudness
    // contract (see Main.qml) reads this before applying itself and never
    // sets it - only the actual control handlers in LoudnessGroup.qml do -
    // so the contract's own writes can never look like a user edit and
    // block themselves from re-applying, and a genuine user edit is never
    // silently overwritten.
    void loudnessTouchedChanged();
    // See formatDefaultsTouched's own comment.
    void formatDefaultsTouchedChanged();
    // One signal for every encoding decision. They are read together by the
    // summary lines and gate each other besides - the codec decides which
    // layouts exist, which decides whether the tools apply - so splitting them
    // would only invite a binding that misses the change it depended on.
    void planChanged();
    void objectsChanged();
    void routingChanged();
    void captureDevicesChanged();
    void captureDeviceRowsChanged();
    void outputDevicesChanged();
    void playingChanged();
    void recordingChanged();
    void recordedSecondsChanged();
    void layoutChanged();
    void levelsChanged();
    void meteringChanged();
    void encodeFinished(bool ok, const QString& message);
    // A run that was refused before it ever opened a run entry - plan
    // validation, an incomplete assignment, the sixteen-object cap, a
    // capture device that would not open. `reason` is the same text
    // setStatus just showed; the QML raises the failure banner from this,
    // because a status line the run strip has scrolled away is not a home
    // for a refusal (the mockup gives every failure the banner).
    void encodeRefused(const QString& reason);
    void liveActiveChanged();
    void liveStatsChanged();
    void liveReconnectingChanged();
    void liveWavSafetyCopyChanged();
    void motionPreviewActiveChanged();
    void motionPreviewTimeChanged();
    void sourceLevelsChanged();

private:
    struct Source;
    // Object identity: (source index, channel index) into sourceShapes()'s
    // own flat addressing - the same pair Assignment/touched_channels_
    // already key by. object_configs_/object_keyframes_/object_path_labels_
    // hang authored motion off this rather than off an object's position in
    // the current dynamic-object list, so a channel keeps its motion when
    // reassignment or a source's removal changes which position it shows up
    // at (see keyForObjectIndex, and removeSource's own comment). A live
    // session has no file channel to key by; it uses a reserved sentinel
    // source index instead (see keyForObjectIndex).
    using ObjectKey = std::pair<std::size_t, std::size_t>;

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
    // sourceShapes()'s own flat addressing, but each entry is that
    // channel's SOURCE's start offset, in samples at `sample_rate` -
    // encodeChannels/encodeObjects/previewPlanMeters all read this
    // alongside `planes` (in the same flat/repacked order they use) to
    // read-shift a source's audio rather than mutate it.
    [[nodiscard]] std::vector<std::size_t> flatChannelOffsetSamples(
        std::uint32_t sample_rate) const;
    // objectModel()'s own sourceLabel for object `flatIndex` (a concatenated
    // index into sourceShapes(), the same addressing planes/Assignment use):
    // "Ch <n>" with exactly one source loaded - unchanged from what this has
    // always shown - else "<file> ch <n>", the same "<source> ch <n>"
    // phrasing assignmentRows()/unassignedWarnings() already use, so an
    // object and the channel it came from are named the same way everywhere
    // this app names one.
    [[nodiscard]] QString objectSourceLabel(std::size_t flatIndex) const;
    // The group overload: group.size() == 1 is exactly the single-channel
    // case above; a multi-channel objm group instead reads "<file> ch
    // <a>-<b> (mono)", making the fold buildObjectPlane performs for it
    // visible in the label. A contiguous objm group is always within one
    // source (see DestinationKind::kObjectMono's own comment), so the
    // group's first and last channel share one source's label.
    [[nodiscard]] QString objectSourceLabel(const std::vector<std::size_t>& group) const;
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
    // Ensures every currently-dynamic object index (0..object_count_-1) has
    // a config entry, spreading newly-seen (source, channel) identities out
    // along x instead of defaulting them all onto the same overlapping point
    // (the design brief's own complaint about the single-point-plus-spread
    // model). Called wherever object_count_ is set. An identity that already
    // has an entry - the common case, an object that survived whatever just
    // changed - keeps it untouched; nothing is ever pruned here, so a
    // channel's motion sits dormant rather than lost while it is not
    // currently an object (unassigned, pinned to a bed position, or its
    // source briefly absent), and reappears if it becomes one again. See
    // keyForObjectIndex for how an index resolves to that identity.
    void refreshObjectConfigs();
    // The shared lookup addObjectKeyframe/removeObjectKeyframe/
    // objectKeyframes/evaluateObjectPath all build on: object_keyframes_'s
    // entry for this index's (source, channel) identity, sorted by time, or
    // empty if it has none.
    [[nodiscard]] std::vector<ac3::oba::Keyframe> sortedKeyframes(int objectIndex) const;
    // flatIndex's (source, channel) pair in sourceShapes()'s own flat
    // addressing - the same loop objectSourceLabel uses to name it.
    [[nodiscard]] ObjectKey sourceChannelForFlatIndex(std::size_t flatIndex) const;
    // Where object_configs_/object_keyframes_/object_path_labels_ keep
    // objectIndex's (position in the current dynamic-object list) data:
    // its underlying (source, channel) identity via dynamicObjectChannels(),
    // or - while a live session has resized these over the capture device's
    // own channel count instead of a loaded file's (see startLiveSession and
    // live_object_backup_) - a reserved sentinel source index paired with
    // objectIndex itself, since a live device channel has no (source,
    // channel) of its own to key by. nullopt for an index outside the
    // current object count.
    [[nodiscard]] std::optional<ObjectKey> keyForObjectIndex(int objectIndex) const;

    struct ObjectConfig;
    struct LiveOutputWriters;
    // Opens whatever runLiveSession will need to write incrementally - the
    // take itself (or, for Matroska, its elementary-stream spool - see
    // LiveOutputWriters' own comment) and the optional raw-WAV safety copy -
    // on the GUI thread, before anything is marked live, so a bad
    // destination refuses the session the same way a bad device choice
    // already does rather than surfacing as a mid-session failure. Returns
    // null (having already called setStatus/emitted encodeRefused itself,
    // the same convention every other refusal in startLiveSession follows)
    // on any open failure; a null return with write_to_disk false is not a
    // failure, just "there is nothing to open" - runLiveSession itself reads
    // write_to_disk again to tell the two apart. A unique_ptr rather than
    // a plain return-by-value: LiveOutputWriters is only forward-declared
    // here (its definition, alongside ac3::io::WavStreamWriter, has no
    // business in this header), and unique_ptr is the one smart pointer that
    // tolerates an incomplete type at the declaration site.
    [[nodiscard]] std::unique_ptr<LiveOutputWriters> openLiveOutputWriters(
        const QString& path, bool write_to_disk, const ac3::capture::DeviceInfo& device);
    // The live session worker. One function for both channel and object mode
    // (mirrors ac3cli's own `live` command, which combines them the same way)
    // rather than split like encodeChannels/encodeObjects: almost everything
    // here - capture, monitor, passthrough, the disk-write, the live counters
    // - is identical between the two, and only the "turn source samples into
    // one encoded unit" step differs. `writers` is already open (or, with
    // write_to_disk false, null) - see openLiveOutputWriters.
    // `device2` is the slave, when a second device was selected and opened
    // successfully - nullopt for an ordinary single-device session, which
    // then behaves exactly as it always has. See docs/gui/live-session.md
    // for the clock-master model this implements.
    void runLiveSession(ac3::capture::DeviceInfo device,
                        std::optional<ac3::capture::DeviceInfo> device2, bool monitor,
                        bool passthrough, bool write_to_disk, QString file_path,
                        std::unique_ptr<LiveOutputWriters> writers);
    // A snapshot of object_configs_ that setObjectPosition/setObjectLfeSend
    // also keep current, guarded by live_object_mutex_ - the one piece of
    // state the live worker thread and the GUI thread genuinely touch
    // concurrently (dragging the room while a live Atmos session runs).
    [[nodiscard]] std::vector<ObjectConfig> liveObjectSnapshot() const;
    // Slot -> capture-channel bindings for the current live Atmos session's
    // object budget, guarded the same way and for the same reason as
    // liveObjectSnapshot (addLiveObject/reassignLiveObjectSlot write it from
    // the GUI thread, the worker reads it once per frame). Empty outside a
    // live Atmos session.
    [[nodiscard]] std::vector<int> liveSlotChannels() const;

    // Writes an elementary stream, or muxes Matroska/MP4/MPEG-TS, or wraps
    // fMP4/CMAF (a folder of files - see outputIsFolder()), according to the
    // chosen container. Returns an empty string on success and the reason
    // otherwise.
    [[nodiscard]] QString writeOutput(const QString& path,
                                      const std::vector<std::vector<std::byte>>& frames,
                                      std::uint32_t sample_rate, int channels) const;

    void setStatus(const QString& text);
    void setBusy(bool busy);
    // Adds a new "encoding" entry to runs_ and remembers its id, so the
    // encodeFinished this run eventually emits (there are several call
    // sites; a run is always started right after setBusy(true) rather than
    // duplicated at each one) knows which entry to settle. `durationText`
    // overrides the source-derived length ("live" for captures, whose length
    // nobody knows at start); `label` overrides the filename for a session
    // that writes no file at all; `forceCbr` keeps a live session's rate
    // text honest (runLiveSession drops VBR unconditionally).
    void startRun(const QString& path, const QString& durationText = QString(),
                  const QString& label = QString(), bool forceCbr = false);
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
    // One entry per DYNAMIC object in object mode, each entry the group of
    // flat channel indices (the sourceShapes()/Assignment addressing) that
    // fold together into it: size 1 for an ordinary "obj" channel (or every
    // channel, while nothing is explicitly assigned - unchanged from what
    // this has always done), or every channel of one contiguous "objm"
    // range (see DestinationKind::kObjectMono) folded to a single mono
    // object. Order is flat order (source, then channel), which is object-
    // index order - dynamicObjectChannels().size() is still the dynamic
    // object COUNT regardless of any internal grouping, so every caller
    // that only ever read that size (recomputeObjectCount, encodeTo's
    // fifteen-object budget check, ...) needs no change at all; a caller
    // that used to read one flat index per object now reads
    // group.front() for that object's IDENTITY (what keyForObjectIndex
    // keys authored motion by - a group's shape never changing mid-file is
    // what keeps that stable) or the whole group, via buildObjectPlane, for
    // its actual audio.
    [[nodiscard]] std::vector<std::vector<std::size_t>> dynamicObjectChannels() const;
    // Builds one dynamic object's plane: `group`'s own content (linear-gain
    // trimmed by that channel's Destination::trim_db - see assignment.hpp)
    // for an ordinary single-channel group, or the equal-weight fold of
    // every (also trimmed) channel in a multi-channel objm group - sum *
    // 1/n, so several full-range channels folded together do not sum past
    // what one channel alone would peak at (documented alongside
    // DestinationKind::kObjectMono). `planes` is in sourceShapes()'s own
    // flat order, already offset-shifted where the caller does that
    // (encodeObjects'/startMotionPreview's own `planes` construction) -
    // shared by both so they can never fold or trim a group differently.
    [[nodiscard]] std::vector<float> buildObjectPlane(
        const std::vector<std::size_t>& group,
        const std::vector<std::vector<float>>& planes) const;
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
    // Each channel's CLIP is latched here (clip_latched_[ch] ||=
    // levels[ch].clipped) rather than shown raw - the published `clipped`
    // field is always the latched value, so QML never has to know latching
    // exists (see clearClipLatch/clearClipLatches for the only two ways a
    // latch ever comes back down).
    void publishLevels(std::span<const ac3::analysis::ChannelLevel> levels);
    // The same, built from a meter's exact whole-run statistics rather than
    // its ballistics: what a finished encode or a freshly loaded file should
    // leave on the display.
    void publishSummary(const ac3::analysis::LevelMeter& meter);
    // Zeroes every channel's latched CLIP flag (not the ballistic levels
    // themselves) - called once at the start of every real transport (see
    // encodeChannels/encodeObjects/runLiveSession/startRecording/
    // startMotionPreview's own call sites), never from setLayout/
    // previewPlanMeters, so idly browsing the assignment table or Format tab
    // never clears a latch a completed run actually earned.
    void clearClipLatches();
    // sourceLevels' reset half - a fresh floor-filled list sized to the
    // CURRENT source count, published synchronously and unconditionally at
    // the top of previewPlanMeters() the same way setLayout's own
    // publishLevels(default) starts channelLevels silent, so a stale
    // reading never sits under a source row that just changed (added,
    // removed, or reordered by a primary swap).
    void resetSourceLevels();
    // sourceLevels' real half - one whole-programme peak/RMS reduction per
    // loaded source, pooling every one of that source's own channels into a
    // single ac3::analysis::ChannelSummary (not per-channel: a rail row is
    // one pip, not one per channel) - published by previewPlanMeters'
    // background pass once it lands, the exact same async-then-overwrite
    // shape channelLevels already follows via publishLevels.
    void publishSourceLevels(std::span<const ac3::analysis::ChannelSummary> levels);

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
    // See loudnessTouchedChanged()'s own comment.
    bool loudness_touched_ = false;
    // See formatDefaultsTouched's own comment.
    bool format_defaults_touched_ = false;
    // setPendingCliLine's one-shot handoff into the next startRun() call -
    // see its own comment.
    QString pending_cli_line_;
    // setPendingPlayDevice's one-shot handoff into the next startRun() call
    // - see its own comment. -1 is "no pre-selection", the value every run
    // not opened through Guided's amp destination keeps.
    int pending_play_device_ = -1;
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
    // Ch2's own DRC/heavy state, same shape and same reason held apart from
    // meta_.drc2/meta_.heavy2 as the programme-1 fields above.
    int drc2_index_ = 0;
    double ceiling2_db_ = -0.5;
    double dialogue2_db_ = -20.0;

    bool atmos_enabled_ = false;
    int object_count_ = 0;
    int selected_object_index_ = 0;
    // One static position per object - independent now, not a shared point
    // plus a spread fan-out. Populated (and freshly spread out, so a loaded
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
    // Keyed by ObjectKey (source, channel), not by an object's position in
    // the current dynamic-object list - see ObjectKey's own comment. An
    // entry for a channel that is not currently an object (unassigned,
    // pinned to the bed, or briefly absent mid-edit) simply sits unread
    // until keyForObjectIndex resolves something to it again.
    std::map<ObjectKey, ObjectConfig> object_configs_;
    // Authored motion, keyed the same way. An identity absent here (the
    // common case) falls back to the object's static ObjectConfig placement
    // in encodeObjects, held constant for the whole file.
    std::map<ObjectKey, std::vector<ac3::oba::Keyframe>> object_keyframes_;
    // The preset name that authored an object's path ("orbit", "lift"),
    // absent for hand-authored or hand-edited paths - what the object
    // table's Path column prints instead of a bare "path". Kept strictly in
    // step with object_keyframes_: every place that clears or hand-edits a
    // path clears its label too.
    std::map<ObjectKey, QString> object_path_labels_;
    // A snapshot of object_configs_/object_keyframes_/selected_object_index_
    // as they stood before a live Atmos session resized them to the CAPTURE
    // DEVICE's channel count instead of a loaded file's (see
    // startLiveSession's own comment on why that resize happens at all).
    // Restored once the session ends (runLiveSession's completion callback),
    // so an unrelated live excursion can never permanently clobber authored
    // object placements/motion a loaded file already had. nullopt means
    // nothing needs restoring - no session has resized anything yet, or the
    // device's channel count already matched and nothing was touched. Its
    // presence is also what keyForObjectIndex reads to know a live session
    // has objects keyed by the device-channel sentinel right now rather
    // than by a loaded file's (source, channel) identity.
    struct LiveObjectBackup {
        int count = 0;
        std::map<ObjectKey, ObjectConfig> configs;
        std::map<ObjectKey, std::vector<ac3::oba::Keyframe>> keyframes;
        std::map<ObjectKey, QString> path_labels;
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
    // What outputPath holds - see the outputIsEac3 property. Snapshotted by
    // encodeTo/startRecording alongside output_path_ itself.
    bool output_eac3_ = false;
    QStringList capture_devices_;
    QStringList output_devices_;
    std::vector<ac3::capture::DeviceInfo> devices_;
    std::vector<ac3::sinks::RenderDeviceInfo> outputs_;
    // captureDeviceRows' own selection - indices into devices_, size 0..2,
    // row 0 the master. Mutated only by addCaptureDevice/removeCaptureDevice
    // and clamped by refreshCaptureDevices when a device disappears; a
    // running session does not touch this, so it survives a stop/restart
    // (switchLiveLayout's own restart) without needing to be threaded
    // through LiveSessionRequest.
    std::vector<int> live_selected_devices_;

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
    // One flag per coded channel, parallel to channel_levels_ - see
    // publishLevels' own comment on how this OR's into the `clipped` field
    // it publishes, and clearClipLatch/clearClipLatches for the only two
    // ways a latch clears. Resized (grow-or-shrink, never touching a
    // surviving element's value) by publishLevels alongside channel_levels_
    // itself; actually zeroed only by clearClipLatches.
    std::vector<bool> clip_latched_;
    QVariantMap soundfield_;
    // sourceLevels' storage - see that property's own doc comment.
    QVariantList source_levels_;

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
    // Per-source start offsets in seconds, parallel to source_/
    // extra_sources_ (not stored ON Source itself: Source is shared with
    // previewPlanMeters' background worker on the strength of never being
    // mutated after construction - see source_'s own comment - and an
    // offset is exactly the kind of thing a drag mutates in place). Kept in
    // step with extra_sources_ by every add/remove site; see
    // flatChannelOffsetSamples for how these turn into a read-shift.
    double source_offset_seconds_ = 0.0;
    std::vector<double> extra_source_offsets_seconds_;
    // Parallel to extra_sources_, kept in step with it by every add/remove
    // site the same way extra_source_offsets_seconds_ already is: nullopt
    // for a source whose rate matched the primary's at load, else the rate
    // it actually carried on disk BEFORE addSourceFile resampled it to the
    // primary's - what sourceModel's resampleLabel reads to print
    // "44.1→48 k". The primary itself has no equivalent field: it is never
    // resampled (every OTHER source matches IT, not the reverse).
    std::vector<std::optional<std::uint32_t>> extra_source_original_rates_;
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
    // Pre-flight only - startLiveSession reads this once, at session start,
    // the same way it already reads keepPartialOutput; changing it mid-
    // session has no effect on a copy already open.
    bool live_wav_safety_copy_ = false;
    QString live_receiver_plan_text_;
    bool live_gap_ = false;
    QString live_capture_detail_;
    QString live_receiver_name_;
    bool live_wanted_passthrough_ = false;
    bool live_receiver_eac3_ = false;
    bool live_reconnecting_ = false;
    double live_running_seconds_ = 0.0;
    qint64 live_frames_encoded_ = 0;
    qint64 live_frames_dropped_ = 0;
    quint64 live_underruns_ = 0;
    double live_latency_ms_ = 0.0;
    bool live_latency_measured_ = false;
    // The current live session's own device channel count - what
    // addLiveObject/reassignLiveObjectSlot validate a capture-channel
    // argument against, without either needing to reach into live_capture_
    // (worker-owned once the session is running) from the GUI thread. Reset
    // to 0 alongside every other live_* flag when a session ends.
    int live_device_channels_ = 0;
    // ---- two-device capture ---------------------------------------------
    bool live_second_device_active_ = false;
    QString live_second_device_name_;
    double live_drift_ppm_ = 0.0;
    // ---- parallel downmix receiver leg -----------------------------------
    bool live_downmix_leg_ = false;
    // Genuinely shared with the live worker thread (dragging the Live
    // session's room, or the Objects tab's, while a live Atmos session is
    // running): every read and write goes through live_object_mutex_.
    mutable std::mutex live_object_mutex_;
    std::vector<ObjectConfig> live_object_snapshot_;
    // Slot -> capture-channel bindings (see liveSlotChannels' own comment);
    // -1 marks an allocated-but-unbound slot, which the worker encodes as
    // silence. Guarded by live_object_mutex_ alongside live_object_snapshot_
    // above, for the same reason.
    std::vector<int> live_slot_channels_;
    // Opened and (via the worker's final invokeMethod) closed on the GUI
    // thread, matching capture_'s own convention - only buffer()/submit()/
    // stats() are called from the worker while a session runs.
    std::unique_ptr<ac3::capture::Capture> live_capture_;
    // The slave device, when a two-device session opened one - same
    // GUI-thread-owns-open/close, worker-thread-only-reads convention as
    // live_capture_ itself. Null for an ordinary single-device session.
    std::unique_ptr<ac3::capture::Capture> live_capture2_;
    std::unique_ptr<ac3::sinks::MonitorSink> live_monitor_sink_;
    std::unique_ptr<ac3::sinks::PassthroughSink> live_passthrough_sink_;
    // switchLiveReceiver's handoff to the worker thread: the GUI thread
    // writes a request here, the worker thread claims it (and clears it)
    // once per outer-loop iteration and does the actual close-old/open-new
    // itself - the only thread that ever calls submit() on
    // live_passthrough_sink_, so performing the swap there too means there
    // is never a window where another thread could be mid-submit on a sink
    // this is about to destroy. A full RenderDeviceInfo rather than just an
    // index, resolved from outputs_ on the GUI thread at request time, so
    // the worker never has to touch outputs_ (GUI-thread-owned state) itself.
    struct PendingReceiverSwitch {
        bool want_passthrough = false;
        ac3::sinks::RenderDeviceInfo receiver;  // valid only if want_passthrough
    };
    std::mutex live_receiver_switch_mutex_;
    std::optional<PendingReceiverSwitch> live_receiver_switch_request_;

    // ---- Objects tab's audible motion preview ---------------------------
    // stop_motion_preview_ is the only piece of this the worker thread
    // reads; everything else it only ever touches through a
    // QMetaObject::invokeMethod back onto the GUI thread - the same
    // discipline runLiveSession's worker already follows.
    std::atomic_bool stop_motion_preview_{false};
    bool motion_preview_active_ = false;
    double motion_preview_time_ = 0.0;
    // Opened at startMotionPreview, closed by the worker's own completion
    // callback - matches live_monitor_sink_'s open-on-GUI-thread/submit-
    // from-worker convention. A dedicated sink rather than reusing
    // live_monitor_sink_: a motion preview and a live session are mutually
    // exclusive (busy_ already guards that), but sharing one member would
    // tie this feature's lifecycle to liveActive's own signals for no
    // reason.
    std::unique_ptr<ac3::sinks::MonitorSink> motion_preview_monitor_sink_;
};
