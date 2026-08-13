#include "encoder_controller.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <iterator>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
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

// AC-3 accepts only these three rates (A/52 Table 5.6). Used for capture
// devices too, deliberately: real audio hardware does not offer the Annex E
// fscod2 half rates, so a device gate has no reason to accept them.
std::optional<ac3::SampleRate> to_sample_rate(std::uint32_t hz) {
    switch (hz) {
        case 48000: return ac3::SampleRate::k48000;
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        default: return std::nullopt;
    }
}

// A loaded file's rate can be one of the three Annex E fscod2 half rates too,
// since unlike a capture device a WAV genuinely can be authored at 24/22.05/
// 16 kHz - but only when the target is E-AC-3; classic AC-3 has no fscod2.
std::optional<ac3::SampleRate> to_sample_rate_for_file(std::uint32_t hz, plan::Codec codec) {
    if (const auto sr = to_sample_rate(hz)) {
        return sr;
    }
    if (codec != plan::Codec::kEac3) {
        return std::nullopt;
    }
    switch (hz) {
        case 24000: return ac3::SampleRate::k24000;
        case 22050: return ac3::SampleRate::k22050;
        case 16000: return ac3::SampleRate::k16000;
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

// In the handoff's own display order - 1+1 first, "drawn ... with a dashed
// border so it reads as categorically different" (it is a bed, not a
// location mask; see EncoderController::isDualMono()'s own comment).
constexpr std::array<BedInfo, 8> kBeds{{
    {ac3::Acmod::kDualMono, "1+1"},
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
    {"wide", "Front wide", ac3::eac3::chanmap::kLwRwBit},
    {"rear", "Rear surround", ac3::eac3::chanmap::kLrsRrsBit},
    {"topf", "Ceiling front", ac3::eac3::chanmap::kVhlVhrBit},
    {"topr", "Ceiling rear", ac3::eac3::chanmap::kLtsRtsBit},
    {"lfe2", "Second LFE", ac3::eac3::chanmap::kLfe2Bit},
}};

// Space-joined location names for a bed's own full-bandwidth channels, e.g.
// "L C R Ls Rs" for 3/2 - what the bed button shows beneath its id.
//
// acmod_map(kDualMono, ...) answers "L R" - a placeholder Table E2.5 bits
// happen to need, documented at its own definition as "not a layout" and
// "rejected before it's ever consulted" for real encoding. It is not
// rejected here, so this has to name the actual thing instead: two
// programmes, not a stereo pair.
QString bed_channel_names(ac3::Acmod acmod) {
    if (acmod == ac3::Acmod::kDualMono) {
        return QStringLiteral("Program 1 · Program 2");
    }
    QStringList names;
    for (const auto location : ac3::eac3::chanmap::expand(
             ac3::eac3::chanmap::acmod_map(acmod, false))) {
        names.append(to_qstring(ac3::eac3::chanmap::name(location)));
    }
    return names.join(QStringLiteral(" "));
}

// ---------------------------------------------------------------------------
// Where a Table E2.5 location sits on the soundfield plans. This is a GUI-
// only convention - nothing about encoding reads it - extending the ITU-R
// BS.775 ring ac3::spatial::kSpeakerAzimuthDeg already fixes for the bed's
// five positions (L +30, C 0, R -30, Ls +110, Rs -110, degrees CCW from
// front) to the wider set of channels the general channel model can carry.
// Without this, a plan wider than a plain 5.1 bed had no way to place its
// extra channels at all: channel_azimuth_deg(acmod, lfe, index) only ever
// knew about indices inside the BED's own acmod, so a dependent substream's
// channels always came back non-directional and simply never appeared on the
// ring, ceiling or otherwise. LFE/LFE2 stay non-directional; every other
// location gets a plausible placement instead of vanishing.
// ---------------------------------------------------------------------------

std::optional<double> location_azimuth_deg(ac3::eac3::chanmap::Location location) {
    using ac3::eac3::chanmap::Location;
    switch (location) {
        case Location::kLeft: return 30.0;
        case Location::kCentre: return 0.0;
        case Location::kRight: return -30.0;
        case Location::kLeftSurround: return 110.0;
        case Location::kRightSurround: return -110.0;
        case Location::kLc: return 15.0;
        case Location::kRc: return -15.0;
        case Location::kLrs: return 135.0;
        case Location::kRrs: return -135.0;
        case Location::kCs: return 180.0;
        case Location::kTs: return 180.0;    // ceiling: overhead-rear
        case Location::kLsd: return 90.0;
        case Location::kRsd: return -90.0;
        case Location::kLw: return 60.0;
        case Location::kRw: return -60.0;
        case Location::kVhl: return 45.0;    // ceiling: front height
        case Location::kVhr: return -45.0;   // ceiling: front height
        case Location::kVhc: return 0.0;     // ceiling: centre height
        case Location::kLts: return 110.0;   // ceiling: rear height
        case Location::kRts: return -110.0;  // ceiling: rear height
        case Location::kLfe2:
        case Location::kLfe:
            return std::nullopt;
    }
    return std::nullopt;
}

// Where a bed-pinned object sits in the room so that pan_room() lands its
// energy on exactly that speaker. pan_room reads azimuth as
// atan2(0.5 - x, 0.5 - y) (CCW from front), so a point 0.45 out from the
// listener along a speaker's own azimuth pans entirely into that speaker -
// VBAP at a speaker's exact angle puts the whole gain there. Only the five
// 5.1 ring positions are reachable this way; object mode's bed is always
// 5.1, and setAssignment's own vocabulary check keeps anything wider out.
ac3::oba::Position speaker_pin_position(double azimuth_deg) {
    const double radians = azimuth_deg * std::numbers::pi / 180.0;
    return {.x = 0.5 - 0.45 * std::sin(radians), .y = 0.5 - 0.45 * std::cos(radians), .z = 0.0};
}

// Where a failed or cancelled run's frames land when the keep-partial
// preference is on: ".partial" spliced in before the suffix, so "out.ec3"
// keeps its half-finished take as "out.partial.ec3" - named and kept, never
// silently discarded, and never squatting on the name the finished file was
// going to have.
QString partial_output_path(const QString& path) {
    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
    const qsizetype slash = std::max(path.lastIndexOf(QLatin1Char('/')),
                                     path.lastIndexOf(QLatin1Char('\\')));
    if (dot > slash) {
        return path.left(dot) + QStringLiteral(".partial") + path.mid(dot);
    }
    return path + QStringLiteral(".partial");
}

// The two soundfield rings: everything overhead goes on the ceiling plan,
// everything else - however far back or wide - stays on the ear-level one.
bool is_ceiling_location(ac3::eac3::chanmap::Location location) {
    using ac3::eac3::chanmap::Location;
    switch (location) {
        case Location::kTs:
        case Location::kVhl:
        case Location::kVhr:
        case Location::kVhc:
        case Location::kLts:
        case Location::kRts:
            return true;
        default:
            return false;
    }
}

// Interleaves `channels` (one vector per decoded channel, AC-3/E-AC-3 coded
// order) into WAV/Windows speaker order for playback, reading order[i] as
// which channels[] entry belongs at interleaved position i - the same
// permutation plan::wav_order/ac3::io::wav_channel_order already produce for
// exactly this AC-3-order-vs-playback-order reconciliation (mirrors
// ac3cli's run_live, which monitors a live session the same way).
std::vector<float> interleave_reordered(std::span<const std::vector<float>> channels,
                                        std::span<const std::size_t> order) {
    const auto frame_count = channels.empty() ? std::size_t{0} : channels.front().size();
    std::vector<float> out(frame_count * order.size());
    for (std::size_t i = 0; i < frame_count; ++i) {
        for (std::size_t ch = 0; ch < order.size(); ++ch) {
            out[i * order.size() + ch] = channels[order[ch]][i];
        }
    }
    return out;
}

}  // namespace

struct EncoderController::Source {
    ac3::io::WavData wav;
    // "orbit51.wav" (or the raw path if it was never a local file) - what
    // sourceModel/sourceShapes label this source with, and what a repeated
    // add of the same file overwrites rather than duplicates would need to
    // disambiguate.
    QString path;
};

EncoderController::EncoderController(QObject* parent) : QObject(parent) {
    // Every encodeFinished emission (there are several call sites, one per
    // early-exit failure plus the two workers' own completions) settles
    // whichever run startRun() most recently opened, without each site
    // having to say so itself.
    connect(this, &EncoderController::encodeFinished, this, &EncoderController::finishRun);
    // The trailing edge of notifyObjectsChangedSoon()'s coalescing window.
    object_notify_timer_.setSingleShot(true);
    object_notify_timer_.setInterval(16);
    connect(&object_notify_timer_, &QTimer::timeout, this, [this] {
        object_notify_elapsed_.restart();
        emit objectsChanged();
    });
    object_notify_elapsed_.start();
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

QVariantList EncoderController::objectModel() const {
    QVariantList out;
    // Object i is the i-th channel the assignments send to "obj" (every
    // channel, when nothing is assigned) - the same mapping encodeObjects
    // uses, so the list names exactly what will ride as objects.
    const auto dynamic = dynamicObjectChannels();
    for (std::size_t i = 0; i < object_configs_.size(); ++i) {
        const auto& config = object_configs_[i];
        const auto keyframes = sortedKeyframes(static_cast<int>(i));
        QVariantMap row;
        row[QStringLiteral("index")] = static_cast<int>(i);
        // Objects are a channel, one each, so the honest name for where one
        // comes from is which channel it is - and, once more than one
        // source is loaded, which FILE that channel is in (see
        // objectSourceLabel's own comment).
        row[QStringLiteral("sourceLabel")] =
            objectSourceLabel(i < dynamic.size() ? dynamic[i] : i);
        row[QStringLiteral("x")] = config.x;
        row[QStringLiteral("y")] = config.y;
        row[QStringLiteral("z")] = config.z;
        row[QStringLiteral("lfeSend")] = config.lfe_send;
        row[QStringLiteral("hasPath")] = !keyframes.empty();
        row[QStringLiteral("keyCount")] = static_cast<int>(keyframes.size());
        out.append(row);
    }
    return out;
}

QString EncoderController::channelShapeName() const {
    if (isDualMono()) {
        return QStringLiteral("1+1");
    }
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
    // Ch1 and Ch2 - always exactly two positions, independent of the
    // 16-position budget the location-mask beds below share.
    if (isDualMono()) {
        return 2;
    }
    return ac3::eac3::chanmap::channel_count(currentLocationMask());
}

QString EncoderController::channelLocationsText() const {
    // "1+1" is a named layout, the same token ac3cli's own [layout]
    // argument takes for it (see resolve_layout()) - not a Table E2.5
    // location list, so format_channels()'s comma-separated form has
    // nothing to format here.
    if (isDualMono()) {
        return QStringLiteral("1+1");
    }
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

QString EncoderController::vbrToken() const {
    std::optional<ac3::eac3::VbrConfig> vbr;
    if (vbr_enabled_) {
        ac3::eac3::VbrConfig config;
        config.quality = static_cast<double>(vbr_quality_) / 100.0;
        if (vbr_min_enabled_) {
            config.min_kbps = vbr_min_kbps_;
        }
        if (vbr_max_enabled_) {
            config.max_kbps = vbr_max_kbps_;
        }
        vbr = config;
    }
    return to_qstring(plan::format_vbr(vbr));
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

void EncoderController::setVbrEnabled(bool on) {
    if (on == vbr_enabled_ || busy_) {
        return;
    }
    vbr_enabled_ = on;
    emit planChanged();
}

void EncoderController::setVbrQuality(int value) {
    const int clamped = std::clamp(value, 0, 100);
    if (clamped == vbr_quality_ || busy_) {
        return;
    }
    vbr_quality_ = clamped;
    emit planChanged();
}

void EncoderController::setVbrMinEnabled(bool on) {
    if (on == vbr_min_enabled_ || busy_) {
        return;
    }
    vbr_min_enabled_ = on;
    emit planChanged();
}

void EncoderController::setVbrMinKbps(int value) {
    const auto clamped = static_cast<std::uint32_t>(std::clamp(value, 32, 6144));
    if (clamped == vbr_min_kbps_ || busy_) {
        return;
    }
    vbr_min_kbps_ = clamped;
    emit planChanged();
}

void EncoderController::setVbrMaxEnabled(bool on) {
    if (on == vbr_max_enabled_ || busy_) {
        return;
    }
    vbr_max_enabled_ = on;
    emit planChanged();
}

void EncoderController::setVbrMaxKbps(int value) {
    const auto clamped = static_cast<std::uint32_t>(std::clamp(value, 32, 6144));
    if (clamped == vbr_max_kbps_ || busy_) {
        return;
    }
    vbr_max_kbps_ = clamped;
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
    if (acmod == ac3::Acmod::kDualMono) {
        // "Selecting it clears the LFE, extras and objects" - objects are
        // already unreachable here (atmos_enabled_ already refused above,
        // same as it does for every other bed change), so LFE and extras
        // are the only state left to clear.
        bed_lfe_ = false;
        extras_mask_ = 0;
    }
    emit planChanged();
    refreshRouting();
}

void EncoderController::setBedLfe(bool on) {
    if (busy_ || atmos_enabled_ || isDualMono() || on == bed_lfe_) {
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
        // Adding any extra under plain AC-3 promotes the codec - the extras
        // decide the codec, never the reverse, exactly as a preset needing
        // a dependent substream already does.
        if (!checked && codec_ == plan::Codec::kAc3) {
            codec_ = plan::Codec::kEac3;
            emit outputChanged();
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
        ac3::Acmod acmod;
        bool lfe;
        std::uint16_t extras;
    };
    // A preset is a starting point for the general model, not a separate one
    // - see the file comment on kBeds/kExtras for why this goes through
    // chanmap::allocate() rather than the legacy LayoutId table (fewer
    // transmitted channels for 7.1/7.1.4 than the old hand-picked
    // k71Rear/kTopQuad dependents, same rendered speakers). "stereo" is the
    // one preset not built on the widest bed - it exists so the guided
    // setup's "a laptop / a stereo pair" card writes the same tables as
    // everything else.
    static constexpr std::array<Preset, 7> kPresets{{
        {"stereo", ac3::Acmod::k2_0, false, 0},
        {"5.1", ac3::Acmod::k3_2, true, 0},
        {"7.1", ac3::Acmod::k3_2, true, ac3::eac3::chanmap::kLrsRrsBit},
        {"5.1.4", ac3::Acmod::k3_2, true,
         static_cast<std::uint16_t>(ac3::eac3::chanmap::kVhlVhrBit | ac3::eac3::chanmap::kLtsRtsBit)},
        {"7.1.4", ac3::Acmod::k3_2, true,
         static_cast<std::uint16_t>(ac3::eac3::chanmap::kLrsRrsBit | ac3::eac3::chanmap::kVhlVhrBit |
                                    ac3::eac3::chanmap::kLtsRtsBit)},
        {"5.2", ac3::Acmod::k3_2, true, ac3::eac3::chanmap::kLfe2Bit},
        {"7.2.4", ac3::Acmod::k3_2, true,
         static_cast<std::uint16_t>(ac3::eac3::chanmap::kLrsRrsBit | ac3::eac3::chanmap::kVhlVhrBit |
                                    ac3::eac3::chanmap::kLtsRtsBit | ac3::eac3::chanmap::kLfe2Bit)},
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
        bed_acmod_ = preset.acmod;
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

void EncoderController::setDialnorm2(int value) {
    const int clamped = std::clamp(value, 1, 31);
    if (clamped == meta_.dialnorm2) {
        return;
    }
    meta_.dialnorm2 = clamped;
    emit planChanged();
}

void EncoderController::setMeasureDialnorm2(bool on) {
    if (on == meta_.measure_dialnorm2) {
        return;
    }
    meta_.measure_dialnorm2 = on;
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

void EncoderController::setSelectedObjectIndex(int index) {
    if (index < 0 || index >= object_count_ || index == selected_object_index_) {
        return;
    }
    selected_object_index_ = index;
    emit objectsChanged();
}

void EncoderController::setObjectPosition(int objectIndex, double x, double y, double z) {
    if (objectIndex < 0 || objectIndex >= static_cast<int>(object_configs_.size())) {
        return;
    }
    auto& config = object_configs_[static_cast<std::size_t>(objectIndex)];
    config.x = std::clamp(x, 0.0, 1.0);
    config.y = std::clamp(y, 0.0, 1.0);
    config.z = std::clamp(z, -1.0, 1.0);
    {
        // Kept current even when nothing is live: cheaper than branching on
        // liveActive from the GUI thread, and the worker only ever reads this
        // when it is actually running.
        std::lock_guard lock(live_object_mutex_);
        if (objectIndex < static_cast<int>(live_object_snapshot_.size())) {
            live_object_snapshot_[static_cast<std::size_t>(objectIndex)] = config;
        }
    }
    notifyObjectsChangedSoon();
}

void EncoderController::setObjectLfeSend(int objectIndex, double value) {
    if (objectIndex < 0 || objectIndex >= static_cast<int>(object_configs_.size())) {
        return;
    }
    auto& config = object_configs_[static_cast<std::size_t>(objectIndex)];
    config.lfe_send = std::clamp(value, 0.0, 1.0);
    {
        std::lock_guard lock(live_object_mutex_);
        if (objectIndex < static_cast<int>(live_object_snapshot_.size())) {
            live_object_snapshot_[static_cast<std::size_t>(objectIndex)] = config;
        }
    }
    notifyObjectsChangedSoon();
}

void EncoderController::notifyObjectsChangedSoon() {
    // Leading edge: a fresh gesture (or a slow one) still notifies on the
    // spot, so a single click never waits a frame. Inside the window, the
    // trailing single-shot carries the newest state out once.
    if (!object_notify_timer_.isActive() && object_notify_elapsed_.elapsed() >= 16) {
        object_notify_elapsed_.restart();
        emit objectsChanged();
        return;
    }
    if (!object_notify_timer_.isActive()) {
        object_notify_timer_.start();
    }
}

std::vector<EncoderController::ObjectConfig> EncoderController::liveObjectSnapshot() const {
    std::lock_guard lock(live_object_mutex_);
    return live_object_snapshot_;
}

void EncoderController::setObjectPathKeyframes(int objectIndex, const QVariantList& keyframes) {
    if (keyframes.isEmpty()) {
        object_keyframes_.remove(objectIndex);
        emit objectsChanged();
        return;
    }
    std::vector<ac3::oba::Keyframe> parsed;
    parsed.reserve(static_cast<std::size_t>(keyframes.size()));
    for (const auto& entry : keyframes) {
        const auto map = entry.toMap();
        parsed.push_back({.time_s = map.value(QStringLiteral("time"), 0.0).toDouble(),
                          .position = {.x = map.value(QStringLiteral("x"), 0.5).toDouble(),
                                       .y = map.value(QStringLiteral("y"), 0.5).toDouble(),
                                       .z = map.value(QStringLiteral("z"), 0.0).toDouble()},
                          .gain = map.value(QStringLiteral("gain"), 1.0).toDouble(),
                          .lfe_send = map.value(QStringLiteral("lfeSend"), 0.0).toDouble()});
    }
    object_keyframes_[objectIndex] = std::move(parsed);
    emit objectsChanged();
}

void EncoderController::clearObjectPath(int objectIndex) {
    if (object_keyframes_.remove(objectIndex)) {
        emit objectsChanged();
    }
}

std::vector<ac3::oba::Keyframe> EncoderController::sortedKeyframes(int objectIndex) const {
    const auto found = object_keyframes_.constFind(objectIndex);
    if (found == object_keyframes_.constEnd()) {
        return {};
    }
    auto keyframes = *found;
    std::ranges::sort(keyframes, {}, &ac3::oba::Keyframe::time_s);
    return keyframes;
}

QVariantList EncoderController::objectKeyframes(int objectIndex) const {
    QVariantList out;
    for (const auto& key : sortedKeyframes(objectIndex)) {
        out.append(QVariantMap{
            {QStringLiteral("time"), key.time_s},
            {QStringLiteral("x"), key.position.x},
            {QStringLiteral("y"), key.position.y},
            {QStringLiteral("z"), key.position.z},
            {QStringLiteral("gain"), key.gain},
            {QStringLiteral("lfeSend"), key.lfe_send},
        });
    }
    return out;
}

void EncoderController::addObjectKeyframe(int objectIndex, double timeS) {
    if (objectIndex < 0 || objectIndex >= static_cast<int>(object_configs_.size())) {
        return;
    }
    const auto& config = object_configs_[static_cast<std::size_t>(objectIndex)];
    auto keyframes = sortedKeyframes(objectIndex);
    // Same moment, not the same float: two cues a hundredth of a second apart
    // are not a user trying to nudge one, they are a mis-click.
    constexpr double kSameInstant = 0.01;
    const auto existing = std::ranges::find_if(keyframes, [&](const ac3::oba::Keyframe& key) {
        return std::abs(key.time_s - timeS) < kSameInstant;
    });
    ac3::oba::Keyframe key{.time_s = timeS,
                           .position = {.x = config.x, .y = config.y, .z = config.z},
                           .gain = 1.0,
                           .lfe_send = config.lfe_send};
    if (existing != keyframes.end()) {
        *existing = key;
    } else {
        keyframes.push_back(key);
    }
    object_keyframes_[objectIndex] = std::move(keyframes);
    emit objectsChanged();
}

void EncoderController::removeObjectKeyframe(int objectIndex, double timeS) {
    auto keyframes = sortedKeyframes(objectIndex);
    constexpr double kSameInstant = 0.01;
    const auto before = keyframes.size();
    std::erase_if(keyframes, [&](const ac3::oba::Keyframe& key) {
        return std::abs(key.time_s - timeS) < kSameInstant;
    });
    if (keyframes.size() == before) {
        return;
    }
    if (keyframes.empty()) {
        object_keyframes_.remove(objectIndex);
    } else {
        object_keyframes_[objectIndex] = std::move(keyframes);
    }
    emit objectsChanged();
}

QVariantMap EncoderController::evaluateObjectPath(int objectIndex, double timeS) const {
    QVariantMap out;
    if (objectIndex < 0 || objectIndex >= static_cast<int>(object_configs_.size())) {
        return out;
    }
    const auto& config = object_configs_[static_cast<std::size_t>(objectIndex)];
    ac3::oba::Position position{.x = config.x, .y = config.y, .z = config.z};
    double gain = 1.0;
    double lfe_send = config.lfe_send;

    const auto keyframes = sortedKeyframes(objectIndex);
    if (!keyframes.empty()) {
        if (const auto path = ac3::oba::KeyframePath::create(keyframes)) {
            const auto placement = path->evaluate(timeS);
            position = placement.position;
            gain = placement.gain;
            lfe_send = placement.lfe_send;
        }
    }
    out[QStringLiteral("x")] = position.x;
    out[QStringLiteral("y")] = position.y;
    out[QStringLiteral("z")] = position.z;
    out[QStringLiteral("gain")] = gain;
    out[QStringLiteral("lfeSend")] = lfe_send;
    return out;
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
    if (atmos_enabled_) {
        // p.layout stays the default 5.1 above; nothing else to set.
    } else if (isDualMono()) {
        // 1+1 names a layout, not a location mask - custom_locations has no
        // way to express "two independent programmes" (see isDualMono()'s
        // own comment), so this is the one bed that goes through
        // plan.layout instead, the same as ac3cli's own resolve_layout()
        // does for a named "1+1" argument.
        p.layout = plan::LayoutId::kDualMono;
    } else {
        p.custom_locations = currentLocationMask();
    }
    if (source_) {
        if (const auto rate = to_sample_rate_for_file(source_->wav.sample_rate, codec_)) {
            p.sample_rate = *rate;
        }
    }
    // Gated here rather than trusted from vbrEnabled() alone: a user can
    // switch codec (or turn object mode on) after ticking VBR, and this is
    // what keeps the plan internally consistent regardless - validate()
    // rejects vbr set alongside AC-3 outright (PlanError::kVbrNeedsEac3),
    // so a stale vbr_enabled_ left over from an E-AC-3 session would
    // otherwise refuse an AC-3 encode for no reason visible on screen.
    if (vbr_enabled_ && codec_ == plan::Codec::kEac3 && !atmos_enabled_) {
        ac3::eac3::VbrConfig vbr;
        vbr.quality = static_cast<double>(vbr_quality_) / 100.0;
        if (vbr_min_enabled_) {
            vbr.min_kbps = vbr_min_kbps_;
        }
        if (vbr_max_enabled_) {
            vbr.max_kbps = vbr_max_kbps_;
        }
        p.vbr = vbr;
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

std::vector<plan::SourceShape> EncoderController::sourceShapes() const {
    std::vector<plan::SourceShape> shapes;
    if (!source_) {
        return shapes;
    }
    shapes.push_back({.channels = source_->wav.channels.size(),
                      .label = QFileInfo(source_->path).fileName().toStdString()});
    for (const auto& extra : extra_sources_) {
        shapes.push_back({.channels = extra->wav.channels.size(),
                          .label = QFileInfo(extra->path).fileName().toStdString()});
    }
    return shapes;
}

QString EncoderController::objectSourceLabel(std::size_t flatIndex) const {
    const auto shapes = sourceShapes();
    // Exactly one source: unchanged from what objectModel() has always
    // shown - there is nothing a filename would add over a plain channel
    // number when only one file is in play.
    if (shapes.size() <= 1) {
        return QStringLiteral("Ch %1").arg(flatIndex + 1);
    }
    std::size_t base = 0;
    for (const auto& shape : shapes) {
        if (flatIndex < base + shape.channels) {
            return QStringLiteral("%1 ch %2")
                .arg(QString::fromStdString(shape.label))
                .arg(flatIndex - base + 1);
        }
        base += shape.channels;
    }
    // Past every loaded source's channels - object_count_ is capped to the
    // sum of them (see refreshAfterSourceListChange), so this is defensive
    // only, not a path real state reaches.
    return QStringLiteral("Ch %1").arg(flatIndex + 1);
}

std::vector<std::size_t> EncoderController::dynamicObjectChannels() const {
    std::vector<std::size_t> out;
    const auto shapes = sourceShapes();
    std::size_t flat = 0;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        for (std::size_t c = 0; c < shapes[s].channels; ++c, ++flat) {
            if (!has_explicit_assignment_ ||
                assignment_.at(s, c).kind == plan::DestinationKind::kObject) {
                out.push_back(flat);
            }
        }
    }
    return out;
}

std::vector<std::pair<std::size_t, ac3::eac3::chanmap::Location>>
EncoderController::pinnedObjectChannels() const {
    std::vector<std::pair<std::size_t, ac3::eac3::chanmap::Location>> out;
    if (!has_explicit_assignment_) {
        return out;
    }
    const auto shapes = sourceShapes();
    std::size_t flat = 0;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        for (std::size_t c = 0; c < shapes[s].channels; ++c, ++flat) {
            const auto dest = assignment_.at(s, c);
            if (dest.kind == plan::DestinationKind::kLocation) {
                out.emplace_back(flat, dest.location);
            }
        }
    }
    return out;
}

void EncoderController::recomputeObjectCount() {
    object_count_ =
        static_cast<int>(std::min<std::size_t>(dynamicObjectChannels().size(), 15));
    refreshObjectConfigs();
}

std::optional<plan::Routing> EncoderController::routingForSources(const plan::ChannelPlan& target,
                                                                   const plan::Plan& p) const {
    if (!source_) {
        return std::nullopt;
    }
    if (!has_explicit_assignment_) {
        if (!extra_sources_.empty()) {
            // Automatic panning only ever meant something for one source;
            // several with no explicit assignment is refused the same way
            // ac3cli's src=/map= refuses it (see main.cpp's
            // routing_for_sources) rather than inventing an automatic
            // multi-file blend nothing else here defines.
            return std::nullopt;
        }
        return plan::route(target, source_->wav.channels.size(), p.meta.cmixlev,
                           p.meta.surmixlev);
    }
    const auto shapes = sourceShapes();
    return target.bed_acmod == ac3::Acmod::kDualMono
              ? plan::dual_mono_routing(shapes, assignment_)
              : plan::route(target, shapes, assignment_);
}

void EncoderController::refreshRouting() {
    refreshRoutingSummary();
    // routingChanged (emitted above, on every path out of the summary) is
    // also plannedChannels' NOTIFY - the fed set is a routing fact. The
    // meter preview then follows the same plan the strings just described.
    previewPlanMeters();
}

void EncoderController::refreshRoutingSummary() {
    const auto p = currentPlan();
    const auto label = effectiveLabel();

    if (atmos_enabled_) {
        const auto npinned = pinnedObjectChannels().size();
        if (object_count_ > 0 && npinned > 0) {
            routing_summary_ = QStringLiteral("%1 objects and %2 bed-fed channels over a 5.1 "
                                              "bed; a legacy decoder hears the bed.")
                                   .arg(object_count_)
                                   .arg(static_cast<int>(npinned));
        } else if (object_count_ > 0) {
            routing_summary_ =
                QStringLiteral("%1 objects over a 5.1 bed; a legacy decoder hears the bed.")
                    .arg(object_count_);
        } else if (npinned > 0) {
            routing_summary_ = QStringLiteral("%1 channels feed the 5.1 bed and nothing rides "
                                              "as an object — send a channel to \"an object\" "
                                              "or turn object mode off.")
                                   .arg(static_cast<int>(npinned));
        } else {
            routing_summary_ =
                QStringLiteral("Each source channel becomes an object over a 5.1 bed.");
        }
        emit routingChanged();
        return;
    }

    if (!source_) {
        routing_summary_ = QStringLiteral("%1 · %2").arg(label, layoutDetail());
        emit routingChanged();
        return;
    }

    const auto cp = effectiveChannelPlan();
    if (!has_explicit_assignment_ && !extra_sources_.empty()) {
        routing_summary_ = QStringLiteral("%1 sources loaded — set an assignment for each "
                                          "channel below.")
                               .arg(static_cast<int>(extra_sources_.size()) + 1);
        emit routingChanged();
        return;
    }
    const auto routing = routingForSources(cp, p);
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
    if (busy) {
        // A run owns the meters from here; any meter preview still rendering
        // answers a question nobody is asking any more.
        preview_generation_.fetch_add(1, std::memory_order_relaxed);
    }
    emit busyChanged();
}

void EncoderController::startRun(const QString& path) {
    double seconds = 0.0;
    if (source_ && source_->wav.sample_rate > 0) {
        seconds = static_cast<double>(source_->wav.frame_count()) /
                  static_cast<double>(source_->wav.sample_rate);
    }
    QVariantMap run;
    run[QStringLiteral("id")] = next_run_id_;
    run[QStringLiteral("filename")] = QFileInfo(path).fileName();
    run[QStringLiteral("bitrateKbps")] = bitrate_kbps_;
    run[QStringLiteral("durationText")] =
        QStringLiteral("%1:%2")
            .arg(static_cast<int>(seconds) / 60)
            .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'));
    run[QStringLiteral("status")] = QStringLiteral("encoding");
    run[QStringLiteral("sizeText")] = QString();
    run[QStringLiteral("detail")] = QString();
    // A VBR run has no target rate to show while it is still running - only
    // the quality it is aiming for. finishRun() replaces this with the real
    // avg/min/max once the run's actual frame sizes are known.
    run[QStringLiteral("rateText")] =
        (vbr_enabled_ && codec_ == plan::Codec::kEac3 && !atmos_enabled_)
            ? QStringLiteral("VBR q%1").arg(vbr_quality_)
            : QStringLiteral("%1 kbps").arg(bitrate_kbps_);
    // Newest first, matching the run strip's own reading order.
    runs_.prepend(run);
    current_run_id_ = next_run_id_;
    ++next_run_id_;
    emit runsChanged();
}

void EncoderController::finishRun(bool ok, const QString& message) {
    if (current_run_id_ < 0) {
        return;
    }
    for (auto& variant : runs_) {
        auto run = variant.toMap();
        if (run.value(QStringLiteral("id")).toInt() != current_run_id_) {
            continue;
        }
        // The same text setStatus() already put on screen for a cancelled
        // run - read back rather than re-decided, so the run chip and the
        // status line that preceded it can never disagree about which of
        // the three this was.
        const bool cancelled = message.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive);
        run[QStringLiteral("status")] = ok ? QStringLiteral("done")
                                       : (cancelled ? QStringLiteral("cancelled")
                                                    : QStringLiteral("failed"));
        run[QStringLiteral("detail")] = message;
        static const QRegularExpression kSizePattern(QStringLiteral(R"(\(([0-9.]+ [KMG]B)\))"));
        const auto match = kSizePattern.match(message);
        if (match.hasMatch()) {
            run[QStringLiteral("sizeText")] = match.captured(1);
        }
        if (!pending_rate_text_.isEmpty()) {
            run[QStringLiteral("rateText")] = pending_rate_text_;
        }
        variant = run;
        break;
    }
    pending_rate_text_.clear();
    current_run_id_ = -1;
    emit runsChanged();
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
        bool any_lfe_send = false;
        for (const auto& config : object_configs_) {
            const auto gains = ac3::spatial::pan_room(config.x, config.y);
            for (std::size_t ch = 0; ch < gains.size(); ++ch) {
                fed[ch] = fed[ch] || gains[ch] != 0.0;
            }
            any_lfe_send = any_lfe_send || config.lfe_send > 0.0;
        }
        // An object reaches the LFE only through the explicit send: there is
        // no direction that points at it (§6.3.2.2 bypasses it entirely).
        fed[5] = any_lfe_send;
        // Bed-pinned channels feed wherever their pin position pans - the
        // same pan_room answer encodeObjects' static keyframe will get.
        for (const auto& [flat, location] : pinnedObjectChannels()) {
            using ac3::eac3::chanmap::Location;
            if (location == Location::kLfe || location == Location::kLfe2) {
                fed[5] = true;
                continue;
            }
            if (const auto azimuth = location_azimuth_deg(location)) {
                const auto pin = speaker_pin_position(*azimuth);
                const auto gains = ac3::spatial::pan_room(pin.x, pin.y);
                for (std::size_t ch = 0; ch < gains.size(); ++ch) {
                    fed[ch] = fed[ch] || gains[ch] != 0.0;
                }
            }
        }
        return fed;
    }
    if (!source_) {
        return std::vector<bool>(count, true);
    }
    const auto routing = routingForSources(cp, p);
    if (!routing) {
        // No routing to read: with one untouched source that is the harmless
        // empty state (automatic routing will feed everything), but several
        // sources with nothing assigned genuinely feed NOTHING yet, and the
        // display saying otherwise would contradict its own warnings.
        return std::vector<bool>(count, extra_sources_.empty() && !has_explicit_assignment_);
    }
    std::vector<bool> fed(count, false);
    for (int c = 0; c < routing->coded_channels; ++c) {
        for (int s = 0; s < routing->source_channels && !fed[static_cast<std::size_t>(c)]; ++s) {
            fed[static_cast<std::size_t>(c)] = routing->at(c, s) != 0.0;
        }
    }
    return fed;
}

QVariantList EncoderController::channelMeta() const {
    QVariantList out;
    out.reserve(channel_names_.size());
    for (qsizetype ch = 0; ch < channel_names_.size(); ++ch) {
        const auto at = static_cast<std::size_t>(ch);
        const bool has_location = at < channel_locations_.size();
        const auto azimuth = has_location ? location_azimuth_deg(channel_locations_[at])
                                          : std::nullopt;
        out.append(QVariantMap{
            {QStringLiteral("name"), channel_names_[ch]},
            {QStringLiteral("azimuthDeg"), azimuth.value_or(0.0)},
            {QStringLiteral("directional"), azimuth.has_value()},
            {QStringLiteral("ceiling"),
             has_location && is_ceiling_location(channel_locations_[at])},
            {QStringLiteral("replaced"), at < channel_replaced_.size() && channel_replaced_[at]},
            {QStringLiteral("fed"), at >= channel_fed_.size() || channel_fed_[at]},
        });
    }
    return out;
}

QVariantList EncoderController::plannedChannels() const {
    QVariantList out;
    if (isDualMono() && !atmos_enabled_) {
        for (int programme = 1; programme <= 2; ++programme) {
            out.append(QVariantMap{
                {QStringLiteral("name"), QStringLiteral("Program %1").arg(programme)},
                {QStringLiteral("token"), QStringLiteral("p%1").arg(programme)},
                {QStringLiteral("azimuthDeg"), 0.0},
                {QStringLiteral("directional"), false},
                {QStringLiteral("ceiling"), false},
                {QStringLiteral("replaced"), false},
                {QStringLiteral("fed"), true},
            });
        }
        return out;
    }
    const auto cp = atmos_enabled_ ? plan::channel_plan_for(plan::LayoutId::k51)
                                   : effectiveChannelPlan();
    const auto coded = plan::coded_channels(cp);
    const auto names = plan::coded_channel_names(cp);
    const auto fed = fedChannels();
    for (std::size_t ch = 0; ch < coded.size(); ++ch) {
        const auto location = coded[ch].location;
        const auto azimuth = location_azimuth_deg(location);
        const bool replaced =
            coded[ch].bed && std::ranges::any_of(coded, [&](const auto& other) {
                return !other.bed && other.location == location;
            });
        out.append(QVariantMap{
            {QStringLiteral("name"),
             ch < names.size() ? QString::fromStdString(names[ch]) : QString()},
            {QStringLiteral("token"), to_qstring(ac3::eac3::chanmap::name(location))},
            {QStringLiteral("azimuthDeg"), azimuth.value_or(0.0)},
            {QStringLiteral("directional"), azimuth.has_value()},
            {QStringLiteral("ceiling"), is_ceiling_location(location)},
            {QStringLiteral("replaced"), replaced},
            {QStringLiteral("fed"), ch >= fed.size() || fed[ch]},
        });
    }
    return out;
}

void EncoderController::previewPlanMeters() {
    // Whatever happens below, any preview still rendering answers a plan
    // that just changed - a stale one landing later would put the OLD
    // source list's levels under the NEW layout's labels.
    preview_generation_.fetch_add(1, std::memory_order_relaxed);
    if (busy_ || !source_) {
        return;
    }
    const auto p = currentPlan();

    // Labels and locations exactly as the encode workers will set them, fed
    // flags included - immediately, because none of this touches audio.
    if (atmos_enabled_) {
        const auto coded = plan::coded_channels(plan::LayoutId::k51);
        const auto names = plan::coded_channel_names(plan::LayoutId::k51);
        QStringList labels;
        for (const auto& name : names) {
            labels.append(QString::fromStdString(name));
        }
        setLayout(ac3::Acmod::k3_2, true, labels, QStringLiteral("5.1 bed"), coded,
                  fedChannels());
        // No audio preview in object mode: what the bed will hold is a
        // per-frame panning question the encode itself answers. The fed
        // flags above already say which positions the objects reach.
        return;
    }

    const auto cp = effectiveChannelPlan();
    QStringList labels;
    if (isDualMono()) {
        labels = {QStringLiteral("Program 1"), QStringLiteral("Program 2")};
    } else {
        for (const auto& name : plan::coded_channel_names(cp)) {
            labels.append(QString::fromStdString(name));
        }
    }
    setLayout(cp.bed_acmod, cp.bed_lfe, labels, effectiveLabel(), plan::coded_channels(cp),
              fedChannels());

    const auto routing = routingForSources(cp, p);
    if (!routing) {
        // Nothing honest to meter: several sources with nothing assigned
        // yet, or a plan the source cannot be routed onto. The silent bars
        // setLayout just published are the right display for both.
        return;
    }

    // Whole-programme levels through the actual routing, off the GUI thread
    // - the same per-frame render the encode will do, minus the encoder.
    // This is what lets the meters answer an assignment edit with real
    // numbers instead of going stale on whatever ran last.
    const int generation = preview_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::vector<std::shared_ptr<Source>> sources;
    sources.reserve(1 + extra_sources_.size());
    sources.push_back(source_);
    for (const auto& extra : extra_sources_) {
        sources.push_back(extra);
    }
    const auto sample_rate = source_->wav.sample_rate;
    const auto acmod = cp.bed_acmod;
    const auto lfe = cp.bed_lfe;
    std::ignore = QtConcurrent::run([this, generation, routing = *routing,
                                     sources = std::move(sources), sample_rate, acmod, lfe] {
        const auto coded_count = static_cast<std::size_t>(routing.coded_channels);
        ac3::analysis::LevelMeter meter{acmod, lfe, sample_rate,
                                        static_cast<int>(coded_count)};

        std::vector<std::span<const float>> planes;
        for (const auto& src : sources) {
            for (const auto& channel : src->wav.channels) {
                planes.emplace_back(channel);
            }
        }
        std::size_t total = 0;
        for (const auto& plane : planes) {
            total = std::max(total, plane.size());
        }

        std::vector<std::vector<float>> source_block(planes.size(),
                                                     std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::vector<float>> block(coded_count,
                                              std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> in;
        std::vector<std::span<float>> out;
        std::vector<std::span<const float>> metered(coded_count);
        for (auto& channel : source_block) {
            in.emplace_back(channel);
        }
        for (auto& channel : block) {
            out.emplace_back(channel);
        }

        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            // A newer preview, or a run starting, makes this answer stale -
            // stop paying for it.
            if (generation != preview_generation_.load(std::memory_order_relaxed)) {
                return;
            }
            const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
            for (std::size_t ch = 0; ch < planes.size(); ++ch) {
                const auto len = planes[ch].size();
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    source_block[ch][static_cast<std::size_t>(i)] =
                        at < len ? planes[ch][at] : 0.0f;
                }
            }
            plan::render(routing, in, out, ac3::kSamplesPerFrame);
            for (std::size_t ch = 0; ch < coded_count; ++ch) {
                metered[ch] = std::span{block[ch]}.first(valid);
            }
            meter.process(metered);
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
        QMetaObject::invokeMethod(this, [this, generation, totals = std::move(totals)] {
            // busy_ means a run owns the meters now; the generation check
            // drops a preview that answered a plan nobody is looking at.
            if (busy_ || generation != preview_generation_.load(std::memory_order_relaxed)) {
                return;
            }
            publishLevels(totals);
        });
    });
}

void EncoderController::setLayout(ac3::Acmod acmod, bool lfe, const QStringList& names,
                                  const QString& label,
                                  const std::vector<ac3::plan::CodedChannel>& coded,
                                  const std::vector<bool>& fed) {
    acmod_ = acmod;
    lfe_ = lfe;
    channel_names_ = names;
    channel_fed_ = fed.empty()
                       ? std::vector<bool>(static_cast<std::size_t>(names.size()), true)
                       : fed;
    channel_fed_.resize(static_cast<std::size_t>(names.size()), true);

    channel_locations_.clear();
    channel_replaced_.clear();
    channel_locations_.reserve(coded.size());
    channel_replaced_.reserve(coded.size());
    for (const auto& channel : coded) {
        channel_locations_.push_back(channel.location);
        // A bed channel a dependent overwrites still exists and still reaches
        // a 5.1 decoder, but Rendered mode hides it - it is coded_channel_
        // names()'s own "(bed)" test, kept in step with it deliberately.
        const bool replaced =
            channel.bed && std::ranges::any_of(coded, [&](const auto& other) {
                return !other.bed && other.location == channel.location;
            });
        channel_replaced_.push_back(replaced);
    }

    layout_name_ = label;
    emit layoutChanged();
    // Start silent: leaving the previous source's levels under the new
    // source's labels would put a number against the wrong channel.
    publishLevels(
        std::vector<ac3::analysis::ChannelLevel>(static_cast<std::size_t>(names.size())));
}

void EncoderController::refreshObjectConfigs() {
    const auto count = static_cast<std::size_t>(std::max(object_count_, 0));
    const auto previous = object_configs_.size();
    if (count == previous) {
        return;
    }
    // Existing objects keep whatever position they were given; only the
    // newly-appeared ones need a default, spread out along x rather than
    // stacked on one point (the design brief's own complaint about the old
    // single-point-plus-spread model, where six objects "overlap into a
    // smear").
    object_configs_.resize(count);
    for (std::size_t i = previous; i < count; ++i) {
        const double offset = count < 2 ? 0.0
                                        : 0.3 * (2.0 * static_cast<double>(i) /
                                                     static_cast<double>(count - 1) - 1.0);
        object_configs_[i] = {.x = std::clamp(0.5 + offset, 0.0, 1.0),
                              .y = 0.15,
                              .z = 0.0,
                              .lfe_send = 0.15};
    }
    if (selected_object_index_ >= static_cast<int>(count)) {
        selected_object_index_ = count > 0 ? static_cast<int>(count) - 1 : 0;
    }
    // A path for an index the new count no longer has is meaningless - drop
    // it rather than let it silently reappear if the count later grows back
    // to cover that index again with motion authored for a different file.
    QList<int> stale;
    for (auto it = object_keyframes_.constBegin(); it != object_keyframes_.constEnd(); ++it) {
        if (it.key() >= static_cast<int>(count)) {
            stale.append(it.key());
        }
    }
    for (const auto key : stale) {
        object_keyframes_.remove(key);
    }
    // objectModel's own NOTIFY - every call site above sets object_count_
    // and calls this, but only ever emits sourceChanged() itself
    // afterwards. objectModel reads object_configs_/object_keyframes_, not
    // anything sourceChanged() already covers, so without this the Objects
    // tab's list, room plan and markers would keep showing whatever set of
    // objects was there before a new file (or a different-length one) was
    // loaded, until something else happened to touch an individual object
    // and emit this incidentally.
    emit objectsChanged();
}

void EncoderController::clearLayout() {
    channel_names_.clear();
    channel_locations_.clear();
    channel_replaced_.clear();
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
        const bool has_location = ch < channel_locations_.size();
        const auto location = has_location ? channel_locations_[ch]
                                           : ac3::eac3::chanmap::Location::kLeft;
        const auto azimuth = has_location ? location_azimuth_deg(location) : std::nullopt;
        const bool ceiling = has_location && is_ceiling_location(location);
        const bool replaced = ch < channel_replaced_.size() && channel_replaced_[ch];
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
            {QStringLiteral("ceiling"), ceiling},
            {QStringLiteral("replaced"), replaced},
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
            QString capability;
            if (device.supports_ac3_passthrough && device.supports_eac3_passthrough) {
                capability = QStringLiteral("AC-3 + E-AC-3 ready");
            } else if (device.supports_ac3_passthrough) {
                capability = QStringLiteral("AC-3 ready");
            } else if (device.supports_eac3_passthrough) {
                capability = QStringLiteral("E-AC-3 ready");
            } else {
                capability = device.supports_exclusive_pcm ? QStringLiteral("cannot bitstream")
                                                            : QStringLiteral("no exclusive access");
            }
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
        if (!bsid) {
            message = QStringLiteral("That file is too short to hold a syncframe.");
        } else {
            const bool eac3 = *bsid > 8;
            if (eac3 && !device.supports_eac3_passthrough) {
                message = QStringLiteral(
                              "\"%1\" will not accept E-AC-3 over IEC 61937. Only S/PDIF and "
                              "HDMI outputs can bitstream, and Dolby Digital Plus must be "
                              "enabled for the device in Sound settings.")
                              .arg(QString::fromStdString(device.name));
            } else if (!eac3 && !device.supports_ac3_passthrough) {
                message = QStringLiteral(
                              "\"%1\" will not accept AC-3 over IEC 61937. Only S/PDIF and "
                              "HDMI outputs can bitstream, and Dolby Digital must be enabled "
                              "for the device in Sound settings.")
                              .arg(QString::fromStdString(device.name));
            } else {
                // Access units for E-AC-3, since a dependent substream's
                // channels only reach the burst alongside the independent
                // one it extends (see run_play's own comment on this).
                const auto units =
                    eac3 ? ac3::split_access_units(stream) : ac3::split_frames(stream);
                if (!units || units->empty()) {
                    message = QStringLiteral("That file is not a valid %1 stream.")
                                  .arg(eac3 ? QStringLiteral("E-AC-3") : QStringLiteral("AC-3"));
                } else {
                    const auto rate = sample_rate_hz(static_cast<ac3::SampleRate>(
                        std::to_integer<std::uint32_t>((*units)[0][4]) >> 6));
                    ac3::sinks::PassthroughSink sink;
                    const auto started = sink.start(
                        device.id, rate,
                        eac3 ? ac3::sinks::BitstreamFormat::kEac3
                             : ac3::sinks::BitstreamFormat::kAc3);
                    if (!started) {
                        const auto why = ac3::sinks::describe(started.error());
                        message = QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()));
                    } else {
                        ac3::iec61937::Eac3BurstPacker eac3_packer;
                        for (const auto& unit : *units) {
                            std::vector<std::byte> burst;
                            if (eac3) {
                                auto result = eac3_packer.push(unit);
                                if (!result) {
                                    break;
                                }
                                if (!*result) {
                                    continue;  // accumulating; nothing to submit yet
                                }
                                burst = std::move(**result);
                            } else {
                                const auto wrapped = ac3::iec61937::wrap_frame(unit);
                                if (!wrapped) {
                                    break;
                                }
                                burst = *wrapped;
                            }
                            while (!sink.submit(burst)) {
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
            }
        }

        QMetaObject::invokeMethod(this, [this, message] {
            playing_ = false;
            emit playingChanged();
            setStatus(message);
        });
    });
}

void EncoderController::stopLiveSession() {
    stop_live_.store(true, std::memory_order_relaxed);
}

void EncoderController::setKeepPartialOutput(bool keep) {
    if (keep == keep_partial_output_) {
        return;
    }
    keep_partial_output_ = keep;
    emit keepPartialOutputChanged();
}

void EncoderController::settleReconnect() {
    if (!live_reconnecting_) {
        return;
    }
    live_reconnecting_ = false;
    emit liveReconnectingChanged();
}

void EncoderController::switchLiveLayout(const QString& presetName) {
    if (!live_active_ || !live_request_) {
        return;
    }
    if (atmos_enabled_) {
        // Object mode fixes the bed; the switcher never offers this, but a
        // property poke should not reach around the same rule.
        return;
    }
    if (live_writing_to_disk_) {
        setStatus(QStringLiteral("The take is being written to disk — stop the session and "
                                 "start a new take to change the layout."));
        return;
    }
    pending_live_relayout_ = presetName;
    setStatus(QStringLiteral("Switching to %1 — the stream stops, the receiver re-locks, and "
                             "about a second of audio is lost.")
                  .arg(presetName));
    stopLiveSession();
}

void EncoderController::startLiveSession(int captureDeviceIndex, bool monitor,
                                         int receiverDeviceIndex, bool writeToDisk,
                                         const QUrl& fileUrl) {
    if (busy_ || recording_ || live_active_) {
        return;
    }
    if (captureDeviceIndex < 0 ||
        static_cast<std::size_t>(captureDeviceIndex) >= devices_.size()) {
        setStatus(QStringLiteral("Choose a capture device first."));
        return;
    }
    const auto device = devices_[static_cast<std::size_t>(captureDeviceIndex)];
    if (!to_sample_rate(device.sample_rate)) {
        setStatus(QStringLiteral("\"%1\" runs at %2 Hz; AC-3/E-AC-3 need 32, 44.1 or 48 kHz.")
                      .arg(QString::fromStdString(device.name))
                      .arg(device.sample_rate));
        return;
    }

    auto p = currentPlan();
    p.sample_rate = *to_sample_rate(device.sample_rate);
    if (const auto bad = plan::validate(p)) {
        setStatus(to_qstring(plan::describe(*bad)));
        return;
    }

    const bool want_passthrough = receiverDeviceIndex >= 0;
    ac3::sinks::RenderDeviceInfo receiver{};
    if (want_passthrough) {
        if (static_cast<std::size_t>(receiverDeviceIndex) >= outputs_.size()) {
            setStatus(QStringLiteral("Choose a receiver device first."));
            return;
        }
        receiver = outputs_[static_cast<std::size_t>(receiverDeviceIndex)];
    }

    QString path;
    if (writeToDisk) {
        path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
        if (path.isEmpty()) {
            setStatus(QStringLiteral("Choose where to save the take first."));
            return;
        }
    }

    live_capture_ = std::make_unique<ac3::capture::Capture>();
    const auto started = live_capture_->start(device.id, device.kind);
    if (!started) {
        const auto why = ac3::capture::describe(started.error());
        live_capture_.reset();
        setStatus(QStringLiteral("Could not open \"%1\": %2")
                      .arg(QString::fromStdString(device.name),
                           QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()))));
        return;
    }

    // §8.3.2.2's decoder-side object gate means nobody downstream ever hears
    // our objects AS objects (see docs/design's own note on this - the real
    // decoder's gate is keyed, and forging that key is deliberately not
    // done), so an Atmos session's receiver leg is always just its 5.1 bed,
    // independent of what the device itself can bitstream.
    const bool eac3 = atmos_enabled_ || p.codec == plan::Codec::kEac3;
    const auto format =
        eac3 ? ac3::sinks::BitstreamFormat::kEac3 : ac3::sinks::BitstreamFormat::kAc3;
    bool passthrough_ok = false;
    if (want_passthrough) {
        const bool supports = eac3 ? receiver.supports_eac3_passthrough
                                   : receiver.supports_ac3_passthrough;
        if (!supports) {
            live_receiver_plan_text_ =
                QStringLiteral("\"%1\" cannot bitstream %2 over IEC 61937.")
                    .arg(QString::fromStdString(receiver.name),
                         eac3 ? QStringLiteral("E-AC-3") : QStringLiteral("AC-3"));
        } else {
            auto psink = std::make_unique<ac3::sinks::PassthroughSink>();
            const auto pstarted = psink->start(receiver.id, device.sample_rate, format);
            if (!pstarted) {
                const auto why = ac3::sinks::describe(pstarted.error());
                live_receiver_plan_text_ =
                    QStringLiteral("\"%1\" would not open: %2")
                        .arg(QString::fromStdString(receiver.name),
                             QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size())));
            } else {
                passthrough_ok = true;
                live_passthrough_sink_ = std::move(psink);
                live_receiver_plan_text_ =
                    atmos_enabled_
                        ? QStringLiteral("Dolby Digital Plus · 5.1 bed only · %1")
                              .arg(QString::fromStdString(receiver.name))
                        : QStringLiteral("%1 · %2 · %3")
                              .arg(eac3 ? QStringLiteral("Dolby Digital Plus")
                                        : QStringLiteral("Dolby Digital"))
                              .arg(channelShapeName(), QString::fromStdString(receiver.name));
            }
        }
    } else {
        live_receiver_plan_text_ = QStringLiteral("No passthrough this session.");
    }
    live_gap_ = want_passthrough && (!passthrough_ok || atmos_enabled_);

    bool monitor_ok = false;
    if (monitor) {
        auto msink = std::make_unique<ac3::sinks::MonitorSink>();
        const auto mstarted = msink->start(
            std::string{}, device.sample_rate,
            static_cast<std::uint16_t>(atmos_enabled_ ? 6 : plan::coded_channels(
                                                              effectiveChannelPlan()).size()));
        if (mstarted) {
            monitor_ok = true;
            live_monitor_sink_ = std::move(msink);
        }
    }

    if (atmos_enabled_) {
        // A live session has no loaded file to size object_configs_ from -
        // loadSourceFile is what normally does that. The capture device's
        // channel count plays the same role here, so the Live room and the
        // Objects tab have something to show/drag for a capture-only session
        // that never opened a file at all.
        const int nobjects = std::min<int>(static_cast<int>(device.channels), 15);
        if (object_count_ != nobjects) {
            // Whatever is here right now is a loaded file's own object
            // state (or a previous live session's, already the device's own
            // shape - either way object_count_ would already equal
            // nobjects and this branch would not run) - save it before
            // resizing over it, so it comes back once this session ends
            // instead of staying clobbered by an unrelated capture device's
            // channel count (see LiveObjectBackup's own comment).
            live_object_backup_ = LiveObjectBackup{.count = object_count_,
                                                   .configs = object_configs_,
                                                   .keyframes = object_keyframes_,
                                                   .selected_index = selected_object_index_};
            object_count_ = nobjects;
            refreshObjectConfigs();
            emit sourceChanged();
        }
    }

    {
        std::lock_guard lock(live_object_mutex_);
        live_object_snapshot_ = object_configs_;
    }

    stop_live_.store(false, std::memory_order_relaxed);
    // What this session was asked for, so switchLiveLayout can restart it
    // under a new preset. A fresh start also clears any relayout a previous
    // session's failure path left pending.
    live_request_ = LiveSessionRequest{.capture_index = captureDeviceIndex,
                                       .monitor = monitor,
                                       .receiver_index = receiverDeviceIndex};
    pending_live_relayout_.reset();
    live_active_ = true;
    live_monitoring_ = monitor_ok;
    live_passthrough_ = passthrough_ok;
    live_writing_to_disk_ = writeToDisk;
    live_running_seconds_ = 0.0;
    live_frames_encoded_ = 0;
    live_frames_dropped_ = 0;
    live_underruns_ = 0;
    live_latency_ms_ =
        2000.0 * static_cast<double>(ac3::kSamplesPerFrame) / static_cast<double>(device.sample_rate);
    setBusy(true);
    setStatus(QStringLiteral("Live session running from %1…")
                  .arg(QString::fromStdString(device.name)));
    emit liveActiveChanged();
    emit liveStatsChanged();

    if (passthrough_ok) {
        // A freshly opened exclusive-mode stream is exactly when a physical
        // receiver drops lock to re-negotiate - the mockup's own copy quotes
        // "expect a second of silence", so the banner clears on the same
        // timescale.
        live_reconnecting_ = true;
        emit liveReconnectingChanged();
        QTimer::singleShot(1500, this, [this] {
            live_reconnecting_ = false;
            emit liveReconnectingChanged();
        });
    }

    runLiveSession(device, monitor_ok, passthrough_ok, writeToDisk, path);
}

void EncoderController::runLiveSession(ac3::capture::DeviceInfo device, bool monitor,
                                       bool passthrough, bool write_to_disk, QString file_path) {
    auto p = currentPlan();
    p.sample_rate = *to_sample_rate(device.sample_rate);
    // Passthrough bursts are fixed-size per access unit, and a live session
    // has no "finished run" to summarize a variable rate against even when
    // nothing is listening on the passthrough leg - so a live session always
    // runs CBR, regardless of what the Format tab's Rate mode control
    // currently holds (see vbrAvailable()'s own comment).
    p.vbr = std::nullopt;
    const bool atmos = atmos_enabled_;
    const bool eac3 = atmos || p.codec == plan::Codec::kEac3;

    std::optional<plan::ChannelPlan> cp;
    std::optional<plan::Routing> routing;
    if (!atmos) {
        cp = effectiveChannelPlan();
        routing = plan::route(*cp, device.channels, p.meta.cmixlev, p.meta.surmixlev);
        if (!routing) {
            live_capture_.reset();
            live_monitor_sink_.reset();
            live_passthrough_sink_.reset();
            live_active_ = false;
            setBusy(false);
            emit liveActiveChanged();
            setStatus(to_qstring(plan::describe(plan::PlanError::kNoSourceLayout)));
            emit encodeFinished(false, status());
            return;
        }
        const auto coded = plan::coded_channels(*cp);
        const auto names = plan::coded_channel_names(*cp);
        QStringList labels;
        for (const auto& name : names) {
            labels.append(QString::fromStdString(name));
        }
        setLayout(cp->bed_acmod, cp->bed_lfe, labels, effectiveLabel(), coded, fedChannels());
    } else {
        const auto coded = plan::coded_channels(plan::LayoutId::k51);
        const auto names = plan::coded_channel_names(plan::LayoutId::k51);
        QStringList labels;
        for (const auto& name : names) {
            labels.append(QString::fromStdString(name));
        }
        setLayout(ac3::Acmod::k3_2, true, labels, QStringLiteral("5.1 bed"), coded,
                 fedChannels());
    }
    setMetering(true);

    const std::size_t nobjects = atmos ? std::min<std::size_t>(device.channels, 15) : 0;
    const std::size_t channels = device.channels;
    const std::uint32_t sample_rate = device.sample_rate;

    std::ignore = QtConcurrent::run([this, p, atmos, eac3, cp = std::move(cp),
                                     routing = std::move(routing), nobjects, channels,
                                     sample_rate, monitor, passthrough, write_to_disk,
                                     file_path]() mutable {
        // Heap-allocated: each of these carries a multi-KB internal history/
        // delay buffer, and stacking all four on this lambda's frame (which
        // PREfast's C6262 flagged) pushed it well past what's comfortable for
        // a worker-thread stack. Constructed once here, at session start, not
        // per audio frame - the make_unique cost is paid once, not in the
        // hot loop below.
        auto ac3_encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(p));
        auto eac3_encoder = std::make_unique<ac3::eac3::AccessUnitEncoder>(plan::eac3_config(p));
        std::unique_ptr<ac3::oba::AtmosEncoder> atmos_encoder;
        if (atmos) {
            atmos_encoder = std::make_unique<ac3::oba::AtmosEncoder>(
                ac3::oba::AtmosConfig{.sample_rate = p.sample_rate,
                                      .bitrate_kbps = p.bitrate_kbps,
                                      .dialnorm = p.meta.dialnorm,
                                      .num_bands_idx = 4},
                static_cast<int>(nobjects));
        }
        auto ac3_monitor_decoder = std::make_unique<ac3::FrameDecoder>();
        ac3::Eac3Decoder eac3_monitor_decoder;
        ac3::iec61937::Eac3BurstPacker eac3_packer;

        ac3::analysis::LevelMeter meter =
            atmos ? ac3::analysis::LevelMeter{ac3::Acmod::k3_2, true, sample_rate}
                  : ac3::analysis::LevelMeter{cp->bed_acmod, cp->bed_lfe, sample_rate,
                                             routing->coded_channels};

        std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                                       channels);
        std::vector<std::vector<float>> object_block(
            std::max<std::size_t>(nobjects, 1), std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::span<const float>> object_views(std::max<std::size_t>(nobjects, 1));
        std::vector<ac3::oba::ObjectPlacement> placement(std::max<std::size_t>(nobjects, 1));
        std::vector<std::span<const float>> bed_views(6);

        const std::size_t coded_count =
            atmos ? 6 : static_cast<std::size_t>(routing->coded_channels);
        std::vector<std::vector<float>> chan_source(
            atmos ? 0 : static_cast<std::size_t>(routing->source_channels),
            std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::vector<float>> chan_block(coded_count,
                                                   std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::span<const float>> chan_in;
        std::vector<std::span<float>> chan_out;
        std::vector<std::span<const float>> chan_views;
        for (auto& channel : chan_source) {
            chan_in.emplace_back(channel);
        }
        for (auto& channel : chan_block) {
            chan_out.emplace_back(channel);
            chan_views.emplace_back(channel);
        }

        std::vector<std::vector<std::byte>> frames;
        std::uint64_t n0 = 0;
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;

        while (!stop_live_.load(std::memory_order_relaxed)) {
            std::size_t filled = 0;
            while (filled < interleaved.size() &&
                   !stop_live_.load(std::memory_order_relaxed)) {
                const auto got = live_capture_->buffer()->read(
                    std::span{interleaved}.subspan(filled, interleaved.size() - filled));
                filled += got;
                if (got == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (filled < interleaved.size()) {
                break;  // stopped mid-frame; drop the partial frame
            }
            n0 += static_cast<std::uint64_t>(ac3::kSamplesPerFrame);

            std::vector<std::byte> unit_bytes;
            if (atmos) {
                for (std::size_t ch = 0; ch < nobjects; ++ch) {
                    for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                        const std::size_t base = static_cast<std::size_t>(i) * channels;
                        object_block[ch][static_cast<std::size_t>(i)] =
                            ch < channels ? interleaved[base + ch] : 0.0f;
                    }
                    object_views[ch] = object_block[ch];
                }
                const auto snapshot = liveObjectSnapshot();
                for (std::size_t i = 0; i < nobjects; ++i) {
                    const auto& config = i < snapshot.size() ? snapshot[i] : ObjectConfig{};
                    placement[i] = {
                        .position = {.x = config.x, .y = config.y, .z = config.z},
                        .gain = 0.7 / std::sqrt(static_cast<double>(nobjects)),
                        .lfe_send = config.lfe_send / std::sqrt(static_cast<double>(nobjects))};
                }
                const auto unit = atmos_encoder->encode_frame(
                    std::span{object_views}.first(nobjects),
                    std::span{placement}.first(nobjects));
                if (!unit) {
                    break;
                }
                for (std::size_t ch = 0; ch < 6; ++ch) {
                    bed_views[ch] = std::span{atmos_encoder->bed()[ch]};
                }
                meter.process(bed_views);
                unit_bytes = unit->bytes;
            } else {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t base = static_cast<std::size_t>(i) * channels;
                    for (std::size_t ch = 0; ch < chan_source.size(); ++ch) {
                        chan_source[ch][static_cast<std::size_t>(i)] =
                            ch < channels ? interleaved[base + ch] : 0.0f;
                    }
                }
                plan::render(*routing, chan_in, chan_out, ac3::kSamplesPerFrame);
                meter.process(chan_views);
                if (eac3) {
                    const auto unit = eac3_encoder->encode_access_unit(chan_views);
                    if (!unit) {
                        break;
                    }
                    unit_bytes = unit->bytes;
                } else {
                    const auto frame = ac3_encoder->encode_frame(chan_views);
                    if (!frame) {
                        break;
                    }
                    unit_bytes = *frame;
                }
            }

            if (monitor) {
                std::optional<std::vector<float>> to_play;
                if (eac3) {
                    const auto decoded = eac3_monitor_decoder.decode_access_unit(unit_bytes);
                    // §3.7: decoded->has_value() is false exactly when this
                    // access unit is being held back pending transient
                    // pre-noise processing (decode_access_unit's own doc
                    // comment) - live monitoring just waits for the next one.
                    if (decoded && decoded->has_value()) {
                        const auto order =
                            plan::wav_order(std::span{(*decoded)->layout.items}.first(
                                static_cast<std::size_t>((*decoded)->layout.count)));
                        to_play = interleave_reordered((*decoded)->channels, order);
                    }
                } else {
                    const auto decoded = ac3_monitor_decoder->decode_frame(unit_bytes);
                    if (decoded) {
                        const auto order =
                            ac3::io::wav_channel_order(decoded->acmod, decoded->lfe);
                        to_play = interleave_reordered(decoded->channels, order);
                    }
                }
                if (to_play) {
                    while (!live_monitor_sink_->submit(*to_play) &&
                          !stop_live_.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
            }

            if (passthrough) {
                std::optional<std::vector<std::byte>> burst;
                if (eac3) {
                    auto packed = eac3_packer.push(unit_bytes);
                    if (packed && *packed) {
                        burst = std::move(**packed);
                    }
                } else {
                    const auto wrapped = ac3::iec61937::wrap_frame(unit_bytes);
                    if (wrapped) {
                        burst = *wrapped;
                    }
                }
                if (burst) {
                    while (!live_passthrough_sink_->submit(*burst) &&
                          !stop_live_.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
            }

            if (write_to_disk) {
                frames.push_back(unit_bytes);
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                const double seconds =
                    static_cast<double>(n0) / static_cast<double>(sample_rate);
                const auto dropped = live_capture_->stats().frames_dropped;
                const auto underruns = passthrough ? live_passthrough_sink_->stats().underruns
                                                   : std::uint64_t{0};
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                  meter.levels().end());
                const auto encoded = static_cast<qint64>(n0 / ac3::kSamplesPerFrame);
                QMetaObject::invokeMethod(
                    this, [this, seconds, dropped, underruns, encoded,
                          snapshot = std::move(snapshot)] {
                        live_running_seconds_ = seconds;
                        live_frames_encoded_ = encoded;
                        live_frames_dropped_ = static_cast<qint64>(dropped);
                        live_underruns_ = underruns;
                        emit liveStatsChanged();
                        publishLevels(snapshot);
                    });
            }
        }

        QString problem;
        if (write_to_disk) {
            problem = writeOutput(file_path, frames, sample_rate,
                                  static_cast<int>(coded_count));
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
        QMetaObject::invokeMethod(this, [this, count, problem, write_to_disk,
                                         totals = std::move(totals)] {
            const auto capture_stats = live_capture_->stats();
            live_capture_->stop();
            live_capture_.reset();
            if (live_monitor_sink_) {
                live_monitor_sink_->stop();
                live_monitor_sink_.reset();
            }
            if (live_passthrough_sink_) {
                live_passthrough_sink_->stop();
                live_passthrough_sink_.reset();
            }
            live_active_ = false;
            live_reconnecting_ = false;
            // Whatever a loaded file (or nothing at all) had before this
            // session resized object_configs_ to the capture device's
            // channel count - see startLiveSession's own comment and
            // LiveObjectBackup's. Only set when that resize actually ran,
            // so a non-Atmos or already-matching-shape session leaves
            // object state untouched, exactly as before this existed.
            if (live_object_backup_) {
                object_count_ = live_object_backup_->count;
                object_configs_ = std::move(live_object_backup_->configs);
                object_keyframes_ = std::move(live_object_backup_->keyframes);
                selected_object_index_ = live_object_backup_->selected_index;
                live_object_backup_.reset();
                emit objectsChanged();
                emit sourceChanged();
            }
            setBusy(false);
            setMetering(false);
            publishLevels(totals);
            if (!problem.isEmpty()) {
                setStatus(problem);
            } else if (write_to_disk) {
                setStatus(QStringLiteral("Live session ended - wrote %1 frames (%2 dropped).")
                              .arg(count)
                              .arg(capture_stats.frames_dropped));
            } else {
                setStatus(QStringLiteral("Live session ended (%1 dropped, nothing written to "
                                         "disk).")
                              .arg(capture_stats.frames_dropped));
            }
            emit liveActiveChanged();
            emit liveReconnectingChanged();
            emit encodeFinished(problem.isEmpty(), status());
            // The layout switcher's second half: the session above was
            // stopped ON PURPOSE to renegotiate, so apply the preset and
            // start again with the same capture/monitor/receiver choices.
            // Runs after setBusy(false) - applyChannelPreset and
            // startLiveSession both refuse while busy - and only when the
            // stopped session ended cleanly; a failure is a real answer and
            // restarting on top of it would bury it.
            if (pending_live_relayout_) {
                const auto preset = *pending_live_relayout_;
                pending_live_relayout_.reset();
                if (problem.isEmpty() && live_request_) {
                    applyChannelPreset(preset);
                    startLiveSession(live_request_->capture_index, live_request_->monitor,
                                     live_request_->receiver_index, false, QUrl());
                }
            }
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

    const auto coded = plan::coded_channels(cp);
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
    setLayout(cp.bed_acmod, cp.bed_lfe, labels, label, coded, fed);
    setMetering(true);
    setStatus(QStringLiteral("Recording from %1…").arg(QString::fromStdString(device.name)));

    const auto channels = capture_->channels();
    const auto sample_rate = capture_->sample_rate();
    const bool eac3 = p.codec == plan::Codec::kEac3;

    std::ignore = QtConcurrent::run([this, path, p, routing = *routing, channels, sample_rate,
                                     cp, eac3]() {
        const auto coded_count = static_cast<std::size_t>(routing.coded_channels);
        // Heap-allocated, not stack: each carries a multi-KB internal history
        // buffer, and both together pushed this lambda's stack frame well
        // past what's comfortable for a worker thread (PREfast's C6262).
        // Constructed once here, at recording start, not per audio frame.
        auto ac3_encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(p));
        auto eac3_encoder = std::make_unique<ac3::eac3::AccessUnitEncoder>(plan::eac3_config(p));
        ac3::analysis::LevelMeter meter{cp.bed_acmod, cp.bed_lfe, sample_rate,
                                        static_cast<int>(coded_count)};

        std::vector<std::vector<std::byte>> frames;
        std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                                       channels);
        std::vector<std::vector<float>> source(
            static_cast<std::size_t>(routing.source_channels),
            std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        std::vector<std::vector<float>> block(coded_count,
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
                const auto unit = eac3_encoder->encode_access_unit(views);
                if (!unit) {
                    break;
                }
                frames.push_back(unit->bytes);
            } else {
                const auto frame = ac3_encoder->encode_frame(views);
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

void EncoderController::loadBundledTestSignal() {
    // WAV speaker order (FL, FR, FC, LFE, BL, BR), one distinct tone per
    // channel so the meters, the soundfield and any downstream decode all
    // show six different things rather than one signal six times.
    constexpr std::uint32_t rate = 48000;
    constexpr double seconds = 8.0;
    constexpr std::array<double, 6> frequencies = {440.0, 660.0, 880.0, 60.0, 330.0, 550.0};
    const auto total = static_cast<std::size_t>(rate * seconds);
    std::vector<std::vector<float>> channels(frequencies.size(),
                                             std::vector<float>(total));
    for (std::size_t ch = 0; ch < channels.size(); ++ch) {
        const double w = 2.0 * std::numbers::pi * frequencies[ch] / rate;
        for (std::size_t i = 0; i < total; ++i) {
            // A slow amplitude sweep keeps every needle moving; the short
            // edge fades keep the file click-free at both ends.
            const double t = static_cast<double>(i) / rate;
            const double envelope = 0.4 + 0.3 * std::sin(2.0 * std::numbers::pi * 0.25 * t);
            const double edge = std::min({1.0, t * 20.0, (seconds - t) * 20.0});
            channels[ch][i] = static_cast<float>(
                envelope * edge * std::sin(w * static_cast<double>(i)));
        }
    }
    const QString path = QDir::temp().filePath(QStringLiteral("ac3forge-test-51.wav"));
    if (const auto written = ac3::io::write_wav_f32(path.toStdString(), channels, rate);
        !written) {
        setStatus(QStringLiteral("Could not write the test signal: %1")
                      .arg(to_qstring(ac3::io::describe(written.error()))));
        return;
    }
    loadSourceFile(QUrl::fromLocalFile(path));
}

void EncoderController::loadSourceFile(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    auto wav = ac3::io::read_wav(path.toStdString());
    if (!wav) {
        source_.reset();
        source_ready_ = false;
        source_path_ = path;
        source_info_.clear();
        // Loading a new primary - successfully or not - always starts a
        // fresh source list: an extra or an assignment made sense relative
        // to whatever was loaded before, and there is no honest way to
        // carry either over to a source that failed to load at all.
        extra_sources_.clear();
        assignment_ = plan::Assignment{};
        touched_channels_.clear();
        has_explicit_assignment_ = false;
        object_count_ = 0;
        refreshObjectConfigs();
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
    if (!to_sample_rate_for_file(rate, codec_)) {
        problem = codec_ == plan::Codec::kEac3
                      ? QStringLiteral("sample rate %1 Hz is not legal here "
                                       "(need 32, 44.1 or 48 kHz, or 16, 22.05 or 24 kHz)")
                            .arg(rate)
                      : QStringLiteral("sample rate %1 Hz is not legal here (need 32, 44.1 or 48 "
                                       "kHz)")
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
    // Same reasoning as the failure branch above - a fresh primary starts a
    // fresh source list, even when the read itself succeeds.
    extra_sources_.clear();
    assignment_ = plan::Assignment{};
    touched_channels_.clear();
    has_explicit_assignment_ = false;
    source_ = std::make_shared<Source>(Source{std::move(*wav), path});
    source_path_ = path;
    source_ready_ = problem.isEmpty();
    // After the source_ swap, not before: with nothing assigned yet every
    // loaded channel is a dynamic object, and "every loaded channel" means
    // the file that just arrived, not the one it replaced.
    recomputeObjectCount();
    setMetering(false);
    emit sourceChanged();
    // The meters follow the PLAN from here (refreshRouting ends in
    // previewPlanMeters): the coded layout's labels and fed flags at once,
    // and the real levels - the file rendered through the actual routing -
    // as soon as the background pass lands. The old separate "metered as the
    // SOURCE" preview showed the same numbers for the common case (a file
    // whose natural layout is the plan), and showed a display nothing else
    // could reproduce for every other case.
    refreshRouting();

    setStatus(source_ready_ ? QStringLiteral("Ready to encode %1.").arg(QFileInfo(path).fileName())
                            : QStringLiteral("Cannot encode %1: %2")
                                  .arg(QFileInfo(path).fileName(), problem));
}

// ---------------------------------------------------------------------------
// Multi-source input and the assignment table
// ---------------------------------------------------------------------------

QVariantList EncoderController::sourceModel() const {
    QVariantList out;
    if (!source_) {
        return out;
    }
    auto addRow = [&](const QString& path, const ac3::io::WavData& wav, bool primary) {
        const double seconds =
            wav.sample_rate > 0
                ? static_cast<double>(wav.frame_count()) / static_cast<double>(wav.sample_rate)
                : 0.0;
        QVariantMap row;
        row[QStringLiteral("index")] = static_cast<int>(out.size());
        row[QStringLiteral("label")] = QFileInfo(path).fileName();
        row[QStringLiteral("path")] = path;
        row[QStringLiteral("channels")] = static_cast<int>(wav.channels.size());
        row[QStringLiteral("primary")] = primary;
        row[QStringLiteral("rate")] = static_cast<int>(wav.sample_rate);
        row[QStringLiteral("seconds")] = seconds;
        // "0:08" - the rail's per-source sub-line and its Length total both
        // print durations this way; formatted once here so they agree.
        row[QStringLiteral("duration")] = QStringLiteral("%1:%2")
                                              .arg(static_cast<int>(seconds) / 60)
                                              .arg(static_cast<int>(seconds) % 60, 2, 10,
                                                   QLatin1Char('0'));
        out.append(row);
    };
    addRow(source_->path, source_->wav, true);
    for (const auto& extra : extra_sources_) {
        addRow(extra->path, extra->wav, false);
    }
    return out;
}

QVariantList EncoderController::assignmentRows() const {
    QVariantList out;
    const auto shapes = sourceShapes();
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        for (std::size_t c = 0; c < shapes[s].channels; ++c) {
            QVariantMap row;
            row[QStringLiteral("source")] = static_cast<int>(s);
            row[QStringLiteral("channel")] = static_cast<int>(c);
            row[QStringLiteral("sourceLabel")] = QString::fromStdString(shapes[s].label);
            row[QStringLiteral("destToken")] =
                to_qstring(plan::format_destination(assignment_.at(s, c)));
            // "none" reads the same for a channel deliberately silenced and
            // one nobody has visited; touched is what tells a table's
            // "Nothing" apart from its "Choose…" placeholder.
            row[QStringLiteral("touched")] = touched_channels_.contains({s, c});
            out.append(row);
        }
    }
    return out;
}

QStringList EncoderController::unassignedWarnings() const {
    QStringList out;
    if (!has_explicit_assignment_ && extra_sources_.empty()) {
        // Automatic single-source routing accounts for every source channel
        // by construction - there is nothing here to warn about. More than
        // one source with nothing explicit set yet is the OTHER thing
        // routingForSources refuses to guess at (see its own comment), so
        // that case warns even before setAssignment has been called once -
        // every one of those channels genuinely goes nowhere right now.
        return out;
    }
    const auto shapes = sourceShapes();
    for (const auto& [s, c] : assignment_.unassigned(shapes)) {
        // Assignment::unassigned() cannot distinguish a channel explicitly
        // set to "none" from one nobody has visited at all (see
        // touched_channels_'s own comment) - subtracting this set is what
        // lets an intentional "none" actually silence the warning instead
        // of nagging forever.
        if (touched_channels_.contains({s, c})) {
            continue;
        }
        out.append(QStringLiteral("%1 ch %2 is loaded but goes nowhere")
                       .arg(QString::fromStdString(shapes[s].label))
                       .arg(c + 1));
    }
    return out;
}

void EncoderController::refreshAfterSourceListChange() {
    const auto shapes = sourceShapes();
    recomputeObjectCount();
    emit sourceChanged();
    refreshRouting();
    if (extra_sources_.empty()) {
        // Back to exactly one source - loadSourceFile's own "what it holds"
        // preview already set a status line the last time the primary
        // changed, and there is nothing new to say here.
        return;
    }
    // The meter preview (refreshRouting -> previewPlanMeters) has nothing to
    // render until an assignment exists - routingForSources refuses to guess
    // at a multi-source blend - so the bars sit silent on the plan's labels
    // and this status line is the immediate, honest summary.
    setStatus(QStringLiteral("%1 sources loaded — set an assignment for each channel below.")
                  .arg(static_cast<int>(shapes.size())));
}

void EncoderController::addSourceFile(const QUrl& url) {
    if (!source_) {
        // No primary yet - the first file loaded through either entry point
        // becomes it, so a caller offering one "add a source" affordance
        // never has to know which entry point to use first.
        loadSourceFile(url);
        return;
    }
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    auto wav = ac3::io::read_wav(path.toStdString());
    if (!wav) {
        setStatus(QStringLiteral("Could not read %1: %2")
                      .arg(QFileInfo(path).fileName(),
                           QString::fromUtf8(ac3::io::describe(wav.error()).data(),
                                             static_cast<qsizetype>(
                                                 ac3::io::describe(wav.error()).size()))));
        return;
    }
    if (wav->sample_rate != source_->wav.sample_rate) {
        // plan::render has no notion of resampling, and a silently
        // mismatched pair would drift apart rather than error - the same
        // reasoning ac3cli's own load_sources() gives for the identical
        // check (see main.cpp).
        setStatus(QStringLiteral("%1 is %2 Hz, but the loaded source is %3 Hz — every source "
                                 "must share a sample rate.")
                      .arg(QFileInfo(path).fileName())
                      .arg(wav->sample_rate)
                      .arg(source_->wav.sample_rate));
        return;
    }
    extra_sources_.push_back(std::make_shared<Source>(Source{std::move(*wav), path}));
    refreshAfterSourceListChange();
}

void EncoderController::removeSource(int index) {
    if (index == 0) {
        // The primary going away drops everything else with it - there is
        // no honest way to guess which extra, if any, should be promoted in
        // its place. Mirrors loadSourceFile's own reset-on-failure path.
        source_.reset();
        source_ready_ = false;
        source_path_.clear();
        source_info_.clear();
        extra_sources_.clear();
        assignment_ = plan::Assignment{};
        touched_channels_.clear();
        has_explicit_assignment_ = false;
        object_count_ = 0;
        refreshObjectConfigs();
        clearLayout();
        emit sourceChanged();
        setStatus(QStringLiteral("Choose a WAV file, or record from a capture device."));
        return;
    }
    const auto extra_index = static_cast<std::size_t>(index - 1);
    if (!source_ || extra_index >= extra_sources_.size()) {
        return;
    }
    extra_sources_.erase(extra_sources_.begin() + static_cast<std::ptrdiff_t>(extra_index));
    // The removed source's rows addressed positions by index, and every
    // later source's index just shifted down by one - there is no honest
    // way to guess which of its old rows survive at the new numbering, so
    // this clears and asks for the assignment to be redone rather than risk
    // silently misrouting a channel. Automatic routing resumes on its own
    // once exactly one source is left (see routingForSources).
    assignment_ = plan::Assignment{};
    touched_channels_.clear();
    has_explicit_assignment_ = !extra_sources_.empty();
    // Object mode addresses objects by the exact same shifted-index scheme
    // (object i is flat channel i across sourceShapes(), the same
    // addressing Assignment uses) - a position or authored path set for
    // object 6 meant a specific channel of a specific file, and the removal
    // above may have moved a DIFFERENT channel into index 6. Clearing
    // rather than silently reattaching motion to the wrong source is the
    // same call assignment_'s own reset just made; refreshAfterSourceListChange()
    // below rebuilds fresh defaults for whatever the new count is, via
    // refreshObjectConfigs() - see its own comment on why preserving by
    // index is only honest when nothing shifted underneath it.
    object_configs_.clear();
    object_keyframes_.clear();
    selected_object_index_ = 0;
    refreshAfterSourceListChange();
}

void EncoderController::setAssignment(int sourceIndex, int channel, const QString& destToken) {
    if (!source_ || sourceIndex < 0 || channel < 0) {
        return;
    }
    const auto dest = plan::parse_destination(destToken.toStdString());
    if (!dest) {
        return;
    }
    assignment_.set(static_cast<std::size_t>(sourceIndex), static_cast<std::size_t>(channel),
                    *dest);
    touched_channels_.insert({static_cast<std::size_t>(sourceIndex),
                              static_cast<std::size_t>(channel)});
    has_explicit_assignment_ = true;
    // Which channels ride as objects follows the table now, so an edit can
    // grow or shrink the object list - and relabel it even when the count
    // holds (refreshObjectConfigs only notifies on a count change).
    recomputeObjectCount();
    emit objectsChanged();
    emit sourceChanged();
    refreshRouting();
}

void EncoderController::clearAssignment() {
    assignment_ = plan::Assignment{};
    touched_channels_.clear();
    has_explicit_assignment_ = false;
    recomputeObjectCount();
    emit objectsChanged();
    emit sourceChanged();
    refreshRouting();
}

void EncoderController::autoAssignByName() {
    if (!source_) {
        return;
    }
    // The positions the current plan actually carries - a name the plan has
    // no place for stays unassigned (and keeps its warning) rather than
    // being invented.
    const auto cp = atmos_enabled_ ? plan::channel_plan_for(plan::LayoutId::k51)
                                   : effectiveChannelPlan();
    std::set<ac3::eac3::chanmap::Location> in_plan;
    for (const auto& channel : plan::coded_channels(cp)) {
        in_plan.insert(channel.location);
    }

    const auto shapes = sourceShapes();
    bool changed = false;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        // A source whose channel count has a natural AC-3 layout carries its
        // own names: a 5.1 WAV's channels ARE L R C LFE Ls Rs in WAV order.
        // A count with no natural layout (3, 7...) has no names to assign by.
        const auto layout = ac3::io::ac3_layout_for(shapes[s].channels);
        if (!layout) {
            continue;
        }
        std::vector<ac3::eac3::chanmap::Location> locations;
        for (const auto location : ac3::eac3::chanmap::expand(
                 ac3::eac3::chanmap::acmod_map(layout->acmod, layout->lfe))) {
            locations.push_back(location);
        }
        for (std::size_t k = 0; k < locations.size() && k < layout->wav_index.size(); ++k) {
            const auto wav_channel = layout->wav_index[k];
            if (!in_plan.contains(locations[k])) {
                continue;
            }
            // Never overwrite a decision already made - explicit positions
            // and deliberate "none"s alike.
            if (assignment_.at(s, wav_channel).kind != plan::DestinationKind::kUnassigned ||
                touched_channels_.contains({s, wav_channel})) {
                continue;
            }
            assignment_.set(s, wav_channel,
                            {.kind = plan::DestinationKind::kLocation,
                             .location = locations[k]});
            touched_channels_.insert({s, wav_channel});
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    has_explicit_assignment_ = true;
    recomputeObjectCount();
    emit objectsChanged();
    emit sourceChanged();
    refreshRouting();
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
    const auto rate = to_sample_rate_for_file(source_->wav.sample_rate, codec_);
    if (!rate) {
        return;
    }
    auto p = currentPlan();
    if (const auto bad = plan::validate(p)) {
        setStatus(to_qstring(plan::describe(*bad)));
        return;
    }

    // Built and validated here, once, rather than inside encodeChannels -
    // the same routing the pre-encode preview (refreshRouting/fedChannels)
    // already agreed on, not a second computation that could disagree with
    // it. Object mode needs none of this: it has no Routing at all.
    //
    // Snapshotted rather than re-read from atmos_enabled_ below: setBusy(true)
    // doesn't happen until after emit outputChanged() a few lines down, and
    // setAtmosEnabled() only guards on busy_ - a direct-connection slot
    // reacting to that signal could flip atmos_enabled_ before this function
    // reaches its second check, leaving `routing` stale relative to it (and,
    // in the object_mode-flipped-false case, dereferencing an empty
    // optional). object_mode pins both decisions to the same read.
    const bool object_mode = atmos_enabled_;
    const auto cp = plan::resolve(p);
    const auto routing = object_mode ? std::nullopt : routingForSources(cp, p);
    if (!object_mode && !routing) {
        setStatus(extra_sources_.empty()
                      ? to_qstring(plan::describe(plan::PlanError::kNoSourceLayout))
                      : QStringLiteral("Set an assignment for every loaded channel before "
                                       "encoding."));
        return;
    }
    if (object_mode) {
        // TS 103 420 §8.3.2.2's sixteen-object cap, with the bed's LFE as
        // one of them: dynamic objects plus every bed-pinned channel have to
        // fit in the other fifteen. Refused here, before a run entry opens,
        // the same way a channel plan that cannot be routed is.
        const auto ndynamic = dynamicObjectChannels().size();
        const auto npinned = pinnedObjectChannels().size();
        if (ndynamic + npinned == 0) {
            setStatus(QStringLiteral("Nothing is assigned to an object or a bed position — "
                                     "give at least one channel a destination."));
            return;
        }
        if (ndynamic + npinned > 15) {
            setStatus(QStringLiteral("%1 objects and %2 bed-fed channels exceed the sixteen-"
                                     "object programme cap (the bed's LFE is one of them) — "
                                     "assign fewer channels.")
                          .arg(static_cast<int>(ndynamic))
                          .arg(static_cast<int>(npinned)));
            return;
        }
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    emit outputChanged();

    cancel_requested_.store(false, std::memory_order_relaxed);
    startRun(path);
    setBusy(true);
    setProgress(0.0);
    setStatus(QStringLiteral("Encoding…"));

    const auto sample_rate = source_->wav.sample_rate;
    // Concatenated in source order (source 0 first) - the same flattening
    // Assignment/Routing already assume, and the identity permutation for
    // the single-source case, so nothing here changes when only one source
    // is loaded. The WAV payload is copied into the worker so the GUI
    // thread stays free to swap the loaded sources while an encode runs.
    std::vector<std::vector<float>> planes = source_->wav.channels;
    for (const auto& extra : extra_sources_) {
        planes.insert(planes.end(), extra->wav.channels.begin(), extra->wav.channels.end());
    }

    if (object_mode) {
        encodeObjects(path, std::move(planes), sample_rate);
        return;
    }
    encodeChannels(path, std::move(planes), *routing, sample_rate);
}

void EncoderController::encodeChannels(const QString& path,
                                       std::vector<std::vector<float>> planes,
                                       const plan::Routing& routing,
                                       std::uint32_t sample_rate) {
    auto p = currentPlan();
    const auto cp = plan::resolve(p);
    const auto label = effectiveLabel();
    // routing is already built and validated by encodeTo, via
    // routingForSources - this function cannot silently disagree with what
    // the pre-encode preview already showed.

    // Dual mono has no "whole programme" for the block below to gate-measure
    // over - Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so
    // a single BS.1770 pass across both would measure a blend of two
    // different things rather than either one. ac3cli's own dual-mono path
    // measures each programme independently instead (measured_dialnorm_channel
    // in main.cpp); this controller does not have that machinery yet, so -
    // matching the same scope cut Part 3 already made for src=/map= - it
    // asks for both values explicitly rather than measuring one of them
    // wrong.
    if (isDualMono() && (p.meta.measure_dialnorm || p.meta.measure_dialnorm2)) {
        setBusy(false);
        setStatus(QStringLiteral("dialnorm=auto is not yet supported for 1+1 dual mono - set "
                                 "both programmes' dialnorm by hand."));
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

    const auto coded = plan::coded_channels(cp);
    // coded_channel_names() answers "L"/"R" for dual mono - acmod_map's own
    // placeholder for a pair of Table E2.5 bits 1+1 needs but is not
    // actually a location (see bed_channel_names()'s identical override).
    // The meters get the honest names instead.
    QStringList labels;
    if (isDualMono()) {
        labels = {QStringLiteral("Program 1"), QStringLiteral("Program 2")};
    } else {
        const auto names = plan::coded_channel_names(cp);
        for (const auto& name : names) {
            labels.append(QString::fromStdString(name));
        }
    }
    setLayout(cp.bed_acmod, cp.bed_lfe, labels, label, coded, fedChannels());
    setMetering(true);

    const bool eac3 = p.codec == plan::Codec::kEac3;
    const bool keep_partial = keep_partial_output_;
    std::ignore = QtConcurrent::run([this, path, p, routing, cp, sample_rate,
                                     eac3, label, keep_partial,
                                     planes = std::move(planes)]() mutable {
        const auto coded_count = static_cast<std::size_t>(routing.coded_channels);
        // Heap-allocated, not stack: each carries a multi-KB internal history
        // buffer, and both together pushed this lambda's stack frame well
        // past what's comfortable for a worker thread (PREfast's C6262).
        // Constructed once here, at encode start, not per audio frame.
        auto ac3_encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(p));
        auto eac3_encoder = std::make_unique<ac3::eac3::AccessUnitEncoder>(plan::eac3_config(p));
        ac3::analysis::LevelMeter meter{cp.bed_acmod, cp.bed_lfe, sample_rate,
                                        static_cast<int>(coded_count)};

        // The longest loaded channel, not just channel 0's - a run with
        // several sources of different lengths covers all of them (each
        // shorter one below simply pads with silence past its own end)
        // rather than truncating to whichever happens to be shortest.
        std::size_t total = 0;
        for (const auto& channel : planes) {
            total = std::max(total, channel.size());
        }
        std::vector<std::vector<float>> source(planes.size(),
                                               std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::vector<float>> block(coded_count,
                                              std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> in;
        std::vector<std::span<float>> out;
        std::vector<std::span<const float>> views;
        std::vector<std::span<const float>> metered(coded_count);
        for (auto& channel : source) {
            in.emplace_back(channel);
        }
        for (auto& channel : block) {
            out.emplace_back(channel);
            views.emplace_back(channel);
        }

        std::vector<std::vector<std::byte>> frames;
        std::uint64_t bytes = 0;
        // Only meaningful when p.vbr is set - CBR's frame size barely moves,
        // so tracking it unconditionally costs nothing and keeps the loop
        // below from needing a vbr-only branch. min starts at 0 ("unset")
        // rather than SIZE_MAX so the first frame always replaces it.
        std::size_t min_frame_bytes = 0;
        std::size_t max_frame_bytes = 0;
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
                // Each channel's OWN length, not the run's overall total: a
                // shorter source among several pads with silence from where
                // IT ends, not from wherever the longest one does.
                const auto len = planes[ch].size();
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    source[ch][static_cast<std::size_t>(i)] = at < len ? planes[ch][at] : 0.0f;
                }
            }
            plan::render(routing, in, out, ac3::kSamplesPerFrame);
            for (std::size_t ch = 0; ch < coded_count; ++ch) {
                metered[ch] = std::span{block[ch]}.first(valid);
            }
            meter.process(metered);

            if (eac3) {
                const auto unit = eac3_encoder->encode_access_unit(views);
                if (!unit) {
                    problem = QStringLiteral(
                        "The encoder cannot express this configuration — a wider layout needs "
                        "a higher bit rate to fit its substreams.");
                    break;
                }
                bytes += unit->bytes.size();
                min_frame_bytes =
                    min_frame_bytes == 0 ? unit->bytes.size()
                                        : std::min(min_frame_bytes, unit->bytes.size());
                max_frame_bytes = std::max(max_frame_bytes, unit->bytes.size());
                frames.push_back(unit->bytes);
            } else {
                const auto frame = ac3_encoder->encode_frame(views);
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
            // Progress rides the same wall-clock throttle as the levels. A
            // file encodes far faster than it plays, and a queued setProgress
            // per frame flooded the GUI event loop badly enough to stutter
            // every animation on screen - ~30 Hz is already more than a
            // progress bar can show.
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                  meter.levels().end());
                QMetaObject::invokeMethod(
                    this, [this, done, snapshot = std::move(snapshot)] {
                        setProgress(std::min(done, 1.0));
                        publishLevels(snapshot);
                    });
            }
        }

        QString partial_note;
        if (problem.isEmpty() && !cancelled) {
            problem = writeOutput(path, frames, sample_rate, plan::rendered_channel_count(cp));
        } else if (keep_partial && !frames.empty()) {
            // Partial output is named and kept, not silently discarded - the
            // half-finished take is real work, and throwing it away decides
            // for the user that it was worthless.
            const QString partial = partial_output_path(path);
            if (writeOutput(partial, frames, sample_rate, plan::rendered_channel_count(cp))
                    .isEmpty()) {
                partial_note = QStringLiteral(" The %1 frames already written are kept at %2.")
                                   .arg(frames.size())
                                   .arg(QFileInfo(partial).fileName());
            }
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
        QMetaObject::invokeMethod(this, [this, count, bytes, min_frame_bytes, max_frame_bytes,
                                         cancelled, problem, partial_note, label, eac3,
                                         vbr = p.vbr, sample_rate,
                                         totals = std::move(totals)] {
            setBusy(false);
            setMetering(false);
            setProgress(cancelled ? 0.0 : 1.0);
            publishLevels(totals);
            if (cancelled) {
                setStatus(QStringLiteral("Encode cancelled.") + partial_note);
            } else if (!problem.isEmpty()) {
                setStatus(problem + partial_note);
            } else {
                setStatus(QStringLiteral("Wrote %1 %2 %3 (%4 KB) to %5")
                              .arg(count)
                              .arg(label, eac3 ? QStringLiteral("access units")
                                                : QStringLiteral("frames"))
                              .arg(bytes / 1024)
                              .arg(QFileInfo(output_path_).fileName()));
            }
            if (!cancelled && problem.isEmpty()) {
                // A VBR run has no target rate to report - only what it
                // actually spent, the same "what it did" framing the CLI's
                // own VBR report uses (main.cpp's run_eac3_encode_multi).
                if (vbr && count > 0) {
                    const auto kbps = [sample_rate](double frame_bytes) {
                        return std::lround(frame_bytes * 8.0 * static_cast<double>(sample_rate) /
                                           (1000.0 * static_cast<double>(ac3::kSamplesPerFrame)));
                    };
                    const double mean_bytes =
                        static_cast<double>(bytes) / static_cast<double>(count);
                    pending_rate_text_ =
                        QStringLiteral("VBR q%1 · avg %2 kbps (%3–%4)")
                            .arg(std::lround(vbr->quality * 100))
                            .arg(kbps(mean_bytes))
                            .arg(kbps(static_cast<double>(min_frame_bytes)))
                            .arg(kbps(static_cast<double>(max_frame_bytes)));
                } else {
                    pending_rate_text_ = QStringLiteral("%1 kbps").arg(bitrate_kbps_);
                }
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

    // Which of the flat channels ride, and how, follows the assignment table
    // (every channel is a dynamic object when nothing is assigned - the
    // behaviour this path has always had). encodeTo already enforced TS 103
    // 420 §8.3.2.2's sixteen-object cap over dynamic + pinned together, with
    // the bed's LFE as one of the sixteen.
    const auto dynamic = dynamicObjectChannels();
    const auto pinned = pinnedObjectChannels();
    const std::size_t ndynamic = std::min<std::size_t>(dynamic.size(), 15);
    const std::size_t nobjects = ndynamic + pinned.size();

    // Each dynamic object gets its own path over time: authored keyframes
    // where the GUI has been given some (see setObjectPathKeyframes),
    // otherwise its own static position (see ObjectConfig - independent per
    // object now, not a shared point plus a spread fan-out), held constant
    // for the whole file. Built here, on the GUI thread, and moved into the
    // worker below - the same timing today's per-object capture already
    // relied on, so nothing about that thread-safety changes.
    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(nobjects);
    for (std::size_t i = 0; i < ndynamic; ++i) {
        const auto authored = object_keyframes_.constFind(static_cast<int>(i));
        if (authored != object_keyframes_.constEnd() && !authored->empty()) {
            auto created = ac3::oba::KeyframePath::create(*authored);
            if (created) {
                paths.emplace_back(std::move(*created));
                continue;
            }
        }
        const auto& config = i < object_configs_.size()
                                 ? object_configs_[i]
                                 : ObjectConfig{};
        auto fallback = ac3::oba::KeyframePath::create(
            {{.time_s = 0.0,
              .position = {.x = config.x, .y = config.y, .z = config.z},
              // Every object is panned into the SAME five channels, so their
              // contributions add there. At unity apiece a six-channel source
              // put the bed's centre 7 dB over full scale; the inverse-root
              // law is what ac3cli's 'atmos' uses, and it keeps the sum near
              // unity for sources that are not identical.
              .gain = 0.7 / std::sqrt(static_cast<double>(std::max<std::size_t>(ndynamic, 1))),
              // The LFE is one channel, and sending every object at full
              // strength would pile the whole programme's low end into it.
              .lfe_send = config.lfe_send /
                          std::sqrt(static_cast<double>(std::max<std::size_t>(ndynamic, 1)))}});
        paths.emplace_back(std::move(*fallback));
    }
    // A channel assigned to a bed position is a static object pinned at that
    // speaker's azimuth: in a JOC stream the bed IS the panned objects, so
    // "carried as a channel" and "an object that never leaves the L speaker"
    // are the same coded thing. Unity gain - it is a channel feed, and the
    // inverse-root law above exists for objects sharing arbitrary positions,
    // not for one source aimed at its own speaker. The LFE has no direction
    // to pin at, so it rides as a pure lfe_send.
    for (const auto& [flat, location] : pinned) {
        using ac3::eac3::chanmap::Location;
        const bool lfe_pin = location == Location::kLfe || location == Location::kLfe2;
        const auto azimuth =
            lfe_pin ? std::optional<double>{} : location_azimuth_deg(location);
        const auto position = azimuth ? speaker_pin_position(*azimuth)
                                      : ac3::oba::Position{.x = 0.5, .y = 0.5, .z = 0.0};
        auto pin_path = ac3::oba::KeyframePath::create(
            {{.time_s = 0.0,
              .position = position,
              .gain = lfe_pin ? 0.0 : 1.0,
              .lfe_send = lfe_pin ? 1.0 : 0.0}});
        paths.emplace_back(std::move(*pin_path));
    }

    // The worker's planes are re-packed to object order: dynamic objects
    // first (object i = the i-th "obj"-assigned channel, the objectModel/
    // config addressing), then the pinned channels. Channels assigned
    // nowhere are dropped here, which is what "Unassigned - it will not be
    // heard" means.
    std::vector<std::vector<float>> object_planes;
    object_planes.reserve(nobjects);
    for (std::size_t i = 0; i < ndynamic; ++i) {
        object_planes.push_back(std::move(planes[dynamic[i]]));
    }
    for (const auto& [flat, location] : pinned) {
        object_planes.push_back(std::move(planes[flat]));
    }
    planes = std::move(object_planes);

    // The meters follow the BED, not the source: 5.1 is what comes out and
    // what a legacy decoder hears, whatever the source layout was.
    const auto coded = plan::coded_channels(plan::LayoutId::k51);
    const auto names = plan::coded_channel_names(plan::LayoutId::k51);
    QStringList labels;
    for (const auto& name : names) {
        labels.append(QString::fromStdString(name));
    }
    setLayout(ac3::Acmod::k3_2, true, labels, QStringLiteral("5.1 bed"), coded, fedChannels());
    setMetering(true);

    const bool keep_partial = keep_partial_output_;
    std::ignore = QtConcurrent::run([this, path, p, sample_rate, nobjects, keep_partial,
                                     paths = std::move(paths),
                                     planes = std::move(planes)]() mutable {
        ac3::oba::AtmosEncoder encoder{{.sample_rate = p.sample_rate,
                                        .bitrate_kbps = p.bitrate_kbps,
                                        .dialnorm = p.meta.dialnorm,
                                        .num_bands_idx = 4},
                                       static_cast<int>(nobjects)};

        ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, sample_rate};

        // The longest of the channels actually used as an object - see
        // encodeChannels' identical reasoning for why this is a max, not
        // just channel 0's length, once more than one source is in play.
        std::size_t total = 0;
        for (std::size_t ch = 0; ch < nobjects; ++ch) {
            total = std::max(total, planes[ch].size());
        }
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
                const auto len = planes[ch].size();
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    block[ch][static_cast<std::size_t>(i)] = at < len ? planes[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
            }
            // The placement is the object's position at the END of the
            // frame - same convention ac3cli's 'atmos' uses, because that is
            // where OAMD's ramp and the JOC matrix both finish. Re-evaluated
            // every frame - see tests/test_atmos_motion.cpp; this must stay
            // inside the loop, not be hoisted above it.
            const double t = static_cast<double>(start + ac3::kSamplesPerFrame) /
                             static_cast<double>(sample_rate);
            const auto placement = ac3::oba::evaluate_placements(paths, t);
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
            // Same wall-clock gate as encodeChannels', for the same reason: a
            // queued setProgress per frame is an event-loop flood, not a
            // smoother progress bar.
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                 meter.levels().end());
                QMetaObject::invokeMethod(
                    this, [this, done, snapshot = std::move(snapshot)] {
                        setProgress(std::min(done, 1.0));
                        publishLevels(snapshot);
                    });
            }
        }

        QString partial_note;
        if (problem.isEmpty() && !cancelled) {
            problem = writeOutput(path, frames, sample_rate, 6);
        } else if (keep_partial && !frames.empty()) {
            // Same "named and kept" rule as the channel path.
            const QString partial = partial_output_path(path);
            if (writeOutput(partial, frames, sample_rate, 6).isEmpty()) {
                partial_note = QStringLiteral(" The %1 frames already written are kept at %2.")
                                   .arg(frames.size())
                                   .arg(QFileInfo(partial).fileName());
            }
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
                                         problem, partial_note, totals = std::move(totals)] {
            setBusy(false);
            setMetering(false);
            setProgress(cancelled ? 0.0 : 1.0);
            publishLevels(totals);
            if (cancelled) {
                setStatus(QStringLiteral("Encode cancelled.") + partial_note);
            } else if (!problem.isEmpty()) {
                setStatus(problem + partial_note);
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
