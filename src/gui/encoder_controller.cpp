#include "encoder_controller.hpp"

#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>

#include <iterator>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/spatial/spatial.hpp"
#include "matroska/matroska.hpp"

namespace plan = ac3::plan;

namespace {

// AC-3 accepts only these three rates (A/52 Table 5.6).
std::optional<ac3::SampleRate> to_sample_rate(std::uint32_t hz) {
    switch (hz) {
        case 48000: return ac3::SampleRate::k48000;
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        default: return std::nullopt;
    }
}

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// Live meters redraw no faster than this. A file encodes far quicker than it
// plays, so without a wall-clock throttle a two-minute track would fire tens
// of thousands of property updates the display could never show.
constexpr auto kPublishInterval = std::chrono::milliseconds(33);

// Where the container choice sits in the combo box; index 0 is the bare
// elementary stream, which is everything this is not.
constexpr int kContainerMatroska = 1;

// The order drcNames() lists the profiles in, after its "none" entry.
constexpr std::array<ac3::meta::ProfileId, 5> kDrcProfiles = {
    ac3::meta::ProfileId::kFilmStandard, ac3::meta::ProfileId::kFilmLight,
    ac3::meta::ProfileId::kMusicStandard, ac3::meta::ProfileId::kMusicLight,
    ac3::meta::ProfileId::kSpeech};

// ---------------------------------------------------------------------------
// The channel model's two tiers. Tier 1 (bed) is Table 5.8's seven speaker
// shapes, always all seven regardless of codec - AC-3 disables only the
// extras (see EncoderController::extrasLocked). Tier 2 (extras) is additive
// Table E2.5 locations on top of the bed.
//
// The handoff's own extras table names three ceiling pairs (front/middle/
// rear). Checked directly against A/52-2018 Table E2.5 (10008-10033 in the
// spec text): there are only TWO ceiling pairs at all - Vhl/Vhr and Lts/Rts -
// plus two unpaired height locations (Vhc, Ts) the handoff's curated list
// does not surface either. "Ceiling middle" is not a real chanmap bit; it is
// dropped here rather than invented. eac3_tables.hpp's own Location enum and
// its spec-cited static_asserts already agreed with this before it was
// double-checked against the spec text directly, which is the point of
// checking rather than trusting a summary.
// ---------------------------------------------------------------------------

struct BedInfo {
    ac3::Acmod acmod;
    const char* id;  // matches the handoff's own ids: "1/0" .. "3/2"
};

// In the handoff's own display order.
constexpr std::array<BedInfo, 7> kBeds{{
    {ac3::Acmod::k1_0, "1/0"},
    {ac3::Acmod::k2_0, "2/0"},
    {ac3::Acmod::k3_0, "3/0"},
    {ac3::Acmod::k2_1, "2/1"},
    {ac3::Acmod::k3_1, "3/1"},
    {ac3::Acmod::k2_2, "2/2"},
    {ac3::Acmod::k3_2, "3/2"},
}};

struct ExtraInfo {
    const char* id;
    const char* label;
    std::uint16_t bits;
};

constexpr std::array<ExtraInfo, 5> kExtras{{
    {"wide", "Front wide", ac3::eac3::chanmap::kLwRw},
    {"rear", "Rear surround", ac3::eac3::chanmap::kLrsRrs},
    {"topf", "Ceiling front", ac3::eac3::chanmap::kVhlVhr},
    {"topr", "Ceiling rear", ac3::eac3::chanmap::kLtsRts},
    {"lfe2", "Second LFE", ac3::eac3::chanmap::kLfe2},
}};

// Space-joined location names for a bed's own full-bandwidth channels, e.g.
// "L C R Ls Rs" for 3/2 - what the bed button shows beneath its id.
QString bed_channel_names(ac3::Acmod acmod) {
    QStringList names;
    for (const auto location : ac3::eac3::chanmap::expand(
             ac3::eac3::chanmap::acmod_map(acmod, false))) {
        names.append(to_qstring(ac3::eac3::chanmap::name(location)));
    }
    return names.join(QStringLiteral(" "));
}

}  // namespace

struct EncoderController::Source {
    ac3::io::WavData wav;
};

EncoderController::EncoderController(QObject* parent) : QObject(parent) {
    refreshCaptureDevices();
    refreshOutputDevices();
    refreshRouting();
}

EncoderController::~EncoderController() = default;

// ---------------------------------------------------------------------------
// Choices. Every list here is built from ac3::plan or ac3::meta rather than
// typed out, so the GUI cannot offer something the command line does not take
// or spell a layout differently from the way the parser reads it.
// ---------------------------------------------------------------------------

QStringList EncoderController::codecNames() const {
    return {to_qstring(plan::codec_label(plan::Codec::kAc3)),
            to_qstring(plan::codec_label(plan::Codec::kEac3))};
}

QStringList EncoderController::containerNames() const {
    return {QStringLiteral("Elementary stream"), QStringLiteral("Matroska (.mkv)")};
}

int EncoderController::bedIndex() const {
    for (std::size_t i = 0; i < kBeds.size(); ++i) {
        if (kBeds[i].acmod == bed_acmod_) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

QVariantList EncoderController::bedChoices() const {
    QVariantList out;
    for (const auto& bed : kBeds) {
        QVariantMap row;
        row[QStringLiteral("id")] = QString::fromLatin1(bed.id);
        row[QStringLiteral("channels")] = bed_channel_names(bed.acmod);
        out.append(row);
    }
    return out;
}

QVariantList EncoderController::extrasModel() const {
    QVariantList out;
    const bool locked = extrasLocked();
    const auto bed_mask = ac3::eac3::chanmap::acmod_map(bed_acmod_, bed_lfe_);
    const QString lock_reason = atmos_enabled_ ? QStringLiteral("fixed by object mode")
                                               : QStringLiteral("Dolby Digital Plus only");

    for (const auto& extra : kExtras) {
        const bool checked = (extras_mask_ & extra.bits) != 0;
        const auto tentative = static_cast<std::uint16_t>(
            checked ? extras_mask_ & ~extra.bits : extras_mask_ | extra.bits);
        // The single general validity check every control here uses, so the
        // picker can never express a combination chanmap::allocate() would
        // then refuse: over the 16-channel ceiling, no Table 5.8 bed fits (not
        // reachable here, the bed is always one), or - the case that is
        // reachable - an LFE2 left with no full-bandwidth companion once its
        // last co-selected extra is the one being unticked.
        const auto result =
            ac3::eac3::chanmap::allocate(static_cast<std::uint16_t>(bed_mask | tentative));

        QString reason;
        if (locked) {
            reason = lock_reason;
        } else if (!result) {
            reason = checked ? QStringLiteral("another extra needs this one")
                             : to_qstring(ac3::eac3::chanmap::describe(result.error()));
        }

        QVariantMap row;
        row[QStringLiteral("id")] = QString::fromLatin1(extra.id);
        row[QStringLiteral("label")] = QString::fromLatin1(extra.label);
        row[QStringLiteral("channels")] = ac3::eac3::chanmap::channel_count(extra.bits);
        row[QStringLiteral("checked")] = checked;
        row[QStringLiteral("enabled")] = !locked && result.has_value();
        row[QStringLiteral("reason")] = reason;
        out.append(row);
    }
    return out;
}

QString EncoderController::channelShapeName() const {
    using ac3::eac3::chanmap::Location;
    int ear = 0;
    int lfe_count = 0;
    int ceiling = 0;
    for (const auto location : ac3::eac3::chanmap::expand(currentLocationMask())) {
        switch (location) {
            case Location::kLfe:
            case Location::kLfe2:
                ++lfe_count;
                break;
            case Location::kTs:
            case Location::kVhl:
            case Location::kVhr:
            case Location::kVhc:
            case Location::kLts:
            case Location::kRts:
                ++ceiling;
                break;
            default:
                ++ear;
                break;
        }
    }
    QString name = QStringLiteral("%1.%2").arg(ear).arg(lfe_count);
    if (ceiling > 0) {
        name += QStringLiteral(".%1").arg(ceiling);
    }
    return name;
}

int EncoderController::channelBudgetUsed() const {
    return ac3::eac3::chanmap::channel_count(currentLocationMask());
}

QString EncoderController::channelLocationsText() const {
    return to_qstring(plan::format_channels(currentLocationMask()));
}

QString EncoderController::layoutDetail() const {
    if (atmos_enabled_) {
        return QStringLiteral("5.1 bed · JOC + OAMD · objects carry the height");
    }
    const auto cp = effectiveChannelPlan();
    const auto rendered = plan::rendered_channel_count(cp);
    const auto transmitted = static_cast<int>(plan::coded_channels(cp).size());
    const auto dependents = static_cast<int>(cp.dependents.size());
    // Whether there is only the independent substream is what "one substream"
    // actually means - dependents == 0, not transmitted == rendered. The two
    // used to coincide when every dependent came from a hand-picked LayoutId
    // (7.1's k71Rear duplicates the bed's Ls/Rs into its dependent, so wider-
    // than-bed always meant transmitted > rendered too), but the general
    // extras model doesn't duplicate anything: a plain "rear" extra alone can
    // need a real dependent while still transmitting exactly what it renders.
    if (dependents == 0) {
        return QStringLiteral("%1 channel%2, one substream")
            .arg(rendered)
            .arg(rendered == 1 ? QString() : QStringLiteral("s"));
    }
    // Where the two differ, say why: a dependent that REPLACES a bed channel
    // spends coded channels a listener never counts.
    return QStringLiteral("%1 speakers from %2 coded channels · %3 dependent substream%4")
        .arg(rendered)
        .arg(transmitted)
        .arg(dependents)
        .arg(dependents == 1 ? QString() : QStringLiteral("s"));
}

QVariantList EncoderController::bitrates() const {
    QVariantList out;
    // AC-3 indexes Table 5.18 and cannot express anything else. E-AC-3 signals
    // frmsiz directly, so the same list is a convenience there rather than a
    // constraint - but offering the same rungs keeps an A/B honest.
    for (const auto kbps : ac3::kBitratesKbps) {
        if (kbps >= 96) {
            out.append(static_cast<int>(kbps));
        }
    }
    return out;
}

QString EncoderController::toolsToken() const {
    return to_qstring(plan::format_tools(tools_));
}

QStringList EncoderController::drcNames() const {
    QStringList names{QStringLiteral("none")};
    for (const auto id : kDrcProfiles) {
        names.append(to_qstring(ac3::meta::profile_name(id)));
    }
    return names;
}

QStringList EncoderController::cmixNames() const {
    return {QStringLiteral("-3 dB"), QStringLiteral("-4.5 dB"), QStringLiteral("-6 dB")};
}

QStringList EncoderController::surmixNames() const {
    return {QStringLiteral("-3 dB"), QStringLiteral("-6 dB"), QStringLiteral("off")};
}

QStringList EncoderController::dmixNames() const {
    return {QStringLiteral("not indicated"), QStringLiteral("Lt/Rt"), QStringLiteral("Lo/Ro")};
}

// ---------------------------------------------------------------------------
// Setters. Each one settles its own field and then re-derives everything that
// depends on it, because the choices gate each other: a codec change can
// invalidate the layout, and a layout change can change what the source has
// to be rendered into.
// ---------------------------------------------------------------------------

void EncoderController::setBitrateKbps(int kbps) {
    if (kbps == bitrate_kbps_ || busy_) {
        return;
    }
    bitrate_kbps_ = kbps;
    emit planChanged();
}

void EncoderController::setCodecIndex(int index) {
    const auto codec = index == 1 ? plan::Codec::kEac3 : plan::Codec::kAc3;
    if (codec == codec_ || busy_) {
        return;
    }
    codec_ = codec;
    // AC-3 has no dependent substreams at all, so extras that needed one have
    // to go somewhere: dropping them is what the extras lock itself would
    // have refused going forward, and leaving them set would silently build a
    // plan validate() then rejects at encode time instead of here.
    if (codec_ == plan::Codec::kAc3) {
        extras_mask_ = 0;
    }
    emit planChanged();
    emit outputChanged();
    refreshRouting();
}

void EncoderController::setBedIndex(int index) {
    if (busy_ || atmos_enabled_ || index < 0 || index >= static_cast<int>(kBeds.size())) {
        return;
    }
    const auto acmod = kBeds[static_cast<std::size_t>(index)].acmod;
    if (acmod == bed_acmod_) {
        return;
    }
    bed_acmod_ = acmod;
    emit planChanged();
    refreshRouting();
}

void EncoderController::setBedLfe(bool on) {
    if (busy_ || atmos_enabled_ || on == bed_lfe_) {
        return;
    }
    bed_lfe_ = on;
    emit planChanged();
    refreshRouting();
}

void EncoderController::toggleExtra(const QString& id) {
    if (busy_ || extrasLocked()) {
        return;
    }
    for (const auto& extra : kExtras) {
        if (id != QLatin1String(extra.id)) {
            continue;
        }
        const bool checked = (extras_mask_ & extra.bits) != 0;
        const auto tentative = static_cast<std::uint16_t>(
            checked ? extras_mask_ & ~extra.bits : extras_mask_ | extra.bits);
        const auto bed_mask = ac3::eac3::chanmap::acmod_map(bed_acmod_, bed_lfe_);
        // Refused rather than truncated: over budget, or - unticking an
        // extra an LFE2 was sharing its substream with - an orphaned LFE2,
        // are both "this does not fit", not "fit what you can".
        if (!ac3::eac3::chanmap::allocate(static_cast<std::uint16_t>(bed_mask | tentative))) {
            return;
        }
        extras_mask_ = tentative;
        emit planChanged();
        refreshRouting();
        return;
    }
}

void EncoderController::applyChannelPreset(const QString& name) {
    if (busy_ || atmos_enabled_) {
        return;
    }
    struct Preset {
        const char* name;
        bool lfe;
        std::uint16_t extras;
    };
    // Bed is always 3/2 here: every named preset in the handoff's own list is
    // built on the widest bed. A preset is a starting point for the general
    // model, not a separate one - see the file comment on kBeds/kExtras for
    // why this goes through chanmap::allocate() rather than the legacy
    // LayoutId table (fewer transmitted channels for 7.1/7.1.4 than the old
    // hand-picked k71Rear/kTopQuad dependents, same rendered speakers).
    static constexpr std::array<Preset, 5> kPresets{{
        {"5.1", true, 0},
        {"7.1", true, ac3::eac3::chanmap::kLrsRrs},
        {"5.1.4", true,
         static_cast<std::uint16_t>(ac3::eac3::chanmap::kVhlVhr | ac3::eac3::chanmap::kLtsRts)},
        {"7.1.4", true,
         static_cast<std::uint16_t>(ac3::eac3::chanmap::kLrsRrs | ac3::eac3::chanmap::kVhlVhr |
                                    ac3::eac3::chanmap::kLtsRts)},
        {"5.2", true, ac3::eac3::chanmap::kLfe2},
    }};
    for (const auto& preset : kPresets) {
        if (name != QLatin1String(preset.name)) {
            continue;
        }
        // A preset needing extras has to bring E-AC-3 with it, the same way a
        // manual tick would otherwise find the row locked and refuse.
        if (preset.extras != 0 && codec_ == plan::Codec::kAc3) {
            codec_ = plan::Codec::kEac3;
        }
        bed_acmod_ = ac3::Acmod::k3_2;
        bed_lfe_ = preset.lfe;
        extras_mask_ = preset.extras;
        emit planChanged();
        emit outputChanged();
        refreshRouting();
        return;
    }
}

void EncoderController::setContainerIndex(int index) {
    if (index == container_index_ || busy_) {
        return;
    }
    container_index_ = index;
    emit planChanged();
    emit outputChanged();
}

void EncoderController::setCoupling(bool on) {
    if (on == tools_.coupling) {
        return;
    }
    tools_.coupling = on;
    emit planChanged();
}

void EncoderController::setSpx(bool on) {
    if (on == tools_.spx) {
        return;
    }
    tools_.spx = on;
    emit planChanged();
}

void EncoderController::setAht(bool on) {
    if (on == tools_.aht) {
        return;
    }
    tools_.aht = on;
    emit planChanged();
}

void EncoderController::setCplBegf(int value) {
    const int clamped = std::clamp(value, -1, 15);
    if (clamped == tools_.cplbegf) {
        return;
    }
    tools_.cplbegf = clamped;
    emit planChanged();
}

void EncoderController::setSpxBegf(int value) {
    const int clamped = std::clamp(value, -1, 7);
    if (clamped == tools_.spxbegf) {
        return;
    }
    tools_.spxbegf = clamped;
    emit planChanged();
}

void EncoderController::setGaqMode(int value) {
    const int clamped = std::clamp(value, -1, 3);
    if (clamped == tools_.gaqmod) {
        return;
    }
    tools_.gaqmod = clamped;
    emit planChanged();
}

void EncoderController::setSpxAtten(bool on) {
    if (on == tools_.spx_atten) {
        return;
    }
    tools_.spx_atten = on;
    emit planChanged();
}

void EncoderController::setDrcIndex(int index) {
    const int clamped = std::clamp(index, 0, static_cast<int>(kDrcProfiles.size()));
    if (clamped == drc_index_) {
        return;
    }
    drc_index_ = clamped;
    meta_.drc = clamped == 0
                    ? std::nullopt
                    : std::optional{ac3::meta::profile(
                          kDrcProfiles[static_cast<std::size_t>(clamped - 1)])};
    emit planChanged();
}

void EncoderController::setHeavy(bool on) {
    if (on == meta_.heavy.has_value()) {
        return;
    }
    if (on) {
        meta_.heavy = ac3::meta::HeavyConfig{.dialogue_target_dbfs = dialogue_db_,
                                             .peak_ceiling_dbfs = ceiling_db_};
    } else {
        meta_.heavy.reset();
    }
    emit planChanged();
}

void EncoderController::setCeilingDb(double db) {
    if (db == ceiling_db_) {
        return;
    }
    ceiling_db_ = db;
    if (meta_.heavy) {
        meta_.heavy->peak_ceiling_dbfs = db;
    }
    emit planChanged();
}

void EncoderController::setDialogueDb(double db) {
    if (db == dialogue_db_) {
        return;
    }
    dialogue_db_ = db;
    if (meta_.heavy) {
        meta_.heavy->dialogue_target_dbfs = db;
    }
    emit planChanged();
}

void EncoderController::setDialnorm(int value) {
    const int clamped = std::clamp(value, 1, 31);
    if (clamped == meta_.dialnorm) {
        return;
    }
    meta_.dialnorm = clamped;
    emit planChanged();
}

void EncoderController::setMeasureDialnorm(bool on) {
    if (on == meta_.measure_dialnorm) {
        return;
    }
    meta_.measure_dialnorm = on;
    emit planChanged();
}

void EncoderController::setCmixIndex(int index) {
    const auto value = static_cast<ac3::meta::CentreMixLevel>(std::clamp(index, 0, 2));
    if (value == meta_.cmixlev) {
        return;
    }
    meta_.cmixlev = value;
    emit planChanged();
    // The downmix levels ARE the fold-down, so changing one changes where a
    // wider source lands.
    refreshRouting();
}

void EncoderController::setSurmixIndex(int index) {
    const auto value = static_cast<ac3::meta::SurroundMixLevel>(std::clamp(index, 0, 2));
    if (value == meta_.surmixlev) {
        return;
    }
    meta_.surmixlev = value;
    emit planChanged();
    refreshRouting();
}

void EncoderController::setMixmeta(bool on) {
    if (on == meta_.mixmeta) {
        return;
    }
    meta_.mixmeta = on;
    emit planChanged();
}

void EncoderController::setLfeMix(int value) {
    // -1 is the "off" end of the slider. §E2.3.1.10 makes absence a decision
    // in its own right: LFE mixing disabled, not merely turned right down.
    const int clamped = std::clamp(value, -1, 31);
    if (clamped == lfeMix()) {
        return;
    }
    meta_.lfemix = clamped < 0 ? std::nullopt : std::optional{clamped};
    emit planChanged();
}

void EncoderController::setDmixIndex(int index) {
    const auto value = static_cast<ac3::meta::DownmixMode>(std::clamp(index, 0, 2));
    if (value == meta_.dmixmod) {
        return;
    }
    meta_.dmixmod = value;
    emit planChanged();
}

void EncoderController::setAtmosEnabled(bool enabled) {
    if (atmos_enabled_ == enabled || busy_) {
        return;
    }
    atmos_enabled_ = enabled;
    // Objects are carried in an E-AC-3 stream and nothing else: the EMDF
    // container rides in Annex E aux data, and AC-3 has no addbsi field to
    // flag it with.
    if (atmos_enabled_) {
        codec_ = plan::Codec::kEac3;
    }
    emit planChanged();
    emit outputChanged();
    refreshRouting();
}

void EncoderController::setObjectX(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (object_x_ != clamped) {
        object_x_ = clamped;
        emit objectsChanged();
    }
}

void EncoderController::setObjectY(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (object_y_ != clamped) {
        object_y_ = clamped;
        emit objectsChanged();
    }
}

void EncoderController::setObjectZ(double value) {
    const double clamped = std::clamp(value, -1.0, 1.0);
    if (object_z_ != clamped) {
        object_z_ = clamped;
        emit objectsChanged();
    }
}

void EncoderController::setObjectSpread(double value) {
    const double clamped = std::clamp(value, 0.0, 0.5);
    if (object_spread_ != clamped) {
        object_spread_ = clamped;
        emit objectsChanged();
    }
}

void EncoderController::setObjectLfeSend(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (object_lfe_send_ != clamped) {
        object_lfe_send_ = clamped;
        emit objectsChanged();
    }
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

std::uint16_t EncoderController::currentLocationMask() const {
    return static_cast<std::uint16_t>(ac3::eac3::chanmap::acmod_map(bed_acmod_, bed_lfe_) |
                                      extras_mask_);
}

plan::Plan EncoderController::currentPlan() const {
    plan::Plan p{.codec = codec_,
                 // Ignored whenever custom_locations is set below (every case
                 // except object mode); kept a plain 5.1 rather than left
                 // default-constructed only so a stray read of it before that
                 // branch runs is never a channel width nothing can carry.
                 .layout = plan::LayoutId::k51,
                 .bitrate_kbps = static_cast<std::uint32_t>(bitrate_kbps_),
                 .tools = tools_,
                 .meta = meta_};
    // Object mode always codes its own 5.1 bed: JOC reconstructs from five
    // channels (§6.3.2.2) and the LFE is outside the matrix entirely, so the
    // bed/LFE/extras picker's own state is beside the point while it is on.
    if (!atmos_enabled_) {
        p.custom_locations = currentLocationMask();
    }
    if (source_) {
        if (const auto rate = to_sample_rate(source_->wav.sample_rate)) {
            p.sample_rate = *rate;
        }
    }
    return p;
}

plan::ChannelPlan EncoderController::effectiveChannelPlan() const {
    return plan::resolve(currentPlan());
}

QString EncoderController::effectiveLabel() const {
    if (atmos_enabled_) {
        return QStringLiteral("5.1 bed");
    }
    return channelShapeName();
}

void EncoderController::refreshRouting() {
    const auto p = currentPlan();
    const auto label = effectiveLabel();

    if (atmos_enabled_) {
        routing_summary_ =
            object_count_ > 0
                ? QStringLiteral("%1 objects over a 5.1 bed; a legacy decoder hears the bed.")
                      .arg(object_count_)
                : QStringLiteral("Each source channel becomes an object over a 5.1 bed.");
        emit routingChanged();
        return;
    }

    if (!source_) {
        routing_summary_ = QStringLiteral("%1 · %2").arg(label, layoutDetail());
        emit routingChanged();
        return;
    }

    const auto cp = effectiveChannelPlan();
    const auto routing = plan::route(cp, source_->wav.channels.size(), p.meta.cmixlev,
                                     p.meta.surmixlev);
    if (!routing) {
        routing_summary_ = QStringLiteral("%1 source channels — %2")
                               .arg(source_->wav.channels.size())
                               .arg(to_qstring(
                                   plan::describe(plan::PlanError::kNoSourceLayout)));
        emit routingChanged();
        return;
    }

    if (routing->is_permutation()) {
        routing_summary_ = QStringLiteral("The source is already %1; every channel is "
                                          "carried straight through.")
                               .arg(label);
        emit routingChanged();
        return;
    }

    // Naming the silent channels is the whole point of this line: a layout the
    // source cannot fill is a legitimate thing to ask for, but only if it is
    // obvious that is what is happening.
    const auto names = plan::coded_channel_names(cp);
    QStringList silent;
    for (int c = 0; c < routing->coded_channels; ++c) {
        bool fed = false;
        for (int s = 0; s < routing->source_channels && !fed; ++s) {
            fed = routing->at(c, s) != 0.0;
        }
        if (!fed) {
            silent.append(QString::fromStdString(names[static_cast<std::size_t>(c)]));
        }
    }
    routing_summary_ =
        QStringLiteral("%1 source channels rendered onto %2.")
            .arg(routing->source_channels)
            .arg(label);
    if (!silent.isEmpty()) {
        routing_summary_ +=
            QStringLiteral("  Silent (the source carries nothing that belongs there): %1")
                .arg(silent.join(QStringLiteral(", ")));
    }
    emit routingChanged();
}

QString EncoderController::outputSuffix() const {
    if (container_index_ == kContainerMatroska) {
        return QStringLiteral("mkv");
    }
    // Object mode is E-AC-3 whatever the codec box says, so the suffix follows
    // the plan rather than the control.
    return to_qstring(plan::codec_suffix(atmos_enabled_ ? plan::Codec::kEac3 : codec_));
}

QString EncoderController::suggestedOutputName() const {
    const QString suffix = QStringLiteral(".") + outputSuffix();
    if (source_path_.isEmpty()) {
        return QStringLiteral("output") + suffix;
    }
    return QFileInfo(source_path_).completeBaseName() + suffix;
}

void EncoderController::refreshCaptureDevices() {
    QStringList names;
    devices_.clear();
    if (auto found = ac3::capture::enumerate_devices()) {
        devices_ = std::move(*found);
        for (const auto& device : devices_) {
            names.append(QString::fromStdString(device.name) +
                         (device.is_default ? QStringLiteral("  [default]") : QString()));
        }
    }
    if (names != capture_devices_) {
        capture_devices_ = names;
        emit captureDevicesChanged();
    }
}

void EncoderController::setStatus(const QString& text) {
    if (text == status_) {
        return;
    }
    status_ = text;
    emit statusChanged();
}

void EncoderController::setBusy(bool busy) {
    if (busy == busy_) {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void EncoderController::setProgress(double value) {
    if (qFuzzyCompare(value + 1.0, progress_ + 1.0)) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void EncoderController::setMetering(bool metering) {
    if (metering == metering_) {
        return;
    }
    metering_ = metering;
    emit meteringChanged();
}

// ---------------------------------------------------------------------------
// Metering. Every figure the meters draw — including where a level sits on
// the bar — comes from ac3::analysis, so the GUI and ac3cli cannot disagree
// about the same audio.
// ---------------------------------------------------------------------------

std::vector<bool> EncoderController::fedChannels() const {
    const auto p = currentPlan();
    const auto cp = effectiveChannelPlan();
    const auto count = plan::coded_channels(cp).size();
    if (atmos_enabled_) {
        // Which bed channels the objects reach depends on where they are, so
        // it is answered by panning them exactly as the encoder will. Objects
        // at the front of the room legitimately leave the surrounds silent,
        // and claiming otherwise would have the display report a fault.
        std::vector<bool> fed(6, false);
        const auto objects = static_cast<std::size_t>(std::max(object_count_, 1));
        for (std::size_t i = 0; i < objects; ++i) {
            const double offset =
                objects < 2 ? 0.0
                            : object_spread_ * (2.0 * static_cast<double>(i) /
                                                    static_cast<double>(objects - 1) - 1.0);
            const auto gains =
                ac3::spatial::pan_room(std::clamp(object_x_ + offset, 0.0, 1.0), object_y_);
            for (std::size_t ch = 0; ch < gains.size(); ++ch) {
                fed[ch] = fed[ch] || gains[ch] != 0.0;
            }
        }
        // An object reaches the LFE only through the explicit send: there is
        // no direction that points at it (§6.3.2.2 bypasses it entirely).
        fed[5] = object_lfe_send_ > 0.0;
        return fed;
    }
    if (!source_) {
        return std::vector<bool>(count, true);
    }
    const auto routing = plan::route(cp, source_->wav.channels.size(), p.meta.cmixlev,
                                     p.meta.surmixlev);
    if (!routing) {
        return std::vector<bool>(count, true);
    }
    std::vector<bool> fed(count, false);
    for (int c = 0; c < routing->coded_channels; ++c) {
        for (int s = 0; s < routing->source_channels && !fed[static_cast<std::size_t>(c)]; ++s) {
            fed[static_cast<std::size_t>(c)] = routing->at(c, s) != 0.0;
        }
    }
    return fed;
}

void EncoderController::setLayout(ac3::Acmod acmod, bool lfe, const QStringList& names,
                                  const QString& label, const std::vector<bool>& fed) {
    acmod_ = acmod;
    lfe_ = lfe;
    channel_names_ = names;
    channel_fed_ = fed.empty()
                       ? std::vector<bool>(static_cast<std::size_t>(names.size()), true)
                       : fed;
    channel_fed_.resize(static_cast<std::size_t>(names.size()), true);
    layout_name_ = label;
    emit layoutChanged();
    // Start silent: leaving the previous source's levels under the new
    // source's labels would put a number against the wrong channel.
    publishLevels(
        std::vector<ac3::analysis::ChannelLevel>(static_cast<std::size_t>(names.size())));
}

void EncoderController::clearLayout() {
    channel_names_.clear();
    layout_name_.clear();
    channel_levels_.clear();
    soundfield_.clear();
    setMetering(false);
    emit layoutChanged();
    emit levelsChanged();
}

void EncoderController::publishLevels(std::span<const ac3::analysis::ChannelLevel> levels) {
    QVariantList entries;
    entries.reserve(static_cast<qsizetype>(levels.size()));
    for (std::size_t ch = 0; ch < levels.size(); ++ch) {
        const auto& level = levels[ch];
        // Only the bed's channels have an azimuth: a dependent substream's
        // speakers are named by a chanmap, which no acmod can place.
        const auto azimuth =
            ac3::analysis::channel_azimuth_deg(acmod_, lfe_, static_cast<int>(ch));
        entries.append(QVariantMap{
            {QStringLiteral("peakDb"), level.peak_db},
            {QStringLiteral("rmsDb"), level.rms_db},
            {QStringLiteral("holdDb"), level.hold_db},
            {QStringLiteral("clipped"), level.clipped},
            // Bar positions are computed here rather than in QML: a front end
            // that mapped decibels its own way would quietly disagree with
            // every other reading of the same signal.
            {QStringLiteral("peak"),
             ac3::analysis::meter_fraction(level.peak_db, kMeterFloorDb)},
            {QStringLiteral("rms"), ac3::analysis::meter_fraction(level.rms_db, kMeterFloorDb)},
            {QStringLiteral("hold"), ac3::analysis::meter_fraction(level.hold_db, kMeterFloorDb)},
            {QStringLiteral("azimuthDeg"), azimuth.value_or(0.0)},
            {QStringLiteral("directional"), azimuth.has_value()},
            // A channel the source cannot fill reads -inf for a reason, and
            // the display should say which reason: silent by routing is not
            // the same as silent because nothing is reaching the meter.
            {QStringLiteral("fed"),
             ch >= channel_fed_.size() || channel_fed_[ch]},
        });
    }
    channel_levels_ = std::move(entries);

    const auto field = ac3::analysis::energy_vector(levels, acmod_);
    soundfield_ = QVariantMap{
        {QStringLiteral("azimuthDeg"), field.azimuth_deg},
        {QStringLiteral("magnitude"), field.magnitude},
        {QStringLiteral("levelDb"), field.level_db},
        {QStringLiteral("active"), field.magnitude > 0.0},
    };
    emit levelsChanged();
}

void EncoderController::publishSummary(const ac3::analysis::LevelMeter& meter) {
    // The exact whole-run figures, not the ballistic tail: once a run is over
    // there is a right answer, and the display should settle on it.
    std::vector<ac3::analysis::ChannelLevel> levels(
        static_cast<std::size_t>(meter.channel_count()));
    for (std::size_t ch = 0; ch < levels.size(); ++ch) {
        const auto& stats = meter.summary()[ch];
        levels[ch].peak_db = stats.peak_db();
        levels[ch].hold_db = stats.peak_db();
        levels[ch].rms_db = stats.rms_db();
        levels[ch].clipped = stats.clipped_samples > 0;
    }
    publishLevels(levels);
}

void EncoderController::refreshOutputDevices() {
    QStringList names;
    outputs_.clear();
    if (auto found = ac3::sinks::enumerate_render_devices()) {
        outputs_ = std::move(*found);
        for (const auto& device : outputs_) {
            // The capability is part of the label: a user staring at a greyed
            // out device deserves to know which of the two reasons applies.
            const QString capability =
                device.supports_ac3_passthrough
                    ? QStringLiteral("AC-3 ready")
                    : (device.supports_exclusive_pcm ? QStringLiteral("cannot bitstream")
                                                     : QStringLiteral("no exclusive access"));
            names.append(QStringLiteral("%1  —  %2")
                             .arg(QString::fromStdString(device.name), capability));
        }
    }
    if (names != output_devices_) {
        output_devices_ = names;
        emit outputDevicesChanged();
    }
}

void EncoderController::playToReceiver(int deviceIndex) {
    if (playing_ || busy_ || output_path_.isEmpty()) {
        return;
    }
    if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= outputs_.size()) {
        setStatus(QStringLiteral("Choose an output device first."));
        return;
    }
    const auto device = outputs_[static_cast<std::size_t>(deviceIndex)];
    if (!device.supports_ac3_passthrough) {
        setStatus(QStringLiteral("\"%1\" will not accept AC-3 over IEC 61937. Only S/PDIF and "
                                 "HDMI outputs can bitstream, and Dolby Digital must be enabled "
                                 "for the device in Sound settings.")
                      .arg(QString::fromStdString(device.name)));
        return;
    }

    playing_ = true;
    emit playingChanged();
    setStatus(QStringLiteral("Streaming to %1…").arg(QString::fromStdString(device.name)));

    const QString path = output_path_;
    std::ignore = QtConcurrent::run([this, path, device] {
        std::ifstream in{path.toStdString(), std::ios::binary};
        const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
        std::vector<std::byte> stream(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            stream[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
        }

        QString message;
        const auto bsid = ac3::stream_bsid(stream);
        const auto frames = ac3::split_frames(stream);
        if (bsid && *bsid > 8) {
            // The IEC 61937 packer emits AC-3 bursts only (data type 1). A
            // receiver would take an E-AC-3 burst; this packer cannot make one.
            message = QStringLiteral("That stream is E-AC-3 (bsid %1). Passthrough here wraps "
                                     "AC-3 bursts only — encode as AC-3 to send it.")
                          .arg(*bsid);
        } else if (!frames || frames->empty()) {
            message = QStringLiteral("That file is not a valid AC-3 stream.");
        } else {
            const auto fscod = std::to_integer<std::uint32_t>((*frames)[0][4]) >> 6;
            const auto rate = sample_rate_hz(static_cast<ac3::SampleRate>(fscod));
            ac3::sinks::PassthroughSink sink;
            const auto started = sink.start(device.id, rate);
            if (!started) {
                const auto why = ac3::sinks::describe(started.error());
                message = QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()));
            } else {
                for (const auto& frame : *frames) {
                    const auto burst = ac3::iec61937::wrap_frame(frame);
                    if (!burst) {
                        break;
                    }
                    while (!sink.submit(*burst)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
                while (sink.stats().bursts_rendered < sink.stats().bursts_submitted) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                const auto stats = sink.stats();
                sink.stop();
                message = QStringLiteral("Streamed %1 bursts (%2 underruns).")
                              .arg(stats.bursts_rendered)
                              .arg(stats.underruns);
            }
        }

        QMetaObject::invokeMethod(this, [this, message] {
            playing_ = false;
            emit playingChanged();
            setStatus(message);
        });
    });
}

void EncoderController::setRecording(bool recording) {
    if (recording == recording_) {
        return;
    }
    recording_ = recording;
    emit recordingChanged();
}

void EncoderController::stopRecording() {
    stop_recording_.store(true, std::memory_order_relaxed);
}

void EncoderController::startRecording(int deviceIndex, const QUrl& url) {
    if (busy_ || recording_) {
        return;
    }
    if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= devices_.size()) {
        setStatus(QStringLiteral("Choose a capture device first."));
        return;
    }
    const auto device = devices_[static_cast<std::size_t>(deviceIndex)];
    const auto rate = to_sample_rate(device.sample_rate);
    if (!rate) {
        setStatus(QStringLiteral("\"%1\" runs at %2 Hz; AC-3 needs 32, 44.1 or 48 kHz. "
                                 "Change the endpoint's shared-mode format in Windows sound "
                                 "settings.")
                      .arg(QString::fromStdString(device.name))
                      .arg(device.sample_rate));
        return;
    }

    // A capture endpoint is a source like any other, so it goes through the
    // same plan: whatever the microphone delivers is routed onto whatever
    // layout is selected, in whichever codec.
    plan::Plan p = currentPlan();
    p.sample_rate = *rate;
    const auto cp = effectiveChannelPlan();
    const auto label = effectiveLabel();
    auto routing = plan::route(cp, device.channels, p.meta.cmixlev, p.meta.surmixlev);
    if (!routing) {
        setStatus(QStringLiteral("\"%1\" delivers %2 channels — %3")
                      .arg(QString::fromStdString(device.name))
                      .arg(device.channels)
                      .arg(to_qstring(plan::describe(plan::PlanError::kNoSourceLayout))));
        return;
    }
    if (const auto bad = plan::validate(p)) {
        setStatus(to_qstring(plan::describe(*bad)));
        return;
    }

    capture_ = std::make_unique<ac3::capture::Capture>();
    const auto started = capture_->start(device.id, device.kind);
    if (!started) {
        const auto why = ac3::capture::describe(started.error());
        capture_.reset();
        setStatus(QStringLiteral("Could not open \"%1\": %2")
                      .arg(QString::fromStdString(device.name),
                           QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()))));
        return;
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    emit outputChanged();

    stop_recording_.store(false, std::memory_order_relaxed);
    setRecording(true);
    setBusy(true);
    recorded_seconds_ = 0.0;
    emit recordedSecondsChanged();

    const auto names = plan::coded_channel_names(cp);
    QStringList labels;
    std::vector<bool> fed(names.size(), false);
    for (const auto& name : names) {
        labels.append(QString::fromStdString(name));
    }
    for (int c = 0; c < routing->coded_channels; ++c) {
        for (int s = 0; s < routing->source_channels && !fed[static_cast<std::size_t>(c)]; ++s) {
            fed[static_cast<std::size_t>(c)] = routing->at(c, s) != 0.0;
        }
    }
    setLayout(cp.bed_acmod, cp.bed_lfe, labels, label, fed);
    setMetering(true);
    setStatus(QStringLiteral("Recording from %1…").arg(QString::fromStdString(device.name)));

    const auto channels = capture_->channels();
    const auto sample_rate = capture_->sample_rate();
    const bool eac3 = p.codec == plan::Codec::kEac3;

    std::ignore = QtConcurrent::run([this, path, p, routing = *routing, channels, sample_rate,
                                     cp, eac3]() {
        const auto coded = static_cast<std::size_t>(routing.coded_channels);
        ac3::FrameEncoder ac3_encoder{plan::ac3_config(p)};
        ac3::eac3::AccessUnitEncoder eac3_encoder{plan::eac3_config(p)};
        ac3::analysis::LevelMeter meter{cp.bed_acmod, cp.bed_lfe, sample_rate,
                                        static_cast<int>(coded)};

        std::vector<std::vector<std::byte>> frames;
        std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                                       channels);
        std::vector<std::vector<float>> source(
            static_cast<std::size_t>(routing.source_channels),
            std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::vector<float>> block(coded,
                                              std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::span<const float>> in;
        std::vector<std::span<float>> out;
        std::vector<std::span<const float>> views;
        for (auto& channel : source) {
            in.emplace_back(channel);
        }
        for (auto& channel : block) {
            out.emplace_back(channel);
            views.emplace_back(channel);
        }

        while (!stop_recording_.load(std::memory_order_relaxed)) {
            std::size_t filled = 0;
            while (filled < interleaved.size() &&
                   !stop_recording_.load(std::memory_order_relaxed)) {
                const auto got = capture_->buffer()->read(
                    std::span{interleaved}.subspan(filled, interleaved.size() - filled));
                filled += got;
                if (got == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (filled < interleaved.size()) {
                break;  // stopped mid-frame; drop the partial frame
            }

            for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                const std::size_t base = static_cast<std::size_t>(i) * channels;
                for (std::size_t ch = 0; ch < source.size(); ++ch) {
                    source[ch][static_cast<std::size_t>(i)] =
                        ch < channels ? interleaved[base + ch] : 0.0f;
                }
            }
            plan::render(routing, in, out, ac3::kSamplesPerFrame);
            meter.process(views);

            if (eac3) {
                const auto unit = eac3_encoder.encode_access_unit(views);
                if (!unit) {
                    break;
                }
                frames.push_back(unit->bytes);
            } else {
                const auto frame = ac3_encoder.encode_frame(views);
                if (!frame) {
                    break;
                }
                frames.push_back(*frame);
            }

            // A frame is 32 ms at 48 kHz, so publishing one snapshot per frame
            // already lands close to 30 Hz without any extra throttling.
            const double seconds = static_cast<double>(frames.size() * ac3::kSamplesPerFrame) /
                                   static_cast<double>(sample_rate);
            std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                              meter.levels().end());
            QMetaObject::invokeMethod(this, [this, seconds, snapshot = std::move(snapshot)] {
                recorded_seconds_ = seconds;
                emit recordedSecondsChanged();
                publishLevels(snapshot);
            });
        }

        const QString problem =
            writeOutput(path, frames, sample_rate, plan::rendered_channel_count(cp));

        std::vector<ac3::analysis::ChannelLevel> totals(
            static_cast<std::size_t>(meter.channel_count()));
        for (std::size_t ch = 0; ch < totals.size(); ++ch) {
            const auto& stats = meter.summary()[ch];
            totals[ch].peak_db = stats.peak_db();
            totals[ch].hold_db = stats.peak_db();
            totals[ch].rms_db = stats.rms_db();
            totals[ch].clipped = stats.clipped_samples > 0;
        }

        const auto count = frames.size();
        QMetaObject::invokeMethod(this, [this, count, problem, totals = std::move(totals)] {
            const auto stats = capture_->stats();
            capture_->stop();
            capture_.reset();
            setRecording(false);
            setBusy(false);
            setMetering(false);
            if (problem.isEmpty()) {
                setStatus(
                    QStringLiteral("Recorded %1 frames to %2 (%3 dropped, %4 silence-filled)")
                        .arg(count)
                        .arg(QFileInfo(output_path_).fileName())
                        .arg(stats.frames_dropped)
                        .arg(stats.frames_silence_filled));
            } else {
                setStatus(problem);
            }
            emit recordedSecondsChanged();
            publishLevels(totals);
            emit encodeFinished(problem.isEmpty(), status());
        });
    });
}

void EncoderController::loadSourceFile(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    auto wav = ac3::io::read_wav(path.toStdString());
    if (!wav) {
        source_.reset();
        source_ready_ = false;
        source_path_ = path;
        source_info_.clear();
        object_count_ = 0;
        clearLayout();
        emit sourceChanged();
        setStatus(QStringLiteral("Could not read %1: %2")
                      .arg(QFileInfo(path).fileName(),
                           QString::fromUtf8(ac3::io::describe(wav.error()).data(),
                                             static_cast<qsizetype>(
                                                 ac3::io::describe(wav.error()).size()))));
        return;
    }

    const auto channels = wav->channels.size();
    const auto rate = wav->sample_rate;
    const double seconds =
        rate > 0 ? static_cast<double>(wav->frame_count()) / static_cast<double>(rate) : 0.0;

    QString problem;
    if (!to_sample_rate(rate)) {
        problem = QStringLiteral("sample rate %1 Hz is not legal here (need 32, 44.1 or 48 kHz)")
                      .arg(rate);
    } else if (!plan::layout_for_source(channels)) {
        problem = QStringLiteral("%1 channels — %2")
                      .arg(channels)
                      .arg(to_qstring(plan::describe(plan::PlanError::kNoSourceLayout)));
    }

    // A newly loaded file picks the bed+extras that match it, which is what a
    // user almost always wants and is the only choice that carries every
    // channel through untouched. Everything else stays where they left it.
    if (const auto natural = plan::layout_for_source(channels)) {
        if (plan::carries(codec_, *natural)) {
            const auto cp = plan::channel_plan_for(*natural);
            bed_acmod_ = cp.bed_acmod;
            bed_lfe_ = cp.bed_lfe;
            extras_mask_ = 0;
            for (const auto dependent : cp.dependents) {
                extras_mask_ = static_cast<std::uint16_t>(extras_mask_ | dependent);
            }
        } else if (codec_ == plan::Codec::kAc3 && extras_mask_ != 0) {
            // The natural layout needs extras AC-3 cannot carry, and the
            // current selection also does - fall back to a plain, always-
            // legal 5.1 rather than leave an uncarryable one in place.
            bed_acmod_ = ac3::Acmod::k3_2;
            bed_lfe_ = true;
            extras_mask_ = 0;
        }
        emit planChanged();
    }

    source_info_ = QStringLiteral("%1 Hz · %2 channel%5 · %3:%4")
                       .arg(rate)
                       .arg(channels)
                       .arg(static_cast<int>(seconds) / 60)
                       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'))
                       .arg(channels == 1 ? QString() : QStringLiteral("s"));
    object_count_ = static_cast<int>(std::min<std::size_t>(channels, 15));
    source_ = std::make_unique<Source>(Source{std::move(*wav)});
    source_path_ = path;
    source_ready_ = problem.isEmpty();
    emit sourceChanged();
    refreshRouting();

    // What the file actually holds, shown before a single frame is encoded:
    // the meters answer "what is in here?" as well as "what is going out?".
    // It is metered as the SOURCE, not as the output layout, because that is
    // the question a freshly loaded file raises.
    if (const auto source_layout = ac3::io::ac3_layout_for(channels)) {
        QStringList labels;
        const int count = ac3::analysis::channel_count(source_layout->acmod,
                                                       source_layout->lfe);
        for (int ch = 0; ch < count; ++ch) {
            labels.append(to_qstring(ac3::analysis::channel_name(source_layout->acmod,
                                                                 source_layout->lfe, ch)));
        }
        setLayout(source_layout->acmod, source_layout->lfe, labels,
                  to_qstring(ac3::analysis::layout_name(source_layout->acmod,
                                                        source_layout->lfe)));
        setMetering(false);
        ac3::analysis::LevelMeter meter{source_layout->acmod, source_layout->lfe, rate};
        std::vector<std::span<const float>> views(source_layout->wav_index.size());
        for (std::size_t ch = 0; ch < views.size(); ++ch) {
            views[ch] = source_->wav.channels[source_layout->wav_index[ch]];
        }
        meter.process(views);
        publishSummary(meter);
    } else {
        clearLayout();
    }

    setStatus(source_ready_ ? QStringLiteral("Ready to encode %1.").arg(QFileInfo(path).fileName())
                            : QStringLiteral("Cannot encode %1: %2")
                                  .arg(QFileInfo(path).fileName(), problem));
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

QString EncoderController::writeOutput(const QString& path,
                                       const std::vector<std::vector<std::byte>>& frames,
                                       std::uint32_t sample_rate, int channels) const {
    if (frames.empty()) {
        return QStringLiteral("Nothing was encoded.");
    }
    if (container_index_ == kContainerMatroska) {
        const bool eac3 = atmos_enabled_ || codec_ == plan::Codec::kEac3;
        const matroska::AudioTrack track{
            .codec_id = std::string{eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
            .sample_rate = sample_rate,
            .channels = channels,
            .samples_per_frame = ac3::kSamplesPerFrame};
        const auto file = matroska::mux(track, frames);
        if (!file) {
            return to_qstring(matroska::describe(file.error()));
        }
        std::ofstream out{path.toStdString(), std::ios::binary};
        if (!out) {
            return QStringLiteral("Could not open the output file for writing.");
        }
        out.write(reinterpret_cast<const char*>(file->data()),
                  static_cast<std::streamsize>(file->size()));
        return out ? QString() : QStringLiteral("Writing the Matroska file failed.");
    }

    std::ofstream out{path.toStdString(), std::ios::binary};
    if (!out) {
        return QStringLiteral("Could not open the output file for writing.");
    }
    for (const auto& frame : frames) {
        out.write(reinterpret_cast<const char*>(frame.data()),
                  static_cast<std::streamsize>(frame.size()));
    }
    return out ? QString() : QStringLiteral("Writing the stream failed.");
}

void EncoderController::cancel() {
    cancel_requested_.store(true, std::memory_order_relaxed);
}

void EncoderController::encodeTo(const QUrl& url) {
    if (busy_ || !source_ready_ || !source_) {
        return;
    }
    const auto rate = to_sample_rate(source_->wav.sample_rate);
    if (!rate) {
        return;
    }
    auto p = currentPlan();
    if (const auto bad = plan::validate(p)) {
        setStatus(to_qstring(plan::describe(*bad)));
        return;
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    emit outputChanged();

    cancel_requested_.store(false, std::memory_order_relaxed);
    setBusy(true);
    setProgress(0.0);
    setStatus(QStringLiteral("Encoding…"));

    const auto sample_rate = source_->wav.sample_rate;
    // The WAV payload is copied into the worker so the GUI thread stays free
    // to swap the loaded source while an encode runs.
    std::vector<std::vector<float>> planes = source_->wav.channels;

    if (atmos_enabled_) {
        encodeObjects(path, std::move(planes), sample_rate);
        return;
    }
    encodeChannels(path, std::move(planes), sample_rate);
}

void EncoderController::encodeChannels(const QString& path,
                                       std::vector<std::vector<float>> planes,
                                       std::uint32_t sample_rate) {
    auto p = currentPlan();
    const auto cp = plan::resolve(p);
    const auto label = effectiveLabel();
    const auto routing = plan::route(cp, planes.size(), p.meta.cmixlev, p.meta.surmixlev);
    if (!routing) {
        setBusy(false);
        setStatus(to_qstring(plan::describe(plan::PlanError::kNoSourceLayout)));
        emit encodeFinished(false, status());
        return;
    }

    // §5.4.2.8 wants dialogue level below full scale, and measuring it needs
    // the whole programme (the BS.1770 relative gate does), so it happens once
    // here rather than per frame. The layout it measures is the OUTPUT's,
    // because the channel weighting depends on which positions are surrounds.
    if (p.meta.measure_dialnorm) {
        ac3::meta::LoudnessMeter loudness{p.sample_rate, cp.bed_acmod, cp.bed_lfe};
        std::vector<std::span<const float>> views;
        for (const auto& channel : planes) {
            views.emplace_back(channel);
        }
        loudness.push(views);
        if (const auto lkfs = loudness.integrated_lkfs()) {
            p.meta.dialnorm = ac3::meta::dialnorm_from_lkfs(*lkfs);
        } else {
            setBusy(false);
            setStatus(QStringLiteral("No audio above the -70 LKFS gate, so dialnorm cannot be "
                                     "measured. Set it by hand instead."));
            emit encodeFinished(false, status());
            return;
        }
    }

    const auto names = plan::coded_channel_names(cp);
    QStringList labels;
    for (const auto& name : names) {
        labels.append(QString::fromStdString(name));
    }
    setLayout(cp.bed_acmod, cp.bed_lfe, labels, label, fedChannels());
    setMetering(true);

    const bool eac3 = p.codec == plan::Codec::kEac3;
    std::ignore = QtConcurrent::run([this, path, p, routing = *routing, cp, sample_rate,
                                     eac3, label, planes = std::move(planes)]() mutable {
        const auto coded = static_cast<std::size_t>(routing.coded_channels);
        ac3::FrameEncoder ac3_encoder{plan::ac3_config(p)};
        ac3::eac3::AccessUnitEncoder eac3_encoder{plan::eac3_config(p)};
        ac3::analysis::LevelMeter meter{cp.bed_acmod, cp.bed_lfe, sample_rate,
                                        static_cast<int>(coded)};

        const std::size_t total = planes.front().size();
        std::vector<std::vector<float>> source(planes.size(),
                                               std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::vector<float>> block(coded,
                                              std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> in;
        std::vector<std::span<float>> out;
        std::vector<std::span<const float>> views;
        std::vector<std::span<const float>> metered(coded);
        for (auto& channel : source) {
            in.emplace_back(channel);
        }
        for (auto& channel : block) {
            out.emplace_back(channel);
            views.emplace_back(channel);
        }

        std::vector<std::vector<std::byte>> frames;
        std::uint64_t bytes = 0;
        bool cancelled = false;
        QString problem;
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;

        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            if (cancel_requested_.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }
            // The tail frame is zero-padded to a full 1536 samples; the meter
            // sees only the real ones, so padding cannot pull the RMS down.
            const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
            for (std::size_t ch = 0; ch < planes.size(); ++ch) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    source[ch][static_cast<std::size_t>(i)] = at < total ? planes[ch][at] : 0.0f;
                }
            }
            plan::render(routing, in, out, ac3::kSamplesPerFrame);
            for (std::size_t ch = 0; ch < coded; ++ch) {
                metered[ch] = std::span{block[ch]}.first(valid);
            }
            meter.process(metered);

            if (eac3) {
                const auto unit = eac3_encoder.encode_access_unit(views);
                if (!unit) {
                    problem = QStringLiteral(
                        "The encoder cannot express this configuration — a wider layout needs "
                        "a higher bit rate to fit its substreams.");
                    break;
                }
                bytes += unit->bytes.size();
                frames.push_back(unit->bytes);
            } else {
                const auto frame = ac3_encoder.encode_frame(views);
                if (!frame) {
                    problem = QStringLiteral("The encoder rejected the settings — AC-3 takes "
                                             "only the 19 nominal rates of Table 5.18.");
                    break;
                }
                bytes += frame->size();
                frames.push_back(*frame);
            }

            const double done = static_cast<double>(start + ac3::kSamplesPerFrame) /
                                static_cast<double>(total);
            const auto now = std::chrono::steady_clock::now();
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                  meter.levels().end());
                QMetaObject::invokeMethod(
                    this, [this, done, snapshot = std::move(snapshot)] {
                        setProgress(std::min(done, 1.0));
                        publishLevels(snapshot);
                    });
            } else {
                QMetaObject::invokeMethod(this,
                                          [this, done] { setProgress(std::min(done, 1.0)); });
            }
        }

        if (problem.isEmpty() && !cancelled) {
            problem = writeOutput(path, frames, sample_rate, plan::rendered_channel_count(cp));
        }

        std::vector<ac3::analysis::ChannelLevel> totals(
            static_cast<std::size_t>(meter.channel_count()));
        for (std::size_t ch = 0; ch < totals.size(); ++ch) {
            const auto& stats = meter.summary()[ch];
            totals[ch].peak_db = stats.peak_db();
            totals[ch].hold_db = stats.peak_db();
            totals[ch].rms_db = stats.rms_db();
            totals[ch].clipped = stats.clipped_samples > 0;
        }

        const auto count = frames.size();
        QMetaObject::invokeMethod(this, [this, count, bytes, cancelled, problem, label, eac3,
                                         totals = std::move(totals)] {
            setBusy(false);
            setMetering(false);
            setProgress(cancelled ? 0.0 : 1.0);
            publishLevels(totals);
            if (cancelled) {
                setStatus(QStringLiteral("Encode cancelled."));
            } else if (!problem.isEmpty()) {
                setStatus(problem);
            } else {
                setStatus(QStringLiteral("Wrote %1 %2 %3 (%4 KB) to %5")
                              .arg(count)
                              .arg(label, eac3 ? QStringLiteral("access units")
                                                : QStringLiteral("frames"))
                              .arg(bytes / 1024)
                              .arg(QFileInfo(output_path_).fileName()));
            }
            emit encodeFinished(!cancelled && problem.isEmpty(), status());
        });
    });
}

// ---------------------------------------------------------------------------
// Object mode. Kept apart from the channel path rather than threaded through
// it with flags: almost nothing is shared — a bed whose channel count has
// nothing to do with the source's, and a per-frame metadata payload the
// channel encoders have no concept of.
// ---------------------------------------------------------------------------

void EncoderController::encodeObjects(const QString& path,
                                      std::vector<std::vector<float>> planes,
                                      std::uint32_t sample_rate) {
    const auto p = currentPlan();
    const ac3::oba::Position centre{.x = object_x_, .y = object_y_, .z = object_z_};
    const double spread = object_spread_;
    const double lfe_send = object_lfe_send_;

    // The meters follow the BED, not the source: 5.1 is what comes out and
    // what a legacy decoder hears, whatever the source layout was.
    const auto names = plan::coded_channel_names(plan::LayoutId::k51);
    QStringList labels;
    for (const auto& name : names) {
        labels.append(QString::fromStdString(name));
    }
    setLayout(ac3::Acmod::k3_2, true, labels, QStringLiteral("5.1 bed"), fedChannels());
    setMetering(true);

    std::ignore = QtConcurrent::run([this, path, p, sample_rate, centre, spread, lfe_send,
                                     planes = std::move(planes)]() mutable {
        // TS 103 420 §8.3.2.2 caps a programme at sixteen objects, and the
        // bed's LFE is one of them.
        const std::size_t nobjects = std::min<std::size_t>(planes.size(), 15);
        ac3::oba::AtmosEncoder encoder{{.sample_rate = p.sample_rate,
                                        .bitrate_kbps = p.bitrate_kbps,
                                        .dialnorm = p.meta.dialnorm,
                                        .num_bands_idx = 4},
                                       static_cast<int>(nobjects)};

        // Objects that reach the bed by the SAME route are exactly the ones
        // JOC cannot pull apart again, so they are spread either side of the
        // chosen point rather than stacked on it. A stereo pair lands at
        // exactly -/+ spread.
        std::vector<ac3::oba::ObjectPlacement> placement(nobjects);
        for (std::size_t i = 0; i < nobjects; ++i) {
            const double offset =
                nobjects < 2 ? 0.0
                             : spread * (2.0 * static_cast<double>(i) /
                                             static_cast<double>(nobjects - 1) - 1.0);
            placement[i].position = {.x = std::clamp(centre.x + offset, 0.0, 1.0),
                                     .y = centre.y,
                                     .z = centre.z};
            // Every object is panned into the SAME five channels, so their
            // contributions add there. At unity apiece a six-channel source
            // put the bed's centre 7 dB over full scale; the inverse-root law
            // is what ac3cli's 'atmos' uses, and it keeps the sum near unity
            // for sources that are not identical.
            placement[i].gain = 0.7 / std::sqrt(static_cast<double>(nobjects));
            // Shared across the objects for the same reason: the LFE is one
            // channel, and sending every object at full strength would pile
            // the whole programme's low end into it.
            placement[i].lfe_send = lfe_send / std::sqrt(static_cast<double>(nobjects));
        }

        ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, sample_rate};

        const std::size_t total = planes.front().size();
        std::vector<std::vector<float>> block(nobjects,
                                              std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> views(nobjects);
        std::vector<std::span<const float>> metered(6);
        std::vector<std::vector<std::byte>> frames;
        std::uint64_t bytes = 0;
        bool cancelled = false;
        QString problem;
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;

        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            if (cancel_requested_.load(std::memory_order_relaxed)) {
                cancelled = true;
                break;
            }
            const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
            for (std::size_t ch = 0; ch < nobjects; ++ch) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    block[ch][static_cast<std::size_t>(i)] = at < total ? planes[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
            }
            const auto unit = encoder.encode_frame(views, placement);
            if (!unit) {
                problem = QStringLiteral(
                    "The frame cannot hold a 5.1 bed and the object metadata at this bit "
                    "rate — try 384 kbps or more.");
                break;
            }
            // The bed only exists once the frame is encoded, so it is metered
            // after the fact - unlike the channel path, where the meter sees
            // the same samples the encoder is about to be handed.
            for (std::size_t ch = 0; ch < metered.size(); ++ch) {
                metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
            }
            meter.process(metered);

            bytes += unit->bytes.size();
            frames.push_back(unit->bytes);

            const double done = static_cast<double>(start + ac3::kSamplesPerFrame) /
                                static_cast<double>(total);
            const auto now = std::chrono::steady_clock::now();
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                 meter.levels().end());
                QMetaObject::invokeMethod(
                    this, [this, done, snapshot = std::move(snapshot)] {
                        setProgress(std::min(done, 1.0));
                        publishLevels(snapshot);
                    });
            } else {
                QMetaObject::invokeMethod(this,
                                          [this, done] { setProgress(std::min(done, 1.0)); });
            }
        }

        if (problem.isEmpty() && !cancelled) {
            problem = writeOutput(path, frames, sample_rate, 6);
        }

        std::vector<ac3::analysis::ChannelLevel> totals(
            static_cast<std::size_t>(meter.channel_count()));
        for (std::size_t ch = 0; ch < totals.size(); ++ch) {
            const auto& stats = meter.summary()[ch];
            totals[ch].peak_db = stats.peak_db();
            totals[ch].hold_db = stats.peak_db();
            totals[ch].rms_db = stats.rms_db();
            totals[ch].clipped = stats.clipped_samples > 0;
        }

        const auto count = frames.size();
        const auto objects = ac3::oba::object_count(encoder.program());
        QMetaObject::invokeMethod(this, [this, count, bytes, nobjects, objects, cancelled,
                                         problem, totals = std::move(totals)] {
            setBusy(false);
            setMetering(false);
            setProgress(cancelled ? 0.0 : 1.0);
            publishLevels(totals);
            if (cancelled) {
                setStatus(QStringLiteral("Encode cancelled."));
            } else if (!problem.isEmpty()) {
                setStatus(problem);
            } else {
                setStatus(QStringLiteral("Wrote %1 Atmos access units (%2 KB) to %3 — "
                                         "%4 dynamic objects + the bed's LFE = %5 objects")
                              .arg(count)
                              .arg(bytes / 1024)
                              .arg(QFileInfo(output_path_).fileName())
                              .arg(nobjects)
                              .arg(objects));
            }
            emit encodeFinished(!cancelled && problem.isEmpty(), status());
        });
    });
}
