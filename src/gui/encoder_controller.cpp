#include "encoder_controller.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <iterator>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/capture/watchdog.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/dsp/biquad.hpp"
#include "ac3/dsp/resampler.hpp"
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

// Corner of the LFE low-pass bundle C's assignment table applies to an
// explicitly LFE/LFE2-routed full-bandwidth channel - see docs/gui/
// source-assignment.md's LFE note and ac3::dsp::LfeLowpass's own header
// comment for why 120 Hz and why a 4th-order Butterworth.
constexpr double kLfeLowpassCornerHz = 120.0;

// Coded-channel indices at an LFE/LFE2 location, in `coded`'s own order -
// exactly the channels an ASSIGNMENT-based routing (has_explicit_
// assignment_ true, so plan::route(target, sources, assignment) or
// dual_mono_routing built the Routing, not the automatic single-source
// route(target, wav_channels, ...) overload) should low-pass. Every call
// site guards on has_explicit_assignment_ itself before calling this - the
// automatic overload's own dedicated-LFE-channel handling stays bit-exact,
// per the assignment table's own documented distinction (see
// DestinationKind::kLocation's routing comment in assignment.hpp).
std::vector<std::size_t> lfe_coded_indices(const std::vector<plan::CodedChannel>& coded) {
    std::vector<std::size_t> out;
    using ac3::eac3::chanmap::Location;
    for (std::size_t i = 0; i < coded.size(); ++i) {
        if (coded[i].location == Location::kLfe || coded[i].location == Location::kLfe2) {
            out.push_back(i);
        }
    }
    return out;
}

// "48 000" / "7 891" - the mockup's space-grouped integers (a locale-
// independent stand-in for its thin space). Negative values never reach a
// readout that groups, so only the digits are walked.
QString group_digits(qint64 value) {
    QString digits = QString::number(value);
    const qsizetype first = digits.startsWith(QLatin1Char('-')) ? 1 : 0;
    for (qsizetype at = digits.size() - 3; at > first; at -= 3) {
        digits.insert(at, QLatin1Char(' '));
    }
    return digits;
}

// Live meters redraw no faster than this. A file encodes far quicker than it
// plays, so without a wall-clock throttle a two-minute track would fire tens
// of thousands of property updates the display could never show.
constexpr auto kPublishInterval = std::chrono::milliseconds(33);

// How often a live session's incremental writers (the take itself and the
// optional raw-WAV safety copy) are flushed/header-patched to disk. Frequent
// enough that a hard crash loses at most a second; infrequent enough that it
// is not a per-frame fsync.
constexpr auto kDiskFlushInterval = std::chrono::milliseconds(1000);

// How long a live capture may go silent before the watchdog decides the
// device itself is gone rather than just momentarily starved.
constexpr auto kDeviceSilenceTimeout = std::chrono::milliseconds(3000);

// A reserved source index (past any real sourceShapes() count, which never
// gets remotely this large) for a live session's objects, which have no
// (source, channel) of their own to key by - see
// EncoderController::keyForObjectIndex.
constexpr std::size_t kLiveObjectSource = std::numeric_limits<std::size_t>::max();

// Bakes each channel's own source offset in as leading silence, so every
// downstream consumer - the dialnorm=auto measurement pass, the frame
// loop's existing zero-pad-past-the-end - sees one continuous, already
// time-aligned programme rather than needing its own offset-aware
// indexing. Only valid for OWNED plane storage (encodeChannels/
// encodeObjects, via encodeTo) - previewPlanMeters reads its sources as
// non-owning spans into shared WavData and cannot prepend to them, so it
// applies the same offsets as a per-frame read-shift instead.
std::vector<std::vector<float>> apply_channel_offsets(std::vector<std::vector<float>> planes,
                                                       const std::vector<std::size_t>& offsets) {
    for (std::size_t ch = 0; ch < planes.size() && ch < offsets.size(); ++ch) {
        if (offsets[ch] > 0) {
            planes[ch].insert(planes[ch].begin(), offsets[ch], 0.0f);
        }
    }
    return planes;
}

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

// Splices a fixed suffix onto `path`'s stem, replacing whatever extension it
// had - the shared "derive a sibling filename" logic live_stream_spool_path
// and the raw-WAV safety copy's own path both need.
QString sibling_path(const QString& path, const QString& suffix) {
    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
    const qsizetype slash = std::max(path.lastIndexOf(QLatin1Char('/')),
                                     path.lastIndexOf(QLatin1Char('\\')));
    return (dot > slash ? path.left(dot) : path) + suffix;
}

// Where a live session's Matroska take spools its elementary stream while it
// grows - matroska::mux() only ever produces a complete file from the WHOLE
// set of frames, so the real .mkv can only be written once, at a clean stop
// (see EncoderController::LiveOutputWriters' own comment). Named with its
// own honest extension beside the final path, deliberately NOT using
// partial_output_path's ".partial" suffix above - that means "kept only
// because a run failed"; this spool exists for the entire life of every
// Matroska live session, failed or not, and is folded into the .mkv (then
// removed) on success. A crash leaves exactly this file behind holding
// everything captured up to that point - "the elementary take" the live
// session docs page promises.
QString live_stream_spool_path(const QString& mkv_path, bool eac3) {
    return sibling_path(mkv_path, eac3 ? QStringLiteral(".live.ec3") : QStringLiteral(".live.ac3"));
}

// Whether the receiver leg should run the parallel 5.1 downmix instead of
// the main plan: the main plan needs E-AC-3 (any object session, or a wide
// channel layout) but the chosen receiver cannot take E-AC-3 - and CAN take
// plain AC-3, which is the one case a capped leg turns a refusal into sound.
// A receiver that can take neither format is a genuine refusal either way,
// so this only ever matters when it is true.
bool wants_downmix_leg(bool main_needs_eac3, const ac3::sinks::RenderDeviceInfo& receiver) {
    return main_needs_eac3 && !receiver.supports_eac3_passthrough &&
          receiver.supports_ac3_passthrough;
}

// Opens a PassthroughSink for `receiver`, or explains why it did not - the
// same open-and-explain logic startLiveSession's own initial open always
// used, factored out so switchLiveReceiver's mid-session hot-swap (running
// on runLiveSession's worker thread) reproduces it byte-for-byte rather than
// drifting from what a fresh session start does. `eac3` is the format being
// REQUESTED of the sink (already downgraded to false by the caller when
// wants_downmix_leg() is true - see its own call sites); `downmix_leg` is
// only for the plan_text wording, so a capped AC-3 leg reads as exactly
// that rather than as an ordinary AC-3 session.
struct LivePassthroughOpen {
    bool ok = false;
    std::unique_ptr<ac3::sinks::PassthroughSink> sink;
    QString plan_text;
};
LivePassthroughOpen open_live_passthrough(const ac3::sinks::RenderDeviceInfo& receiver, bool eac3,
                                          bool atmos_enabled, bool downmix_leg,
                                          std::uint32_t sample_rate, const QString& shape_name) {
    LivePassthroughOpen result;
    const bool supports =
        eac3 ? receiver.supports_eac3_passthrough : receiver.supports_ac3_passthrough;
    if (!supports) {
        result.plan_text = QStringLiteral("\"%1\" cannot bitstream %2 over IEC 61937.")
                                .arg(QString::fromStdString(receiver.name),
                                     eac3 ? QStringLiteral("E-AC-3") : QStringLiteral("AC-3"));
        return result;
    }
    auto psink = std::make_unique<ac3::sinks::PassthroughSink>();
    const auto format =
        eac3 ? ac3::sinks::BitstreamFormat::kEac3 : ac3::sinks::BitstreamFormat::kAc3;
    const auto started = psink->start(receiver.id, sample_rate, format);
    if (!started) {
        const auto why = ac3::sinks::describe(started.error());
        result.plan_text =
            QStringLiteral("\"%1\" would not open: %2")
                .arg(QString::fromStdString(receiver.name),
                     QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size())));
        return result;
    }
    result.ok = true;
    result.sink = std::move(psink);
    result.plan_text =
        downmix_leg
            ? QStringLiteral("Dolby Digital · 5.1 · %1").arg(QString::fromStdString(receiver.name))
        : atmos_enabled
            ? QStringLiteral("Dolby Digital Plus · 5.1 bed only · %1")
                  .arg(QString::fromStdString(receiver.name))
            : QStringLiteral("%1 · %2 · %3")
                  .arg(eac3 ? QStringLiteral("Dolby Digital Plus") : QStringLiteral("Dolby Digital"))
                  .arg(shape_name, QString::fromStdString(receiver.name));
    return result;
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

// std::map has no QHash::value(key, default) - this is that, generically,
// for object_configs_/object_keyframes_/object_path_labels_'s lookups.
template <typename Map>
typename Map::mapped_type map_value(const Map& map, const typename Map::key_type& key) {
    const auto found = map.find(key);
    return found != map.end() ? found->second : typename Map::mapped_type{};
}

// removeSource's non-primary path: drops removed_source's own entries and
// renumbers every later source's down by one - the same shift sourceShapes()
// itself applies once that source's gone, so a surviving source's authored
// motion stays addressed to exactly the same channels it always was.
template <typename Map>
void rekey_after_source_removed(Map& map, std::size_t removed_source) {
    Map rekeyed;
    for (auto& [key, value] : map) {
        if (key.first == removed_source) {
            continue;
        }
        const auto new_source = key.first > removed_source ? key.first - 1 : key.first;
        rekeyed.emplace(typename Map::key_type{new_source, key.second}, std::move(value));
    }
    map = std::move(rekeyed);
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

// What runLiveSession needs to keep writing a take incrementally once its
// GUI-thread preamble hands off to the worker - see
// EncoderController::openLiveOutputWriters. `stream` IS the final output for
// an elementary-stream container (every byte written here is already the
// take); for Matroska it is the spool at live_stream_spool_path(), and
// `frame_sizes` is the lightweight (one uint32 per unit, not the audio
// itself) index that lets the clean-stop path split the spool back into
// frames for matroska::mux() without ever having kept the encoded bytes in
// RAM during the run.
struct EncoderController::LiveOutputWriters {
    std::ofstream stream;
    QString stream_path;   // == the final path unless matroska
    QString final_path;    // always the real destination the user chose
    bool matroska = false;
    std::vector<std::uint32_t> frame_sizes;
    std::unique_ptr<ac3::io::WavStreamWriter> wav_safety;  // null when not requested
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
    // extrasLocked() is only ever true for object mode or dual mono, and the
    // two lock for different reasons: dual mono's extras are not "E-AC-3
    // only" (its codec can be E-AC-3), they are simply not part of two mono
    // programmes. The third reason survives for any future lock that is
    // genuinely codec-driven.
    const QString lock_reason = atmos_enabled_ ? QStringLiteral("fixed by object mode")
                                : isDualMono() ? QStringLiteral("not part of dual mono")
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

        // The channel tokens themselves ("Lw Rw"), not a count - the row
        // prints what the extra actually adds, in the same Table E2.5 names
        // the channel map and the CLI's [layout] argument use.
        QStringList tokens;
        for (const auto location : ac3::eac3::chanmap::expand(extra.bits)) {
            tokens.append(to_qstring(ac3::eac3::chanmap::name(location)));
        }

        QVariantMap row;
        row[QStringLiteral("id")] = QString::fromLatin1(extra.id);
        row[QStringLiteral("label")] = QString::fromLatin1(extra.label);
        row[QStringLiteral("channels")] = ac3::eac3::chanmap::channel_count(extra.bits);
        row[QStringLiteral("tokens")] = tokens.join(QStringLiteral(" "));
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
    // A live Atmos session's objects have no (source, channel) of their own
    // to name - they name a SLOT's capture-channel binding instead (see
    // liveObjectChannels' own comment). live_active_ is the direct signal
    // for that, rather than keyForObjectIndex's own live_object_backup_
    // check: the two agree in every case that matters here, and this one
    // reads correctly even in the (very narrow) coincidence where a loaded
    // file's object count happens to already match the live budget's own
    // formula, which is the one case live_object_backup_ itself would stay
    // unset for.
    const bool live_slots = live_active_ && atmos_enabled_;
    const auto slot_channels = live_slots ? liveSlotChannels() : std::vector<int>{};
    for (int i = 0; i < object_count_; ++i) {
        const auto key = keyForObjectIndex(i);
        const auto config = key ? map_value(object_configs_, *key) : ObjectConfig{};
        const auto keyframes = sortedKeyframes(i);
        QVariantMap row;
        row[QStringLiteral("index")] = i;
        if (live_slots) {
            const int bound = static_cast<std::size_t>(i) < slot_channels.size()
                                  ? slot_channels[static_cast<std::size_t>(i)]
                                  : -1;
            row[QStringLiteral("sourceLabel")] = bound >= 0
                                                     ? QStringLiteral("Capture ch %1").arg(bound + 1)
                                                     : QStringLiteral("silent");
        } else {
            // Objects are a channel, one each, so the honest name for where
            // one comes from is which channel it is - and, once more than
            // one source is loaded, which FILE that channel is in (see
            // objectSourceLabel's own comment).
            row[QStringLiteral("sourceLabel")] = objectSourceLabel(
                static_cast<std::size_t>(i) < dynamic.size()
                    ? dynamic[static_cast<std::size_t>(i)]
                    : std::vector<std::size_t>{static_cast<std::size_t>(i)});
        }
        // Which row of sourceModel this object's channel belongs to - the
        // same identity `key` already carries, exposed so QML can find
        // "every object this source owns" (the timeline clip-band's "move
        // keys with source" drag - see shiftObjectKeyframes) without having
        // to parse it back out of sourceLabel's display text.
        row[QStringLiteral("sourceIndex")] = key ? static_cast<int>(key->first) : -1;
        row[QStringLiteral("x")] = config.x;
        row[QStringLiteral("y")] = config.y;
        row[QStringLiteral("z")] = config.z;
        row[QStringLiteral("lfeSend")] = config.lfe_send;
        row[QStringLiteral("hasPath")] = !keyframes.empty();
        row[QStringLiteral("keyCount")] = static_cast<int>(keyframes.size());
        // "orbit"/"lift" when a preset authored the path, empty for hand-
        // authored ones - the table prints the label, or falls back to the
        // key count.
        row[QStringLiteral("pathLabel")] = key ? map_value(object_path_labels_, *key) : QString();
        out.append(row);
    }
    return out;
}

QVariantList EncoderController::liveObjectChannels() const {
    QVariantList out;
    for (const auto channel : liveSlotChannels()) {
        out.append(channel);
    }
    return out;
}

int EncoderController::liveObjectSlotsBound() const {
    int bound = 0;
    for (const auto channel : liveSlotChannels()) {
        if (channel >= 0) {
            ++bound;
        }
    }
    return bound;
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
        // "4 of 6 bed positions fed" - counted by panning the objects
        // exactly as the encoder will (fedChannels' own atmos branch), so
        // the plan strip and the soundfield dots can never disagree.
        const auto fed = fedChannels();
        const auto nfed = std::ranges::count(fed, true);
        return QStringLiteral("%1 of 6 bed positions fed · JOC + OAMD · objects carry the height")
            .arg(nfed);
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

int EncoderController::codedChannelCount() const {
    if (isDualMono() && !atmos_enabled_) {
        return 2;
    }
    const auto cp = atmos_enabled_ ? plan::channel_plan_for(plan::LayoutId::k51)
                                   : effectiveChannelPlan();
    return static_cast<int>(plan::coded_channels(cp).size());
}

int EncoderController::renderedChannelCount() const {
    if (isDualMono() && !atmos_enabled_) {
        return 2;
    }
    const auto cp = atmos_enabled_ ? plan::channel_plan_for(plan::LayoutId::k51)
                                   : effectiveChannelPlan();
    return plan::rendered_channel_count(cp);
}

QString EncoderController::mapToken() const {
    if (!has_explicit_assignment_ || !source_) {
        return {};
    }
    return QStringLiteral("map=%1")
        .arg(to_qstring(plan::format_assignment(sourceShapes(), assignment_)));
}

QString EncoderController::metaTokens() const {
    // print_meta_usage()'s exact grammar, emitted only where the value
    // differs from a default-constructed plan::Metadata - "everything
    // defaults off, so a command line that says nothing about metadata
    // produces exactly the stream it produced before this layer existed",
    // and the reverse direction should hold too.
    const plan::Metadata defaults{};
    QStringList tokens;
    if (drc_index_ > 0) {
        tokens.append(QStringLiteral("drc=%1").arg(to_qstring(ac3::meta::profile_name(
            kDrcProfiles[static_cast<std::size_t>(drc_index_ - 1)]))));
    }
    if (meta_.heavy) {
        tokens.append(QStringLiteral("heavy"));
        if (ceiling_db_ != -0.5) {
            tokens.append(QStringLiteral("ceiling=%1").arg(ceiling_db_));
        }
        if (dialogue_db_ != -20.0) {
            tokens.append(QStringLiteral("dialogue=%1").arg(dialogue_db_));
        }
    }
    if (meta_.measure_dialnorm) {
        tokens.append(QStringLiteral("dialnorm=auto"));
    } else if (meta_.dialnorm != defaults.dialnorm) {
        tokens.append(QStringLiteral("dialnorm=%1").arg(meta_.dialnorm));
    }
    if (isDualMono() && !atmos_enabled_) {
        if (meta_.measure_dialnorm2) {
            tokens.append(QStringLiteral("dialnorm2=auto"));
        } else if (meta_.dialnorm2 != defaults.dialnorm2) {
            tokens.append(QStringLiteral("dialnorm2=%1").arg(meta_.dialnorm2));
        }
    }
    if (meta_.cmixlev != defaults.cmixlev) {
        static constexpr std::array<const char*, 3> kCmix = {"-3", "-4.5", "-6"};
        tokens.append(QStringLiteral("cmixlev=%1")
                          .arg(QLatin1String(kCmix[static_cast<std::size_t>(meta_.cmixlev)])));
    }
    if (meta_.surmixlev != defaults.surmixlev) {
        static constexpr std::array<const char*, 3> kSurmix = {"-3", "-6", "off"};
        tokens.append(QStringLiteral("surmixlev=%1")
                          .arg(QLatin1String(kSurmix[static_cast<std::size_t>(meta_.surmixlev)])));
    }
    if (codec_ == plan::Codec::kEac3 && meta_.mixmeta) {
        tokens.append(QStringLiteral("mixmeta"));
    }
    // lfemix's default is a VALUE (kLfeMixLevelIdeal), so "off" is the
    // non-default worth spelling; dmixmod's default is Lo/Ro, so "ltrt" and
    // the explicit "none" are the two that need saying.
    if (meta_.lfemix != defaults.lfemix) {
        tokens.append(meta_.lfemix ? QStringLiteral("lfemix=%1").arg(*meta_.lfemix)
                                   : QStringLiteral("lfemix=off"));
    }
    if (meta_.dmixmod != defaults.dmixmod) {
        const QString name = meta_.dmixmod == ac3::meta::DownmixMode::kLtRt
                                 ? QStringLiteral("ltrt")
                                 : meta_.dmixmod == ac3::meta::DownmixMode::kLoRo
                                       ? QStringLiteral("loro")
                                       : QStringLiteral("none");
        tokens.append(QStringLiteral("dmixmod=%1").arg(name));
    }
    return tokens.join(QLatin1Char(' '));
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
    // E-AC-3 signals frmsiz directly rather than indexing the table, so
    // rungs past AC-3's 640 ceiling are legal there - 768 is what a wide
    // object/7.2.4 session actually wants. setCodecIndex clamps back down
    // when a switch to AC-3 would leave a rate Table 5.18 cannot express.
    if (codec_ == plan::Codec::kEac3) {
        out.append(768);
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
        // The E-AC-3-only rungs above 640 have no Table 5.18 code to fall
        // back on - clamped for the same silently-invalid-plan reason the
        // extras are dropped.
        if (bitrate_kbps_ > 640) {
            bitrate_kbps_ = 640;
        }
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
        // The frame must hold a 5.1 bed plus the JOC+OAMD payload - the same
        // floor the assignment table's "obj" path already raises to, applied
        // here too so the switch is never "a flag the table ignores".
        if (bitrate_kbps_ < 384) {
            bitrate_kbps_ = 384;
        }
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
    const auto key = keyForObjectIndex(objectIndex);
    if (!key) {
        return;
    }
    auto& config = object_configs_[*key];
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
    const auto key = keyForObjectIndex(objectIndex);
    if (!key) {
        return;
    }
    auto& config = object_configs_[*key];
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

std::vector<int> EncoderController::liveSlotChannels() const {
    std::lock_guard lock(live_object_mutex_);
    return live_slot_channels_;
}

void EncoderController::setObjectPathKeyframes(int objectIndex, const QVariantList& keyframes,
                                               const QString& label) {
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return;
    }
    if (keyframes.isEmpty()) {
        object_keyframes_.erase(*object_key);
        object_path_labels_.erase(*object_key);
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
    object_keyframes_[*object_key] = std::move(parsed);
    if (label.isEmpty()) {
        object_path_labels_.erase(*object_key);
    } else {
        object_path_labels_[*object_key] = label;
    }
    emit objectsChanged();
}

void EncoderController::clearObjectPath(int objectIndex) {
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return;
    }
    object_path_labels_.erase(*object_key);
    if (object_keyframes_.erase(*object_key) > 0) {
        emit objectsChanged();
    }
}

std::vector<ac3::oba::Keyframe> EncoderController::sortedKeyframes(int objectIndex) const {
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return {};
    }
    auto keyframes = map_value(object_keyframes_, *object_key);
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
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return;
    }
    const auto config = map_value(object_configs_, *object_key);
    auto keyframes = sortedKeyframes(objectIndex);
    // Same moment, not the same float: two cues a hundredth of a second apart
    // are not a user trying to nudge one, they are a mis-click.
    constexpr double kSameInstant = 0.01;
    const auto existing = std::ranges::find_if(keyframes, [&](const ac3::oba::Keyframe& key) {
        return std::abs(key.time_s - timeS) < kSameInstant;
    });
    // Seeded with the same inverse-root gain a path-less object encodes at
    // (encodeObjects' static fallback): unity here made an object ~3-9 dB
    // louder the moment its first key was added, and several keyed objects
    // could sum the shared bed past the headroom rule the fallback exists
    // for. If keyframes already carry an authored gain, that gain is theirs;
    // this only decides what a NEW cue starts from.
    const auto ndynamic =
        std::max<std::size_t>(std::min<std::size_t>(dynamicObjectChannels().size(), 15), 1);
    ac3::oba::Keyframe key{.time_s = timeS,
                           .position = {.x = config.x, .y = config.y, .z = config.z},
                           .gain = 0.7 / std::sqrt(static_cast<double>(ndynamic)),
                           .lfe_send = config.lfe_send};
    if (existing != keyframes.end()) {
        key.gain = existing->gain;
        *existing = key;
    } else {
        keyframes.push_back(key);
    }
    object_keyframes_[*object_key] = std::move(keyframes);
    // A hand-placed cue on a preset-authored path means the path is no
    // longer purely that preset.
    object_path_labels_.erase(*object_key);
    emit objectsChanged();
}

void EncoderController::moveObjectKeyframe(int objectIndex, double fromS, double toS) {
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return;
    }
    auto keyframes = sortedKeyframes(objectIndex);
    constexpr double kSameInstant = 0.01;
    const auto found = std::ranges::find_if(keyframes, [&](const ac3::oba::Keyframe& key) {
        return std::abs(key.time_s - fromS) < kSameInstant;
    });
    if (found == keyframes.end()) {
        return;
    }
    auto moved = *found;
    moved.time_s = std::max(0.0, toS);
    keyframes.erase(found);
    // Landing on another key replaces it - addObjectKeyframe's same-moment
    // rule, so a drag can never stack two cues on one instant.
    std::erase_if(keyframes, [&](const ac3::oba::Keyframe& key) {
        return std::abs(key.time_s - moved.time_s) < kSameInstant;
    });
    keyframes.push_back(moved);
    object_keyframes_[*object_key] = std::move(keyframes);
    object_path_labels_.erase(*object_key);
    emit objectsChanged();
}

void EncoderController::shiftObjectKeyframes(int objectIndex, double deltaSeconds) {
    if (deltaSeconds == 0.0) {
        return;
    }
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return;
    }
    auto keyframes = sortedKeyframes(objectIndex);
    if (keyframes.empty()) {
        return;
    }
    // One bulk rewrite, not N calls to moveObjectKeyframe - see this
    // method's own doc comment on why a per-key shift has an ordering
    // hazard this does not. The clamp keeps the whole path in bounds
    // together, rather than the negative-time keys alone catching up at 0
    // while the rest have already moved by the full delta.
    double earliest = keyframes.front().time_s;
    for (const auto& key : keyframes) {
        earliest = std::min(earliest, key.time_s);
    }
    const double clamped_delta = std::max(deltaSeconds, -earliest);
    if (clamped_delta == 0.0) {
        return;
    }
    for (auto& key : keyframes) {
        key.time_s += clamped_delta;
    }
    object_keyframes_[*object_key] = std::move(keyframes);
    // The path's shape (every key's relative spacing) is unchanged - only
    // its position on the timeline moved - so a preset's own label survives
    // this the way a hand-authored edit's does not.
    emit objectsChanged();
}

void EncoderController::removeObjectKeyframe(int objectIndex, double timeS) {
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return;
    }
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
        object_keyframes_.erase(*object_key);
    } else {
        object_keyframes_[*object_key] = std::move(keyframes);
    }
    object_path_labels_.erase(*object_key);
    emit objectsChanged();
}

QVariantMap EncoderController::evaluateObjectPath(int objectIndex, double timeS) const {
    QVariantMap out;
    const auto object_key = keyForObjectIndex(objectIndex);
    if (!object_key) {
        return out;
    }
    const auto config = map_value(object_configs_, *object_key);
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

bool EncoderController::exportObjectPaths(const QUrl& url) const {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    out.setRealNumberPrecision(9);
    const auto dynamic = dynamicObjectChannels();
    const auto ndynamic =
        std::max<std::size_t>(std::min<std::size_t>(dynamic.size(), 15), 1);
    for (int i = 0; i < object_count_; ++i) {
        // An objm group's export uses its first channel's flat index - the
        // atmos-encode file format this feeds has no concept of a folded
        // group (the same gap bed-pinned channels have, per this function's
        // own header comment: they are not written at all; a group at
        // least gets a representative single line here).
        const auto flat = static_cast<std::size_t>(i) < dynamic.size()
                              ? dynamic[static_cast<std::size_t>(i)].front()
                              : static_cast<std::size_t>(i);
        const auto keyframes = sortedKeyframes(i);
        if (keyframes.empty()) {
            // No authored path - the object's static position is still
            // worth writing, as a single time-0 keyframe under exactly the
            // gain/lfe_send law encodeObjects' own fallback applies, so the
            // exported file reproduces this object's actual placement
            // rather than atmos-encode's own built-in default for it.
            const auto object_key = keyForObjectIndex(i);
            const auto config = object_key ? map_value(object_configs_, *object_key)
                                           : ObjectConfig{};
            const auto scale = 1.0 / std::sqrt(static_cast<double>(ndynamic));
            out << flat << ' ' << 0.0 << ' ' << config.x << ' ' << config.y << ' ' << config.z
                << ' ' << 0.7 * scale << ' ' << config.lfe_send * scale << '\n';
            continue;
        }
        for (const auto& key : keyframes) {
            out << flat << ' ' << key.time_s << ' ' << key.position.x << ' ' << key.position.y
                << ' ' << key.position.z << ' ' << key.gain << ' ' << key.lfe_send << '\n';
        }
    }
    return out.status() == QTextStream::Ok;
}

void EncoderController::startMotionPreview() {
    if (busy_ || !atmos_enabled_ || !source_ || object_count_ <= 0 || motion_preview_active_) {
        return;
    }

    const auto p = currentPlan();

    // Exactly encodeObjects()'s own `paths` construction: authored keyframes
    // where the GUI has some, else each object's static position, plus one
    // pinned path per bed-assigned channel. Built here, on the GUI thread,
    // and moved into the worker below, so the worker never reads
    // object_keyframes_/object_configs_ directly.
    const auto dynamic = dynamicObjectChannels();
    const auto pinned = pinnedObjectChannels();
    const std::size_t ndynamic = std::min<std::size_t>(dynamic.size(), 15);
    const std::size_t nobjects = ndynamic + pinned.size();

    std::vector<ac3::oba::ObjectPath> paths;
    paths.reserve(nobjects);
    for (std::size_t i = 0; i < ndynamic; ++i) {
        const auto object_key = sourceChannelForFlatIndex(dynamic[i].front());
        const auto authored = object_keyframes_.find(object_key);
        if (authored != object_keyframes_.end() && !authored->second.empty()) {
            auto created = ac3::oba::KeyframePath::create(authored->second);
            if (created) {
                paths.emplace_back(std::move(*created));
                continue;
            }
        }
        const auto config = map_value(object_configs_, object_key);
        auto fallback = ac3::oba::KeyframePath::create(
            {{.time_s = 0.0,
              .position = {.x = config.x, .y = config.y, .z = config.z},
              .gain = 0.7 / std::sqrt(static_cast<double>(std::max<std::size_t>(ndynamic, 1))),
              .lfe_send = config.lfe_send /
                          std::sqrt(static_cast<double>(std::max<std::size_t>(ndynamic, 1)))}});
        paths.emplace_back(std::move(*fallback));
    }
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

    // The audio each object actually carries: the loaded source's own real
    // per-channel audio - the same `planes` encodeTo() builds, offsets baked
    // in the same way (apply_channel_offsets + flatChannelOffsetSamples), so
    // a preview honours each source's start offset exactly like a real
    // encode does. Then repacked to object order (dynamic first, then
    // pinned), the same as encodeObjects()'s own `object_planes` - each
    // dynamic entry via buildObjectPlane, so a trimmed channel or an objm
    // fold sounds exactly as it will in the real encode, not merely as it
    // looked in the room plan.
    const auto sample_rate = source_->wav.sample_rate;
    std::vector<std::vector<float>> planes = source_->wav.channels;
    for (const auto& extra : extra_sources_) {
        planes.insert(planes.end(), extra->wav.channels.begin(), extra->wav.channels.end());
    }
    planes = apply_channel_offsets(std::move(planes), flatChannelOffsetSamples(sample_rate));

    std::vector<std::vector<float>> object_planes;
    object_planes.reserve(nobjects);
    for (std::size_t i = 0; i < ndynamic; ++i) {
        object_planes.push_back(buildObjectPlane(dynamic[i], planes));
    }
    for (const auto& [flat, location] : pinned) {
        object_planes.push_back(std::move(planes[flat]));
    }
    planes = std::move(object_planes);

    // Opened here, on the GUI thread - mirrors startLiveSession's own
    // MonitorSink::start() call, using the loaded source's own rate since
    // there is no capture device involved in a file preview.
    motion_preview_monitor_sink_ = std::make_unique<ac3::sinks::MonitorSink>();
    const auto started =
        motion_preview_monitor_sink_->start(std::string{}, sample_rate, /*channels=*/6);
    if (!started) {
        const auto why = ac3::sinks::describe(started.error());
        motion_preview_monitor_sink_.reset();
        setStatus(QStringLiteral("Could not open the preview output: %1")
                      .arg(QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()))));
        return;
    }

    // The meters follow the bed, same as encodeObjects()'s own setLayout()
    // call.
    const auto coded = plan::coded_channels(plan::LayoutId::k51);
    const auto names = plan::coded_channel_names(plan::LayoutId::k51);
    QStringList labels;
    for (const auto& name : names) {
        labels.append(QString::fromStdString(name));
    }
    setLayout(ac3::Acmod::k3_2, true, labels, QStringLiteral("5.1 bed"), coded, fedChannels());
    setMetering(true);
    clearClipLatches();

    stop_motion_preview_.store(false, std::memory_order_relaxed);
    motion_preview_active_ = true;
    motion_preview_time_ = 0.0;
    emit motionPreviewActiveChanged();
    emit motionPreviewTimeChanged();
    setBusy(true);

    std::ignore = QtConcurrent::run([this, p, sample_rate, nobjects, paths = std::move(paths),
                                     planes = std::move(planes)]() mutable {
        // Heap-allocated - see encodeObjects'/runLiveSession's own PREfast
        // C6262 comment on why: a multi-KB internal history buffer pushes a
        // worker thread's stack frame too far.
        auto encoder = std::make_unique<ac3::oba::AtmosEncoder>(
            ac3::oba::AtmosConfig{.sample_rate = p.sample_rate,
                                  .bitrate_kbps = p.bitrate_kbps,
                                  .dialnorm = p.meta.dialnorm,
                                  .num_bands_idx = 4},
            static_cast<int>(nobjects));

        ac3::analysis::LevelMeter meter{ac3::Acmod::k3_2, true, sample_rate};

        // The longest of the channels actually used as an object, same as
        // encodeObjects()'s own `total`.
        std::size_t total = 0;
        for (std::size_t ch = 0; ch < nobjects; ++ch) {
            total = std::max(total, planes[ch].size());
        }
        std::vector<std::vector<float>> block(nobjects,
                                              std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> views(nobjects);
        std::vector<std::span<const float>> metered(6);
        // The bed comes back in AC-3 coded order (L C R Ls Rs LFE); this is
        // the same coded-to-WAV permutation runLiveSession's own monitor
        // path reorders a decoded frame with.
        const std::vector<std::size_t> order =
            ac3::io::wav_channel_order(ac3::Acmod::k3_2, /*lfe=*/true);
        QString problem;
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;

        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            if (stop_motion_preview_.load(std::memory_order_relaxed)) {
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
            // frame - same convention encodeObjects() uses.
            const double t = static_cast<double>(start + ac3::kSamplesPerFrame) /
                             static_cast<double>(sample_rate);
            const auto placement = ac3::oba::evaluate_placements(paths, t);
            const auto unit = encoder->encode_frame(views, placement);
            if (!unit) {
                problem = QStringLiteral(
                    "The frame cannot hold a 5.1 bed and the object metadata at this bit "
                    "rate — try 384 kbps or more.");
                break;
            }
            for (std::size_t ch = 0; ch < metered.size(); ++ch) {
                metered[ch] = std::span{encoder->bed()[ch]}.first(valid);
            }
            meter.process(metered);

            // Real playback, not just metering: interleaved into WAV/
            // playback order and submitted to the sink directly from this
            // worker thread - matches motion_preview_monitor_sink_'s own
            // doc comment ("submit-from-worker", the same convention
            // live_monitor_sink_ uses in runLiveSession). submit() is
            // non-blocking and fails while the device's own queue (about a
            // second deep) is full, so retrying on a short sleep is what
            // paces this at real wall-clock speed instead of flat-out -
            // the queue itself is the real-time clock, exactly as it is for
            // runLiveSession's monitor leg.
            const auto interleaved = interleave_reordered(encoder->bed(), order);
            while (!motion_preview_monitor_sink_->submit(interleaved) &&
                  !stop_motion_preview_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                   meter.levels().end());
                QMetaObject::invokeMethod(this, [this, t, snapshot = std::move(snapshot)] {
                    motion_preview_time_ = t;
                    emit motionPreviewTimeChanged();
                    publishLevels(snapshot);
                });
            }
        }

        QMetaObject::invokeMethod(this, [this, problem] {
            if (motion_preview_monitor_sink_) {
                motion_preview_monitor_sink_->stop();
                motion_preview_monitor_sink_.reset();
            }
            setBusy(false);
            setMetering(false);
            motion_preview_active_ = false;
            emit motionPreviewActiveChanged();
            motion_preview_time_ = 0.0;
            emit motionPreviewTimeChanged();
            if (!problem.isEmpty()) {
                setStatus(problem);
            }
        });
    });
}

void EncoderController::stopMotionPreview() {
    if (!motion_preview_active_) {
        return;
    }
    // The worker's own completion callback (above) does the actual teardown
    // - closing motion_preview_monitor_sink_, clearing busy_/active_,
    // publishing a final meter snapshot - once it notices this flag and
    // unwinds; the same asymmetry stopLiveSession()/runLiveSession's
    // completion callback already has.
    stop_motion_preview_.store(true, std::memory_order_relaxed);
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

std::vector<std::size_t> EncoderController::flatChannelOffsetSamples(
    std::uint32_t sample_rate) const {
    std::vector<std::size_t> out;
    if (!source_) {
        return out;
    }
    const auto to_samples = [sample_rate](double seconds) {
        return static_cast<std::size_t>(
            std::llround(std::max(0.0, seconds) * static_cast<double>(sample_rate)));
    };
    out.insert(out.end(), source_->wav.channels.size(), to_samples(source_offset_seconds_));
    for (std::size_t i = 0; i < extra_sources_.size(); ++i) {
        const auto offset =
            to_samples(i < extra_source_offsets_seconds_.size() ? extra_source_offsets_seconds_[i]
                                                                 : 0.0);
        out.insert(out.end(), extra_sources_[i]->wav.channels.size(), offset);
    }
    return out;
}

EncoderController::ObjectKey EncoderController::sourceChannelForFlatIndex(
    std::size_t flatIndex) const {
    const auto shapes = sourceShapes();
    std::size_t base = 0;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        if (flatIndex < base + shapes[s].channels) {
            return {s, flatIndex - base};
        }
        base += shapes[s].channels;
    }
    // Past every loaded source's channels - object_count_ is capped to the
    // sum of them (see refreshAfterSourceListChange), so this is defensive
    // only, not a path real state reaches.
    return {shapes.empty() ? 0 : shapes.size() - 1, flatIndex};
}

QString EncoderController::objectSourceLabel(std::size_t flatIndex) const {
    const auto shapes = sourceShapes();
    // Exactly one source: unchanged from what objectModel() has always
    // shown - there is nothing a filename would add over a plain channel
    // number when only one file is in play.
    if (shapes.size() <= 1) {
        return QStringLiteral("Ch %1").arg(flatIndex + 1);
    }
    const auto [s, c] = sourceChannelForFlatIndex(flatIndex);
    if (s >= shapes.size()) {
        return QStringLiteral("Ch %1").arg(flatIndex + 1);
    }
    return QStringLiteral("%1 ch %2").arg(QString::fromStdString(shapes[s].label)).arg(c + 1);
}

QString EncoderController::objectSourceLabel(const std::vector<std::size_t>& group) const {
    if (group.size() <= 1) {
        return objectSourceLabel(group.empty() ? std::size_t{0} : group.front());
    }
    const auto shapes = sourceShapes();
    const auto [s, first] = sourceChannelForFlatIndex(group.front());
    const auto [s2, last] = sourceChannelForFlatIndex(group.back());
    // A contiguous objm group is always within one source (see
    // DestinationKind::kObjectMono's own comment), so s2 == s always holds
    // here - s2 is unused beyond that guarantee.
    std::ignore = s2;
    if (s >= shapes.size()) {
        return QStringLiteral("Ch %1-%2 (mono)").arg(first + 1).arg(last + 1);
    }
    return QStringLiteral("%1 ch %2-%3 (mono)")
        .arg(QString::fromStdString(shapes[s].label))
        .arg(first + 1)
        .arg(last + 1);
}

std::vector<float> EncoderController::buildObjectPlane(
    const std::vector<std::size_t>& group, const std::vector<std::vector<float>>& planes) const {
    const auto gain_for = [this](std::size_t flat) {
        if (!has_explicit_assignment_) {
            return 1.0;
        }
        const auto [s, c] = sourceChannelForFlatIndex(flat);
        return std::pow(10.0, assignment_.at(s, c).trim_db / 20.0);
    };
    if (group.size() == 1) {
        const auto flat = group.front();
        std::vector<float> out = planes[flat];
        const auto gain = gain_for(flat);
        if (gain != 1.0) {
            for (auto& sample : out) {
                sample = static_cast<float>(static_cast<double>(sample) * gain);
            }
        }
        return out;
    }
    std::size_t len = 0;
    for (const auto flat : group) {
        len = std::max(len, planes[flat].size());
    }
    std::vector<float> out(len, 0.0f);
    // Equal-weight fold: each channel's own trim first, then 1/n so several
    // full-range channels summed together do not clip past what one alone
    // would - see DestinationKind::kObjectMono's own comment.
    const double scale = 1.0 / static_cast<double>(group.size());
    for (const auto flat : group) {
        const double gain = gain_for(flat) * scale;
        const auto& plane = planes[flat];
        for (std::size_t i = 0; i < plane.size(); ++i) {
            out[i] += static_cast<float>(static_cast<double>(plane[i]) * gain);
        }
    }
    return out;
}

std::optional<EncoderController::ObjectKey> EncoderController::keyForObjectIndex(
    int objectIndex) const {
    if (objectIndex < 0 || objectIndex >= object_count_) {
        return std::nullopt;
    }
    if (live_object_backup_) {
        // A live session has resized object_configs_/object_keyframes_/
        // object_path_labels_ over the capture device's own channel count
        // instead of a loaded file's (see startLiveSession) - there is no
        // (source, channel) to hang a live-only object off, so it addresses
        // the device channel directly under a reserved sentinel source. The
        // file's real identities sit safely in live_object_backup_ until
        // the session ends and they are restored.
        return ObjectKey{kLiveObjectSource, static_cast<std::size_t>(objectIndex)};
    }
    const auto dynamic = dynamicObjectChannels();
    if (static_cast<std::size_t>(objectIndex) >= dynamic.size()) {
        return std::nullopt;
    }
    // A group's IDENTITY channel is its first - stable as long as the
    // group's own shape does not change - what object_configs_/
    // object_keyframes_/object_path_labels_ key authored state by, single
    // channel or objm group alike.
    const auto& group = dynamic[static_cast<std::size_t>(objectIndex)];
    if (group.empty()) {
        return std::nullopt;  // defensive; dynamicObjectChannels() never emits one
    }
    return sourceChannelForFlatIndex(group.front());
}

std::vector<std::vector<std::size_t>> EncoderController::dynamicObjectChannels() const {
    std::vector<std::vector<std::size_t>> out;
    const auto shapes = sourceShapes();
    std::size_t flat = 0;
    for (std::size_t s = 0; s < shapes.size(); ++s) {
        for (std::size_t c = 0; c < shapes[s].channels;) {
            if (!has_explicit_assignment_) {
                out.push_back({flat});
                ++c;
                ++flat;
                continue;
            }
            const auto kind = assignment_.at(s, c).kind;
            if (kind == plan::DestinationKind::kObject) {
                out.push_back({flat});
                ++c;
                ++flat;
            } else if (kind == plan::DestinationKind::kObjectMono) {
                // The maximal contiguous run of kObjectMono rows on this
                // source - the group boundary DestinationKind::kObjectMono's
                // own comment defines. Cannot cross a source boundary: the
                // inner for's own condition (c < shapes[s].channels) stops
                // it there regardless of what the NEXT source's channel 0
                // happens to be assigned.
                std::vector<std::size_t> group;
                do {
                    group.push_back(flat);
                    ++c;
                    ++flat;
                } while (c < shapes[s].channels &&
                         assignment_.at(s, c).kind == plan::DestinationKind::kObjectMono);
                out.push_back(std::move(group));
            } else {
                ++c;
                ++flat;  // not a dynamic-object destination
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

int EncoderController::pinnedObjectCount() const {
    return static_cast<int>(pinnedObjectChannels().size());
}

QString EncoderController::groupDigits(qint64 value) const {
    return group_digits(value);
}

bool EncoderController::outputDeviceCanBitstream(int deviceIndex) const {
    if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= outputs_.size()) {
        return false;
    }
    const auto& device = outputs_[static_cast<std::size_t>(deviceIndex)];
    return output_eac3_ ? device.supports_eac3_passthrough : device.supports_ac3_passthrough;
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

    if (isDualMono()) {
        // The mockup's own dual sentence, whether or not a source is loaded:
        // there is no fold-down story to tell, because the two programmes
        // never meet in a downmix at all.
        routing_summary_ = QStringLiteral(
            "Two programmes are multiplexed into one stream. A receiver plays one or the "
            "other; they are never heard together, so no downmix coefficients apply.");
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
    // Drop any selection a device refresh made stale (unplugged, or the
    // list simply shrank) - the same "an index past the list is nothing"
    // convention startLiveSession's own device-index validation already
    // applies. A first refresh with nothing selected yet picks the default
    // (or first) device as master, matching what a plain ComboBox always
    // auto-selected before the rail had a device LIST to pick from.
    const auto before = live_selected_devices_;
    std::erase_if(live_selected_devices_, [this](int index) {
        return index < 0 || static_cast<std::size_t>(index) >= devices_.size();
    });
    if (live_selected_devices_.empty() && !devices_.empty()) {
        std::size_t default_index = 0;
        for (std::size_t i = 0; i < devices_.size(); ++i) {
            if (devices_[i].is_default) {
                default_index = i;
                break;
            }
        }
        live_selected_devices_.push_back(static_cast<int>(default_index));
    }
    if (live_selected_devices_ != before) {
        emit captureDeviceRowsChanged();
    }
}

QVariantList EncoderController::captureDeviceRows() const {
    QVariantList out;
    for (std::size_t slot = 0; slot < live_selected_devices_.size(); ++slot) {
        const auto device_index = live_selected_devices_[slot];
        if (device_index < 0 || static_cast<std::size_t>(device_index) >= devices_.size()) {
            continue;  // refreshCaptureDevices() keeps this in sync; defensive only
        }
        const auto& device = devices_[static_cast<std::size_t>(device_index)];
        QVariantMap row;
        row[QStringLiteral("slotIndex")] = static_cast<int>(slot);
        row[QStringLiteral("deviceIndex")] = device_index;
        row[QStringLiteral("name")] = QString::fromStdString(device.name);
        row[QStringLiteral("channels")] = device.channels;
        row[QStringLiteral("rateText")] =
            QStringLiteral("%1 ch · %2 Hz").arg(device.channels).arg(group_digits(device.sample_rate));
        row[QStringLiteral("isMaster")] = slot == 0;
        out.append(row);
    }
    return out;
}

QString EncoderController::captureDeviceTotals() const {
    if (live_selected_devices_.empty()) {
        return QString();
    }
    int total_channels = 0;
    for (const auto device_index : live_selected_devices_) {
        if (device_index >= 0 && static_cast<std::size_t>(device_index) < devices_.size()) {
            total_channels += devices_[static_cast<std::size_t>(device_index)].channels;
        }
    }
    return live_selected_devices_.size() == 1
              ? QStringLiteral("1 device · %1 channels captured").arg(total_channels)
              : QStringLiteral("%1 devices · %2 channels captured")
                    .arg(live_selected_devices_.size())
                    .arg(total_channels);
}

QStringList EncoderController::liveCaptureChannelLabels() const {
    QStringList out;
    if (live_selected_devices_.empty() ||
        static_cast<std::size_t>(live_selected_devices_.front()) >= devices_.size()) {
        return out;
    }
    const auto& master = devices_[static_cast<std::size_t>(live_selected_devices_.front())];
    for (std::uint16_t ch = 0; ch < master.channels; ++ch) {
        out.append(QStringLiteral("Ch %1").arg(ch + 1));
    }
    if (live_selected_devices_.size() > 1 &&
        static_cast<std::size_t>(live_selected_devices_[1]) < devices_.size()) {
        const auto& slave = devices_[static_cast<std::size_t>(live_selected_devices_[1])];
        for (std::uint16_t ch = 0; ch < slave.channels; ++ch) {
            out.append(QStringLiteral("Dev2 Ch %1").arg(ch + 1));
        }
    }
    return out;
}

void EncoderController::addCaptureDevice(int deviceIndex) {
    if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= devices_.size()) {
        return;
    }
    if (live_selected_devices_.size() >= 2 ||
        std::ranges::find(live_selected_devices_, deviceIndex) != live_selected_devices_.end()) {
        return;
    }
    live_selected_devices_.push_back(deviceIndex);
    emit captureDeviceRowsChanged();
}

void EncoderController::removeCaptureDevice(int slotIndex) {
    if (slotIndex < 0 || static_cast<std::size_t>(slotIndex) >= live_selected_devices_.size()) {
        return;
    }
    live_selected_devices_.erase(live_selected_devices_.begin() + slotIndex);
    emit captureDeviceRowsChanged();
}

QString EncoderController::liveDriftText() const {
    if (!live_second_device_active_) {
        return QString();
    }
    // Signed so a fast-running slave reads "+" and a slow one "-" - the
    // resampler's own sign convention (see ClockDriftEstimator's doc
    // comment): positive means the slave is arriving faster than it is
    // being drained and the resampler is dropping extra input to compensate.
    return QStringLiteral("slave %1%2 ppm")
        .arg(live_drift_ppm_ >= 0.0 ? QStringLiteral("+") : QStringLiteral("−"))
        .arg(QString::number(std::abs(live_drift_ppm_), 'f', 1));
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

void EncoderController::startRun(const QString& path, const QString& durationText,
                                 const QString& label, bool forceCbr) {
    QString duration = durationText;
    if (duration.isEmpty()) {
        double seconds = 0.0;
        if (source_ && source_->wav.sample_rate > 0) {
            seconds = static_cast<double>(source_->wav.frame_count()) /
                      static_cast<double>(source_->wav.sample_rate);
        }
        duration = QStringLiteral("%1:%2")
                       .arg(static_cast<int>(seconds) / 60)
                       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'));
    }
    QVariantMap run;
    run[QStringLiteral("id")] = next_run_id_;
    run[QStringLiteral("filename")] =
        label.isEmpty() ? QFileInfo(path).fileName() : label;
    // The full path, so a finished chip's "Show in folder" has something
    // real to reveal; empty for a session that writes no file.
    run[QStringLiteral("path")] = path;
    run[QStringLiteral("bitrateKbps")] = bitrate_kbps_;
    run[QStringLiteral("durationText")] = duration;
    run[QStringLiteral("status")] = QStringLiteral("encoding");
    run[QStringLiteral("sizeText")] = QString();
    run[QStringLiteral("detail")] = QString();
    run[QStringLiteral("framesText")] = QString();
    // A VBR run has no target rate to show while it is still running - only
    // the quality it is aiming for. finishRun() replaces this with the real
    // avg/min/max once the run's actual frame sizes are known. forceCbr is
    // the live session, which drops VBR unconditionally (runLiveSession).
    run[QStringLiteral("rateText")] =
        (!forceCbr && vbr_enabled_ && codec_ == plan::Codec::kEac3 && !atmos_enabled_)
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
        // "212 frames" for the failed/cancelled chip - read out of the same
        // message the status line shows ("Wrote 212 frames…", "…The 212
        // frames already written are kept…"), not counted a second time.
        static const QRegularExpression kFramesPattern(
            QStringLiteral(R"((\d+) (?:Atmos access units|access units|frames))"));
        const auto frames = kFramesPattern.match(message);
        if (frames.hasMatch()) {
            run[QStringLiteral("framesText")] =
                QStringLiteral("%1 frames").arg(group_digits(frames.captured(1).toLongLong()));
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
        // Only the currently-dynamic objects (0..object_count_-1), not
        // every entry object_configs_ happens to still hold - a channel
        // that is not an object right now (unassigned, pinned, or its
        // source briefly absent) does not feed anything, even though its
        // dormant config sits in the map waiting for a reassignment.
        for (int i = 0; i < object_count_; ++i) {
            const auto key = keyForObjectIndex(i);
            if (!key) {
                continue;
            }
            const auto config = map_value(object_configs_, *key);
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
    // Synchronous and unconditional, same as setLayout's own "start silent"
    // discipline for channelLevels: a rail row's pip must never show a
    // reading left over from a source that just changed position or left
    // entirely. The real numbers, if any, arrive later from the async pass
    // below and overwrite this.
    resetSourceLevels();
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
    const auto coded = plan::coded_channels(cp);
    QStringList labels;
    if (isDualMono()) {
        labels = {QStringLiteral("Program 1"), QStringLiteral("Program 2")};
    } else {
        for (const auto& name : plan::coded_channel_names(cp)) {
            labels.append(QString::fromStdString(name));
        }
    }
    setLayout(cp.bed_acmod, cp.bed_lfe, labels, effectiveLabel(), coded, fedChannels());

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
    // Captured by value alongside sources/sample_rate - planes below are
    // non-owning spans into shared WavData, so the offset has to apply as a
    // per-frame read-shift here rather than as leading silence baked into
    // owned storage (contrast encodeTo's apply_channel_offsets).
    const auto offsets = flatChannelOffsetSamples(sample_rate);
    // Which coded channels get the LFE low-pass - only ever non-empty when
    // this routing came from the assignment table (has_explicit_
    // assignment_), never the automatic single-source route() overload -
    // see lfe_coded_indices' own comment. Computed here, on the GUI thread,
    // same as sample_rate/acmod/lfe above.
    const auto lfe_indices = has_explicit_assignment_ ? lfe_coded_indices(coded)
                                                      : std::vector<std::size_t>{};
    std::ignore = QtConcurrent::run([this, generation, routing = *routing,
                                     sources = std::move(sources), sample_rate, acmod, lfe,
                                     offsets, lfe_indices] {
        const auto coded_count = static_cast<std::size_t>(routing.coded_channels);
        ac3::analysis::LevelMeter meter{acmod, lfe, sample_rate,
                                        static_cast<int>(coded_count)};
        std::vector<ac3::dsp::LfeLowpass> lfe_filters;
        lfe_filters.reserve(lfe_indices.size());
        for (std::size_t i = 0; i < lfe_indices.size(); ++i) {
            lfe_filters.emplace_back(kLfeLowpassCornerHz, sample_rate);
        }

        std::vector<std::span<const float>> planes;
        for (const auto& src : sources) {
            for (const auto& channel : src->wav.channels) {
                planes.emplace_back(channel);
            }
        }
        std::size_t total = 0;
        for (std::size_t ch = 0; ch < planes.size(); ++ch) {
            const auto offset = ch < offsets.size() ? offsets[ch] : 0;
            total = std::max(total, offset + planes[ch].size());
        }

        // Which SOURCE (not coded channel) each flat index belongs to - the
        // rail's per-source pip (sourceLevels) pools every one of that
        // source's own channels into one ac3::analysis::ChannelSummary,
        // read off this same pre-routing source_block the coded-channel
        // meter below reads from too, so the two never disagree about what
        // the loaded files actually contain.
        std::vector<std::size_t> flat_source;
        flat_source.reserve(planes.size());
        for (std::size_t si = 0; si < sources.size(); ++si) {
            flat_source.insert(flat_source.end(), sources[si]->wav.channels.size(), si);
        }
        std::vector<ac3::analysis::ChannelSummary> source_summaries(sources.size());

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
                const auto offset = ch < offsets.size() ? offsets[ch] : 0;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    const std::size_t shifted = at >= offset ? at - offset : 0;
                    source_block[ch][static_cast<std::size_t>(i)] =
                        at >= offset && shifted < len ? planes[ch][shifted] : 0.0f;
                }
                auto& acc = source_summaries[flat_source[ch]];
                for (std::size_t i = 0; i < valid; ++i) {
                    const double s = static_cast<double>(source_block[ch][i]);
                    acc.peak = std::max(acc.peak, std::abs(s));
                    acc.sum_squares += s * s;
                }
                acc.samples += valid;
            }
            plan::render(routing, in, out, ac3::kSamplesPerFrame);
            for (std::size_t i = 0; i < lfe_indices.size(); ++i) {
                lfe_filters[i].process(std::span<float>(block[lfe_indices[i]]));
            }
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
        QMetaObject::invokeMethod(this, [this, generation, totals = std::move(totals),
                                         source_summaries = std::move(source_summaries)] {
            // busy_ means a run owns the meters now; the generation check
            // drops a preview that answered a plan nobody is looking at.
            if (busy_ || generation != preview_generation_.load(std::memory_order_relaxed)) {
                return;
            }
            publishLevels(totals);
            publishSourceLevels(source_summaries);
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
    // Every current object index needs a config to show. An identity that
    // already has one - the common case, an object that survived whatever
    // just changed - keeps it untouched; only a genuinely new (source,
    // channel) (or, live, device-channel) identity gets a fresh default,
    // spread out along x rather than stacked on one point (the design
    // brief's own complaint about the old single-point-plus-spread model,
    // where six objects "overlap into a smear"). Nothing is ever pruned
    // here - see object_configs_'s own comment on why a dormant identity is
    // kept rather than dropped.
    for (std::size_t i = 0; i < count; ++i) {
        const auto key = keyForObjectIndex(static_cast<int>(i));
        if (!key || object_configs_.contains(*key)) {
            continue;
        }
        const double offset = count < 2 ? 0.0
                                        : 0.3 * (2.0 * static_cast<double>(i) /
                                                     static_cast<double>(count - 1) - 1.0);
        object_configs_[*key] = {.x = std::clamp(0.5 + offset, 0.0, 1.0),
                                 .y = 0.15,
                                 .z = 0.0,
                                 .lfe_send = 0.15};
    }
    if (selected_object_index_ >= static_cast<int>(count)) {
        selected_object_index_ = count > 0 ? static_cast<int>(count) - 1 : 0;
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
    clip_latched_.clear();
    soundfield_.clear();
    setMetering(false);
    emit layoutChanged();
    emit levelsChanged();
}

void EncoderController::publishLevels(std::span<const ac3::analysis::ChannelLevel> levels) {
    // Grow-or-shrink only - resize() never touches a surviving element's
    // value, which is exactly what a latch needs: it must not un-set itself
    // just because another snapshot arrived. Only clearClipLatches()
    // (called at a transport's start, never from here) actually zeroes one.
    clip_latched_.resize(levels.size(), false);

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
        // Latched, not raw: once true, clip_latched_[ch] stays true across
        // every future publishLevels() call until clearClipLatch(ch) or a
        // transport-start clearClipLatches() resets it - see ChannelMeter's
        // CLIP box, which reads exactly this field and has no latch logic
        // of its own.
        clip_latched_[ch] = clip_latched_[ch] || level.clipped;
        entries.append(QVariantMap{
            {QStringLiteral("peakDb"), level.peak_db},
            {QStringLiteral("rmsDb"), level.rms_db},
            {QStringLiteral("holdDb"), level.hold_db},
            // std::vector<bool>::operator[] returns a proxy reference, not a
            // plain bool - QVariant's constructor overload set can only
            // chain ONE implicit user-defined conversion, so the proxy has
            // to be cast explicitly rather than handed over as-is.
            {QStringLiteral("clipped"), static_cast<bool>(clip_latched_[ch])},
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

void EncoderController::clearClipLatches() {
    std::ranges::fill(clip_latched_, false);
    // publishLevels() itself is what channel_levels_'s `clipped` field
    // actually rebuilds from - clearing clip_latched_ alone would not clear
    // the ALREADY-PUBLISHED value QML is reading right now. Every call site
    // calls this immediately after setLayout()'s own publishLevels(default)
    // though, which already has clipped=false throughout (a fresh, all-
    // default ChannelLevel vector) - so no republish is needed here, unlike
    // clearClipLatch()'s single-channel case, which has no such guarantee
    // about what just ran before it.
}

void EncoderController::clearClipLatch(int channel) {
    if (channel < 0 || static_cast<std::size_t>(channel) >= clip_latched_.size()) {
        return;
    }
    clip_latched_[static_cast<std::size_t>(channel)] = false;
    if (static_cast<std::size_t>(channel) >= static_cast<std::size_t>(channel_levels_.size())) {
        return;
    }
    // Mutates the already-published entry directly so the box goes dark the
    // instant it is clicked, rather than waiting for whatever next tick
    // publishLevels() happens to run - which, once a run has finished and
    // metering is off, might be never.
    auto row = channel_levels_[channel].toMap();
    row[QStringLiteral("clipped")] = false;
    channel_levels_[channel] = row;
    emit levelsChanged();
}

void EncoderController::resetSourceLevels() {
    QVariantList out;
    for (std::size_t i = 0; i < sourceShapes().size(); ++i) {
        out.append(QVariantMap{
            {QStringLiteral("peakDb"), ac3::analysis::kFloorDb},
            {QStringLiteral("rmsDb"), ac3::analysis::kFloorDb},
        });
    }
    source_levels_ = std::move(out);
    emit sourceLevelsChanged();
}

void EncoderController::publishSourceLevels(std::span<const ac3::analysis::ChannelSummary> levels) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(levels.size()));
    for (const auto& summary : levels) {
        out.append(QVariantMap{
            {QStringLiteral("peakDb"), summary.peak_db()},
            {QStringLiteral("rmsDb"), summary.rms_db()},
        });
    }
    source_levels_ = std::move(out);
    emit sourceLevelsChanged();
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

void EncoderController::setLiveWavSafetyCopy(bool on) {
    if (on == live_wav_safety_copy_) {
        return;
    }
    live_wav_safety_copy_ = on;
    emit liveWavSafetyCopyChanged();
}

void EncoderController::addLiveObject(int captureChannel) {
    if (!live_active_ || !atmos_enabled_) {
        return;
    }
    if (captureChannel < 0 || captureChannel >= live_device_channels_) {
        return;
    }
    bool bound = false;
    {
        std::lock_guard lock(live_object_mutex_);
        for (auto& slot : live_slot_channels_) {
            if (slot < 0) {
                slot = captureChannel;
                bound = true;
                break;
            }
        }
    }
    if (bound) {
        emit objectsChanged();
    }
}

void EncoderController::reassignLiveObjectSlot(int slotIndex, int captureChannel) {
    if (!live_active_ || !atmos_enabled_) {
        return;
    }
    if (slotIndex < 0 || slotIndex >= object_count_) {
        return;
    }
    if (captureChannel >= live_device_channels_) {
        return;
    }
    {
        std::lock_guard lock(live_object_mutex_);
        if (static_cast<std::size_t>(slotIndex) >= live_slot_channels_.size()) {
            return;
        }
        // Any negative value silences the slot - the same "unbound" state
        // an allocated-but-never-bound slot already has, so a chip's
        // "silent" choice and "never touched" both read identically.
        live_slot_channels_[static_cast<std::size_t>(slotIndex)] =
            captureChannel < 0 ? -1 : captureChannel;
    }
    emit objectsChanged();
}

void EncoderController::switchLiveReceiver(int receiverDeviceIndex) {
    if (!live_active_) {
        return;
    }
    PendingReceiverSwitch request;
    if (receiverDeviceIndex >= 0) {
        if (static_cast<std::size_t>(receiverDeviceIndex) >= outputs_.size()) {
            return;
        }
        request.want_passthrough = true;
        request.receiver = outputs_[static_cast<std::size_t>(receiverDeviceIndex)];
    }
    // outputs_ is read here, on the GUI thread where it is safe to touch -
    // the worker thread never reaches into it itself, only into the
    // RenderDeviceInfo this copies out of it (see PendingReceiverSwitch's
    // own comment).
    std::lock_guard lock(live_receiver_switch_mutex_);
    live_receiver_switch_request_ = std::move(request);
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

std::unique_ptr<EncoderController::LiveOutputWriters> EncoderController::openLiveOutputWriters(
    const QString& path, bool write_to_disk, const ac3::capture::DeviceInfo& device) {
    if (!write_to_disk) {
        return nullptr;
    }
    auto writers = std::make_unique<LiveOutputWriters>();
    writers->final_path = path;
    writers->matroska = container_index_ == kContainerMatroska;
    const bool eac3 = atmos_enabled_ || codec_ == plan::Codec::kEac3;
    writers->stream_path = writers->matroska ? live_stream_spool_path(path, eac3) : path;
    writers->stream.open(writers->stream_path.toStdString(), std::ios::binary);
    if (!writers->stream) {
        setStatus(QStringLiteral("Could not open \"%1\" for writing.")
                      .arg(QFileInfo(writers->stream_path).fileName()));
        emit encodeRefused(status_);
        return nullptr;
    }
    if (live_wav_safety_copy_) {
        auto safety = std::make_unique<ac3::io::WavStreamWriter>();
        const QString safety_path = sibling_path(path, QStringLiteral(".raw.wav"));
        const auto opened = safety->open(safety_path.toStdString(), device.sample_rate,
                                         device.channels);
        if (!opened) {
            const auto why = ac3::io::describe(opened.error());
            setStatus(QStringLiteral("Could not open the raw-WAV safety copy at \"%1\": %2")
                          .arg(QFileInfo(safety_path).fileName(),
                               QString::fromUtf8(why.data(), static_cast<qsizetype>(why.size()))));
            emit encodeRefused(status_);
            return nullptr;
        }
        writers->wav_safety = std::move(safety);
    }
    return writers;
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
        emit encodeRefused(status_);
        return;
    }
    const auto device = devices_[static_cast<std::size_t>(captureDeviceIndex)];
    if (!to_sample_rate(device.sample_rate)) {
        setStatus(QStringLiteral("\"%1\" runs at %2 Hz; AC-3/E-AC-3 need 32, 44.1 or 48 kHz.")
                      .arg(QString::fromStdString(device.name))
                      .arg(device.sample_rate));
        emit encodeRefused(status_);
        return;
    }

    auto p = currentPlan();
    p.sample_rate = *to_sample_rate(device.sample_rate);
    if (const auto bad = plan::validate(p)) {
        setStatus(to_qstring(plan::describe(*bad)));
        emit encodeRefused(status_);
        return;
    }

    const bool want_passthrough = receiverDeviceIndex >= 0;
    ac3::sinks::RenderDeviceInfo receiver{};
    if (want_passthrough) {
        if (static_cast<std::size_t>(receiverDeviceIndex) >= outputs_.size()) {
            setStatus(QStringLiteral("Choose a receiver device first."));
            emit encodeRefused(status_);
            return;
        }
        receiver = outputs_[static_cast<std::size_t>(receiverDeviceIndex)];
    }

    QString path;
    if (writeToDisk) {
        path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
        if (path.isEmpty()) {
            setStatus(QStringLiteral("Choose where to save the take first."));
            emit encodeRefused(status_);
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
        emit encodeRefused(status_);
        return;
    }

    // The slave, when the rail selected a second device (captureDeviceRows'
    // row 1) alongside this one - opened here so a bad slave is refused the
    // same low-ceremony way a bad monitor open already is: non-fatally. A
    // two-device session that can't get its second device still starts as
    // an ordinary single-device one rather than failing outright, since the
    // master alone is a perfectly good session.
    std::optional<ac3::capture::DeviceInfo> device2;
    if (live_selected_devices_.size() > 1) {
        const int slave_index = live_selected_devices_[1];
        if (slave_index >= 0 && static_cast<std::size_t>(slave_index) < devices_.size() &&
            slave_index != captureDeviceIndex) {
            const auto candidate = devices_[static_cast<std::size_t>(slave_index)];
            if (to_sample_rate(candidate.sample_rate)) {
                live_capture2_ = std::make_unique<ac3::capture::Capture>();
                if (live_capture2_->start(candidate.id, candidate.kind)) {
                    device2 = candidate;
                } else {
                    live_capture2_.reset();
                }
            }
        }
    }
    live_second_device_active_ = device2.has_value();
    live_second_device_name_ =
        device2 ? QString::fromStdString(device2->name) : QString();

    // Opened next, before anything else is marked live - a bad destination
    // is refused exactly like a bad device choice above rather than
    // surfacing as a failure minutes into a take. After capture (not
    // before): a device failure needs no cleanup this way, since nothing
    // writer-side has been created yet for it to leave behind.
    auto writers = openLiveOutputWriters(path, writeToDisk, device);
    if (writeToDisk && !writers) {
        if (live_capture2_) {
            live_capture2_->stop();
            live_capture2_.reset();
        }
        live_capture_->stop();
        live_capture_.reset();
        return;  // openLiveOutputWriters already set the status and refused
    }

    // §8.3.2.2's decoder-side object gate means nobody downstream ever hears
    // our objects AS objects (see docs/design's own note on this - the real
    // decoder's gate is keyed, and forging that key is deliberately not
    // done), so an Atmos session's receiver leg is always just its 5.1 bed,
    // independent of what the device itself can bitstream.
    const bool eac3 = atmos_enabled_ || p.codec == plan::Codec::kEac3;
    bool passthrough_ok = false;
    live_downmix_leg_ = false;
    if (want_passthrough) {
        live_downmix_leg_ = wants_downmix_leg(eac3, receiver);
        auto opened = open_live_passthrough(receiver, eac3 && !live_downmix_leg_, atmos_enabled_,
                                            live_downmix_leg_, device.sample_rate,
                                            channelShapeName());
        passthrough_ok = opened.ok;
        live_receiver_plan_text_ = opened.plan_text;
        if (passthrough_ok) {
            live_passthrough_sink_ = std::move(opened.sink);
        } else {
            live_downmix_leg_ = false;
        }
    } else {
        live_receiver_plan_text_ = QStringLiteral("No passthrough this session.");
    }
    // The GAP banner is for a receiver leg that carries LESS than the encode
    // - the object case (the leg is always just the 5.1 bed) and the new
    // downmix-leg case (a wide channel layout's leg is capped to 5.1) both
    // qualify. A passthrough that failed to open outright is a different
    // story with its own banner (liveWantedPassthrough && !livePassthrough):
    // "everything past what the leg carries" would be a lie when the leg
    // carries nothing.
    live_gap_ = want_passthrough && passthrough_ok && (atmos_enabled_ || live_downmix_leg_);
    live_wanted_passthrough_ = want_passthrough;
    live_receiver_name_ =
        want_passthrough ? QString::fromStdString(receiver.name) : QString();
    live_receiver_eac3_ = want_passthrough && receiver.supports_eac3_passthrough;
    live_capture_detail_ = QStringLiteral("%1 ch · %2 Hz")
                               .arg(device.channels)
                               .arg(group_digits(device.sample_rate));

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

    // The flat capture-channel space object slots/assignment address: the
    // master's own channels, then the slave's appended after - devices are
    // sources, the same (source, channel) identity bundle A gave loaded
    // files (see keyForObjectIndex's own comment on the live sentinel).
    const std::uint16_t combined_channels =
        static_cast<std::uint16_t>(device.channels + (device2 ? device2->channels : 0));
    live_device_channels_ = static_cast<int>(combined_channels);
    if (atmos_enabled_) {
        // A live session has no loaded file to size object_configs_ from -
        // loadSourceFile is what normally does that. Unlike a file (whose
        // object count IS its channel count), a live session pre-allocates
        // a fixed BUDGET of slots - max(8, combined device channels),
        // capped at the TS 103 420 fifteen-object ceiling - rather than
        // exactly the device's own channel count: the JOC object count is
        // baked into the AtmosEncoder at construction (see runLiveSession)
        // and cannot change mid-stream, so "add an object" needs slots
        // already sitting there, allocated but unbound, for addLiveObject
        // to bind into. A two-channel device still gets eight slots to
        // grow into; a device (or a master+slave pair) with more than eight
        // combined channels simply starts with all of them bound (see the
        // identity binding built below).
        const int nobjects =
            std::clamp(std::max<int>(8, static_cast<int>(combined_channels)), 8, 15);
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
                                                   .path_labels = object_path_labels_,
                                                   .selected_index = selected_object_index_};
            object_count_ = nobjects;
            refreshObjectConfigs();
            emit sourceChanged();
        }
        // The starting binding: slot i <- capture channel i, for as many
        // slots as the device actually has channels for; anything beyond
        // that (the "budget" over "device channels") starts unbound and
        // silent, exactly what "Add an object" is for.
        {
            std::vector<int> initial(static_cast<std::size_t>(object_count_), -1);
            for (std::size_t i = 0; i < initial.size() && i < combined_channels; ++i) {
                initial[i] = static_cast<int>(i);
            }
            std::lock_guard lock(live_object_mutex_);
            live_slot_channels_ = std::move(initial);
        }
    }

    {
        // The live worker reads this positionally, by device-channel index -
        // flattened out of the (possibly sparse, identity-keyed)
        // object_configs_ via keyForObjectIndex, the same translation every
        // other object accessor uses.
        std::vector<ObjectConfig> snapshot;
        snapshot.reserve(static_cast<std::size_t>(object_count_));
        for (int i = 0; i < object_count_; ++i) {
            const auto key = keyForObjectIndex(i);
            snapshot.push_back(key ? map_value(object_configs_, *key) : ObjectConfig{});
        }
        std::lock_guard lock(live_object_mutex_);
        live_object_snapshot_ = std::move(snapshot);
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
    live_latency_measured_ = false;
    setBusy(true);
    // A real session - a take on disk or a receiver leg - lands in the run
    // history like any other encode: it is where a mid-session failure gets
    // its chip and banner, and where a finished take's "Show in folder"
    // lives. A monitor-only check deliberately does not (the rail's Monitor,
    // auto-started by merely picking a device, would spam the history with
    // entries nobody asked to keep).
    if (writeToDisk || want_passthrough) {
        startRun(path, QStringLiteral("live"),
                 writeToDisk ? QString() : QStringLiteral("live session"),
                 /*forceCbr=*/true);
    }
    setStatus(QStringLiteral("Live session running from %1…")
                  .arg(QString::fromStdString(device.name)));
    emit liveActiveChanged();
    emit liveStatsChanged();

    if (passthrough_ok) {
        // A freshly opened exclusive-mode stream is exactly when a physical
        // receiver drops lock to re-negotiate - the mockup's own copy quotes
        // "expect a second of silence" and runs its banner ~2.2 s, so the
        // pulse clears on the same timescale.
        live_reconnecting_ = true;
        emit liveReconnectingChanged();
        QTimer::singleShot(2200, this, [this] {
            live_reconnecting_ = false;
            emit liveReconnectingChanged();
        });
    }

    runLiveSession(device, device2, monitor_ok, passthrough_ok, writeToDisk, path,
                   std::move(writers));
}

void EncoderController::runLiveSession(ac3::capture::DeviceInfo device,
                                       std::optional<ac3::capture::DeviceInfo> device2,
                                       bool monitor, bool passthrough, bool write_to_disk,
                                       QString file_path,
                                       std::unique_ptr<LiveOutputWriters> writers) {
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
    const bool downmix_leg = live_downmix_leg_;
    const std::uint32_t downmix_bitrate_kbps = ac3::clamp_to_legal_ac3_bitrate(p.bitrate_kbps);

    // The master alone routes into the coded bed - see runLiveSession's own
    // design note (docs/gui/live-session.md): route()'s panning model treats
    // a source's channel COUNT as a specific named WAV layout, which has no
    // sound meaning for two independent devices concatenated together, so a
    // plain channel-mode session's bed continues to come from the master
    // device exactly as a single-device session always has. The slave's
    // captured, drift-corrected audio still rides the combined flat channel
    // space object-mode slots address (see combined_channels above), is
    // watched by its own SilenceWatchdog and is reflected in the drift
    // readout - captured and honestly accounted for either way, just not
    // auto-panned into a bed position with no principled default.
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
    clearClipLatches();

    // The allocated slot BUDGET (see startLiveSession's own comment on why
    // this is no longer just device.channels), not recomputed here - it has
    // to match exactly what startLiveSession sized object_configs_/
    // live_slot_channels_ to, and object_count_ is that single source of
    // truth for the whole session's lifetime.
    const std::size_t nobjects = atmos ? static_cast<std::size_t>(object_count_) : 0;
    const std::size_t channels = device.channels;
    const std::size_t channels2 = device2 ? device2->channels : 0;
    const std::size_t combined_channels = channels + channels2;
    const std::uint32_t sample_rate = device.sample_rate;
    const std::uint32_t sample_rate2 = device2 ? device2->sample_rate : 0;
    // Snapshotted once, for switchLiveReceiver's hot-swap path to reuse
    // verbatim later on the worker thread - the layout stays fixed for a
    // live session except via switchLiveLayout, which stops and restarts
    // the whole session rather than hot-swapping, so this never goes stale
    // mid-session.
    const QString shape_name = channelShapeName();

    std::ignore = QtConcurrent::run([this, p, atmos, eac3, downmix_leg, downmix_bitrate_kbps,
                                     cp = std::move(cp), routing = std::move(routing), nobjects,
                                     channels, channels2, combined_channels, sample_rate,
                                     sample_rate2, monitor, passthrough, write_to_disk, file_path,
                                     shape_name, device_name = QString::fromStdString(device.name),
                                     device2_name =
                                         device2 ? QString::fromStdString(device2->name)
                                                : QString(),
                                     has_device2 = device2.has_value(),
                                     writers = std::move(writers)]() mutable {
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
        // The parallel receiver leg: an independent AC-3 5.1 encoder fed the
        // main plan's already-computed bed channels (chan_views[0..5] for a
        // channel session, atmos_encoder->bed() for an object one - both are
        // ALREADY a self-sufficient §7.8 fold-down of the whole programme,
        // see plan::route's own "the bed stays a self-sufficient rendering"
        // guarantee, so there is no separate fold to compute here). Built
        // unconditionally, same as ac3_encoder/eac3_encoder above, since
        // downmix_leg can turn on mid-session via switchLiveReceiver's
        // hot-swap and this has to already exist when it does.
        auto downmix_encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
            .sample_rate = p.sample_rate,
            .bitrate_kbps = downmix_bitrate_kbps,
            .dialnorm = p.meta.dialnorm,
            .acmod = ac3::Acmod::k3_2,
            .lfe = true,
            .cmixlev = p.meta.cmixlev,
            .surmixlev = p.meta.surmixlev});
        bool leg_active = downmix_leg;

        ac3::analysis::LevelMeter meter =
            atmos ? ac3::analysis::LevelMeter{ac3::Acmod::k3_2, true, sample_rate}
                  : ac3::analysis::LevelMeter{cp->bed_acmod, cp->bed_lfe, sample_rate,
                                             routing->coded_channels};

        std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                                       channels);
        // ---- slave device: drain -> resample -> lockstep with the master --
        // A separate per-iteration buffer rather than one physically widened
        // `interleaved` - the resampler already produces exactly one frame's
        // worth each iteration, so there is nothing to gain from copying it
        // into a combined buffer, and every de-interleave site below reads
        // the master's own channels from `interleaved` and the slave's from
        // this one, addressed as one logical combined_channels space (see
        // the atmos per-slot de-interleave further down).
        std::vector<float> slave_resampled(static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                                           channels2);
        std::optional<ac3::capture::DriftResampler> slave_resampler;
        std::optional<ac3::capture::ClockDriftEstimator> slave_drift;
        // Generous headroom (8 frame periods) so a burst of slave jitter
        // never starves the resampler mid-frame; the servo steers actual
        // occupancy back towards one frame period's worth (kSamplesPerFrame)
        // on its own.
        std::vector<float> slave_scratch;
        std::size_t slave_scratch_valid_frames = 0;
        ac3::capture::SilenceWatchdog slave_watchdog(kDeviceSilenceTimeout);
        if (has_device2) {
            const double nominal_ratio =
                static_cast<double>(sample_rate) / static_cast<double>(sample_rate2);
            slave_resampler.emplace(channels2);
            slave_resampler->reset();
            slave_drift.emplace(nominal_ratio, static_cast<std::size_t>(ac3::kSamplesPerFrame));
            slave_scratch.assign(
                8 * static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels2, 0.0f);
        }

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

        std::uint64_t n0 = 0;
        std::uint64_t frames_written = 0;
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;
        auto last_disk_flush_at = std::chrono::steady_clock::now();
        ac3::capture::SilenceWatchdog watchdog(kDeviceSilenceTimeout);
        watchdog.reset(std::chrono::steady_clock::now());
        if (has_device2) {
            slave_watchdog.reset(std::chrono::steady_clock::now());
        }
        bool device_lost = false;
        // Which device the loop's one `device_lost` flag refers to - master
        // (false, the default - matches every session before a slave could
        // exist) or slave (true) - so the failure text below names the
        // actual device that went quiet rather than always blaming the
        // master.
        bool lost_is_slave = false;
        // The one-shot capture->monitor latency measurement: only attempted
        // once monitoring is on and the pipeline has run for about a second
        // (past whatever startup transient the first few frames carry), and
        // only until it succeeds once - see the measuring_latency check
        // below and its use around the monitor submit() call.
        bool latency_measured = false;
        std::chrono::steady_clock::time_point capture_done_at{};

        while (!stop_live_.load(std::memory_order_relaxed)) {
            // switchLiveReceiver's handoff: claimed and cleared here, once
            // per frame period, on the only thread that ever calls submit()
            // on live_passthrough_sink_ - performing the close-old/open-new
            // itself right here (rather than on the GUI thread) is what
            // makes the swap safe without holding a lock across it: there is
            // never a window where another thread could be mid-submit on a
            // sink this is about to destroy, because nothing else ever
            // submits to it at all.
            std::optional<PendingReceiverSwitch> switch_request;
            {
                std::lock_guard lock(live_receiver_switch_mutex_);
                switch_request = std::move(live_receiver_switch_request_);
                live_receiver_switch_request_.reset();
            }
            if (switch_request) {
                if (live_passthrough_sink_) {
                    live_passthrough_sink_->stop();
                    live_passthrough_sink_.reset();
                }
                bool new_ok = false;
                QString plan_text;
                QString receiver_name;
                bool receiver_eac3 = false;
                bool new_leg_active = false;
                if (switch_request->want_passthrough) {
                    new_leg_active = wants_downmix_leg(eac3, switch_request->receiver);
                    auto opened = open_live_passthrough(switch_request->receiver,
                                                        eac3 && !new_leg_active, atmos,
                                                        new_leg_active, sample_rate, shape_name);
                    plan_text = opened.plan_text;
                    if (opened.ok) {
                        new_ok = true;
                        live_passthrough_sink_ = std::move(opened.sink);
                        receiver_name = QString::fromStdString(switch_request->receiver.name);
                        receiver_eac3 = switch_request->receiver.supports_eac3_passthrough;
                    } else {
                        new_leg_active = false;
                    }
                } else {
                    plan_text = QStringLiteral("No passthrough this session.");
                }
                passthrough = new_ok;
                leg_active = new_leg_active;
                const bool wanted = switch_request->want_passthrough;
                QMetaObject::invokeMethod(
                    this,
                    [this, new_ok, plan_text, receiver_name, receiver_eac3, wanted, atmos,
                    new_leg_active] {
                        live_passthrough_ = new_ok;
                        live_wanted_passthrough_ = wanted;
                        live_receiver_name_ = receiver_name;
                        live_receiver_eac3_ = receiver_eac3;
                        live_receiver_plan_text_ = plan_text;
                        live_downmix_leg_ = new_leg_active;
                        live_gap_ = wanted && new_ok && (atmos || new_leg_active);
                        emit liveActiveChanged();
                        if (new_ok) {
                            // Same "expect a second of silence" pulse a
                            // fresh session's own first open already shows -
                            // a hot-swap is a real exclusive-mode re-open too.
                            live_reconnecting_ = true;
                            emit liveReconnectingChanged();
                            QTimer::singleShot(2200, this, [this] {
                                live_reconnecting_ = false;
                                emit liveReconnectingChanged();
                            });
                        }
                    });
            }

            std::size_t filled = 0;
            while (filled < interleaved.size() &&
                   !stop_live_.load(std::memory_order_relaxed)) {
                const auto got = live_capture_->buffer()->read(
                    std::span{interleaved}.subspan(filled, interleaved.size() - filled));
                filled += got;
                const auto read_at = std::chrono::steady_clock::now();
                watchdog.on_read(got, read_at);
                if (got == 0) {
                    if (watchdog.timed_out(read_at)) {
                        device_lost = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (device_lost) {
                break;
            }
            if (filled < interleaved.size()) {
                break;  // stopped mid-frame; drop the partial frame
            }

            // The slave: an opportunistic, NON-blocking drain of whatever
            // its ring buffer holds right now (the master's own blocking
            // fill loop just above already gave it roughly one frame
            // period's worth of wall-clock time to deliver into), resampled
            // to the master's clock. See docs/gui/live-session.md for the
            // servo/resampler design; ClockDriftEstimator/DriftResampler are
            // the shared library pieces ac3cli's own `live capture2=` uses.
            if (has_device2) {
                const std::size_t capacity_frames = slave_scratch.size() / channels2;
                if (slave_scratch_valid_frames < capacity_frames) {
                    const auto got = live_capture2_->buffer()->read(std::span{slave_scratch}.subspan(
                        slave_scratch_valid_frames * channels2,
                        (capacity_frames - slave_scratch_valid_frames) * channels2));
                    const auto read_at = std::chrono::steady_clock::now();
                    slave_watchdog.on_read(got, read_at);
                    slave_scratch_valid_frames += got / channels2;
                    if (got == 0 && slave_watchdog.timed_out(read_at)) {
                        device_lost = true;
                        lost_is_slave = true;
                        break;
                    }
                }
                slave_drift->update(slave_scratch_valid_frames);
                slave_resampler->set_ratio(slave_drift->ratio());
                const auto consumed = slave_resampler->render(
                    std::span<const float>{slave_scratch}.first(slave_scratch_valid_frames *
                                                                 channels2),
                    slave_scratch_valid_frames, slave_resampled,
                    static_cast<std::size_t>(ac3::kSamplesPerFrame));
                const std::size_t remaining_frames = slave_scratch_valid_frames - consumed;
                if (remaining_frames > 0 && consumed > 0) {
                    std::copy(slave_scratch.begin() + static_cast<std::ptrdiff_t>(consumed * channels2),
                             slave_scratch.begin() + static_cast<std::ptrdiff_t>(
                                                          slave_scratch_valid_frames * channels2),
                             slave_scratch.begin());
                }
                slave_scratch_valid_frames = remaining_frames;
            }

            const bool measuring_latency =
                monitor && !latency_measured && n0 >= static_cast<std::uint64_t>(sample_rate);
            if (measuring_latency) {
                capture_done_at = std::chrono::steady_clock::now();
            }
            n0 += static_cast<std::uint64_t>(ac3::kSamplesPerFrame);

            std::vector<std::byte> unit_bytes;
            if (atmos) {
                // Slot `ch` carries whichever capture channel
                // slot_channels[ch] names - -1 (unbound, or the slot count
                // simply not having grown that far via addLiveObject yet)
                // reads as silence, exactly like a bed position past the
                // device's own channel count already did before per-slot
                // binding existed.
                // A slot's bound channel addresses the COMBINED space: 0..
                // channels-1 is the master (interleaved), channels..
                // combined_channels-1 is the slave (slave_resampled, index
                // shifted back down by `channels`) - devices are sources,
                // concatenated after one another the same way bundle A
                // concatenates a second loaded file's channels after the
                // first's.
                const auto slot_channels = liveSlotChannels();
                for (std::size_t ch = 0; ch < nobjects; ++ch) {
                    const int bound = ch < slot_channels.size() ? slot_channels[ch] : -1;
                    const bool silent =
                        bound < 0 || static_cast<std::size_t>(bound) >= combined_channels;
                    const bool from_slave = !silent && static_cast<std::size_t>(bound) >= channels;
                    const std::size_t local =
                        silent ? 0
                              : (from_slave ? static_cast<std::size_t>(bound) - channels
                                            : static_cast<std::size_t>(bound));
                    const std::size_t src_channels = from_slave ? channels2 : channels;
                    for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                        const std::size_t base = static_cast<std::size_t>(i) * src_channels;
                        object_block[ch][static_cast<std::size_t>(i)] =
                            silent ? 0.0f
                                  : (from_slave ? slave_resampled[base + local]
                                                : interleaved[base + local]);
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
                bool submitted = false;
                if (to_play) {
                    while (!(submitted = live_monitor_sink_->submit(*to_play)) &&
                          !stop_live_.load(std::memory_order_relaxed)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                }
                // The real capture->monitor round trip: from this frame's
                // capture buffer finishing (stamped above, before the encode
                // even ran) to its decoded audio actually landing in the
                // sink's queue. Neither sink reports its own buffered depth
                // today, so this is the whole measurable span rather than
                // that span plus an unavailable extra - see liveLatencyMs'
                // own doc comment. A frame the decoder held back
                // (to_play empty, §3.7) or lost the submit race to a stop
                // just leaves latency_measured false for the next
                // measuring_latency frame to try again.
                if (measuring_latency && submitted) {
                    const auto submit_at = std::chrono::steady_clock::now();
                    const auto measured_ms =
                        std::chrono::duration<double, std::milli>(submit_at - capture_done_at)
                            .count();
                    latency_measured = true;
                    QMetaObject::invokeMethod(this, [this, measured_ms] {
                        live_latency_ms_ = measured_ms;
                        live_latency_measured_ = true;
                        emit liveStatsChanged();
                    });
                }
            }

            if (passthrough) {
                std::optional<std::vector<std::byte>> burst;
                if (leg_active) {
                    // The capped receiver leg: an independent AC-3 5.1
                    // encode of the main plan's ALREADY-COMPUTED bed - see
                    // downmix_encoder's own construction comment for why
                    // this needs no separate §7.8 fold. The main encode
                    // above (unit_bytes) is untouched and still reaches
                    // meters/monitor/disk exactly as it always has.
                    const auto& bed_source = atmos ? bed_views : std::span{chan_views}.first(6);
                    const auto leg_frame = downmix_encoder->encode_frame(bed_source);
                    if (leg_frame) {
                        const auto wrapped = ac3::iec61937::wrap_frame(*leg_frame);
                        if (wrapped) {
                            burst = *wrapped;
                        }
                    }
                } else if (eac3) {
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

            if (write_to_disk && writers) {
                writers->stream.write(reinterpret_cast<const char*>(unit_bytes.data()),
                                      static_cast<std::streamsize>(unit_bytes.size()));
                if (writers->matroska) {
                    // The lightweight index the clean-stop mux step splits
                    // the spool back into frames with - a few bytes per
                    // unit, not the audio itself, which is exactly what
                    // keeps this incremental instead of the old
                    // frames.push_back(unit_bytes) that held the WHOLE take
                    // in RAM for as long as the session ran.
                    writers->frame_sizes.push_back(static_cast<std::uint32_t>(unit_bytes.size()));
                }
                ++frames_written;
                if (writers->wav_safety) {
                    // The raw captured PCM, in the device's own channel
                    // order, before any routing or encoding - a safety net
                    // for the SOURCE audio, independent of channel/object
                    // mode.
                    std::ignore = writers->wav_safety->write(interleaved);
                }
                const auto flush_at = std::chrono::steady_clock::now();
                if (flush_at - last_disk_flush_at >= kDiskFlushInterval) {
                    last_disk_flush_at = flush_at;
                    writers->stream.flush();
                    if (writers->wav_safety) {
                        writers->wav_safety->flush_header();
                    }
                }
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
                const double drift_ppm = has_device2 ? slave_drift->drift_ppm() : 0.0;
                QMetaObject::invokeMethod(
                    this, [this, seconds, dropped, underruns, encoded, drift_ppm,
                          snapshot = std::move(snapshot)] {
                        live_running_seconds_ = seconds;
                        live_frames_encoded_ = encoded;
                        live_frames_dropped_ = static_cast<qint64>(dropped);
                        live_underruns_ = underruns;
                        live_drift_ppm_ = drift_ppm;
                        emit liveStatsChanged();
                        publishLevels(snapshot);
                    });
            }
        }

        // The failure story, in priority order: a device that stopped
        // delivering audio is the interesting cause even if the disk side
        // finishes cleanly; a disk-side problem (only reachable for
        // Matroska - an elementary-stream take is already fully written by
        // the time the loop above exits) only replaces it when there was
        // nothing more specific to say.
        QString problem;
        if (device_lost) {
            problem = QStringLiteral(
                          "\"%1\" stopped delivering audio - the capture device may have been "
                          "disconnected. Wrote %2 frames before it went quiet.")
                          .arg(lost_is_slave ? device2_name : device_name)
                          .arg(frames_written);
        }
        if (write_to_disk && writers) {
            writers->stream.flush();
            writers->stream.close();
            if (writers->wav_safety) {
                writers->wav_safety->close();
            }
            if (writers->matroska) {
                // matroska::mux() only ever produces a complete file from
                // the WHOLE set of frames, so the spool this session has
                // been growing incrementally is read back and muxed HERE,
                // once, at a clean stop - not per frame, which is the whole
                // point of spooling instead of holding every frame in RAM
                // for the run's whole duration (see LiveOutputWriters' own
                // comment). A device-lost or otherwise interrupted session
                // still reaches this (the loop's every `break` falls
                // through to here), so the spool is folded into the .mkv
                // whenever there is anything to fold; only an outright
                // process crash leaves it behind unmuxed.
                std::ifstream spool_in{writers->stream_path.toStdString(), std::ios::binary};
                std::vector<std::vector<std::byte>> mux_frames;
                mux_frames.reserve(writers->frame_sizes.size());
                bool read_ok = static_cast<bool>(spool_in);
                for (const auto size : writers->frame_sizes) {
                    std::vector<std::byte> frame(size);
                    spool_in.read(reinterpret_cast<char*>(frame.data()),
                                 static_cast<std::streamsize>(size));
                    if (!spool_in) {
                        read_ok = false;
                        break;
                    }
                    mux_frames.push_back(std::move(frame));
                }
                spool_in.close();
                QString mux_problem = read_ok
                    ? writeOutput(writers->final_path, mux_frames, sample_rate,
                                 static_cast<int>(coded_count))
                    : QStringLiteral("Could not read back the elementary stream to mux it.");
                if (!mux_problem.isEmpty()) {
                    mux_problem += QStringLiteral(" The elementary stream is kept at \"%1\".")
                                       .arg(QFileInfo(writers->stream_path).fileName());
                    if (problem.isEmpty()) {
                        problem = mux_problem;
                    }
                } else {
                    QFile::remove(writers->stream_path);
                }
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

        QMetaObject::invokeMethod(this, [this, frames_written, problem, write_to_disk,
                                         totals = std::move(totals)] {
            const auto capture_stats = live_capture_->stats();
            live_capture_->stop();
            live_capture_.reset();
            if (live_capture2_) {
                live_capture2_->stop();
                live_capture2_.reset();
            }
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
            live_second_device_active_ = false;
            live_second_device_name_.clear();
            live_drift_ppm_ = 0.0;
            live_downmix_leg_ = false;
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
                object_path_labels_ = std::move(live_object_backup_->path_labels);
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
                              .arg(frames_written)
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
        emit encodeRefused(status_);
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
        emit encodeRefused(status_);
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
        emit encodeRefused(status_);
        return;
    }
    if (const auto bad = plan::validate(p)) {
        setStatus(to_qstring(plan::describe(*bad)));
        emit encodeRefused(status_);
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
        emit encodeRefused(status_);
        return;
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    output_eac3_ = p.codec == plan::Codec::kEac3;
    emit outputChanged();

    stop_recording_.store(false, std::memory_order_relaxed);
    setRecording(true);
    setBusy(true);
    // A recording is an encode with a history like any other - the mockup's
    // canonical failure is a device dying mid-take, and a failure with no
    // run entry has no chip and no banner to land on. Length is unknowable
    // at start, so the chip says what it honestly is.
    startRun(path, QStringLiteral("live"));
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
    clearClipLatches();
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
        // ObjectKey's (source, channel) identities are only meaningful
        // relative to the source list they were authored against - a fresh
        // (and here, entirely empty) one starts with nothing to inherit.
        object_configs_.clear();
        object_keyframes_.clear();
        object_path_labels_.clear();
        selected_object_index_ = 0;
        source_offset_seconds_ = 0.0;
        extra_source_offsets_seconds_.clear();
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
    // fresh source list, even when the read itself succeeds. Object state is
    // explicitly cleared too (rather than left to refreshObjectConfigs'
    // resize below): it is now keyed by (source, channel) identity, which a
    // same- or larger-sized new file would otherwise silently inherit from
    // whatever the old one authored at the same channel numbers - a
    // different bug than the one this identity keying fixes, but the same
    // class of it.
    extra_sources_.clear();
    assignment_ = plan::Assignment{};
    touched_channels_.clear();
    has_explicit_assignment_ = false;
    object_configs_.clear();
    object_keyframes_.clear();
    object_path_labels_.clear();
    selected_object_index_ = 0;
    source_offset_seconds_ = 0.0;
    extra_source_offsets_seconds_.clear();
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
    auto khz_label = [](std::uint32_t hz) {
        QString s = QString::number(static_cast<double>(hz) / 1000.0, 'f', 1);
        if (s.endsWith(QStringLiteral(".0"))) {
            s.chop(2);
        }
        return s;
    };
    auto addRow = [&](const QString& path, const ac3::io::WavData& wav, bool primary,
                      double offset_seconds, std::optional<std::uint32_t> resampled_from) {
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
        row[QStringLiteral("offsetSeconds")] = offset_seconds;
        // "44.1→48 k" when addSourceFile had to resample this source onto
        // the primary's rate, empty otherwise - see addSourceFile's own
        // comment.
        row[QStringLiteral("resampleLabel")] =
            resampled_from ? QStringLiteral("%1→%2 k")
                                 .arg(khz_label(*resampled_from), khz_label(wav.sample_rate))
                           : QString();
        out.append(row);
    };
    addRow(source_->path, source_->wav, true, source_offset_seconds_, std::nullopt);
    for (std::size_t i = 0; i < extra_sources_.size(); ++i) {
        addRow(extra_sources_[i]->path, extra_sources_[i]->wav, false,
              i < extra_source_offsets_seconds_.size() ? extra_source_offsets_seconds_[i] : 0.0,
              i < extra_source_original_rates_.size() ? extra_source_original_rates_[i]
                                                       : std::nullopt);
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
            // The row's own trim control reads its starting value from
            // here - see setAssignmentTrim.
            row[QStringLiteral("trimDb")] = assignment_.at(s, c).trim_db;
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
    std::optional<std::uint32_t> original_rate;
    if (wav->sample_rate != source_->wav.sample_rate) {
        // A mismatch is resampled to the PRIMARY's rate rather than refused
        // outright: plan::render itself still has no notion of resampling,
        // but there is no reason to make the USER fix that by hand first
        // when ac3::dsp::resample_planar() (a proper offline windowed-sinc
        // conversion - see its own header comment on why it can afford a
        // better kernel than the live capture path's drift resampler) can
        // do it once, here, before anything reaches render(). The refusal
        // survives only when the PRIMARY's own rate has no legal AC-3
        // target at all (to_sample_rate_for_file) - resampling TO an
        // illegal rate would just move the problem, not solve it, the same
        // edge case ac3cli's own load_sources() would still refuse too.
        if (!to_sample_rate_for_file(source_->wav.sample_rate, codec_)) {
            setStatus(QStringLiteral("%1 is %2 Hz, but the loaded source's %3 Hz is not a rate "
                                     "AC-3 can encode at all — load a source at a legal rate "
                                     "first.")
                          .arg(QFileInfo(path).fileName())
                          .arg(wav->sample_rate)
                          .arg(source_->wav.sample_rate));
            return;
        }
        original_rate = wav->sample_rate;
        wav->channels =
            ac3::dsp::resample_planar(wav->channels, *original_rate, source_->wav.sample_rate);
        wav->sample_rate = source_->wav.sample_rate;
    }
    extra_sources_.push_back(std::make_shared<Source>(Source{std::move(*wav), path}));
    extra_source_offsets_seconds_.push_back(0.0);
    extra_source_original_rates_.push_back(original_rate);
    refreshAfterSourceListChange();
}

void EncoderController::setSourceOffset(int index, double seconds) {
    if (index < 0) {
        return;
    }
    const double clamped = std::max(0.0, seconds);
    if (index == 0) {
        if (!source_) {
            return;
        }
        source_offset_seconds_ = clamped;
    } else {
        const auto extra_index = static_cast<std::size_t>(index - 1);
        if (extra_index >= extra_sources_.size()) {
            return;
        }
        if (extra_index >= extra_source_offsets_seconds_.size()) {
            extra_source_offsets_seconds_.resize(extra_index + 1, 0.0);
        }
        extra_source_offsets_seconds_[extra_index] = clamped;
    }
    emit sourceChanged();
    // The derived programme length and the meter preview both read this -
    // refreshRouting ends in previewPlanMeters, the same "an edit re-renders
    // the preview" path every other source/assignment change already takes.
    refreshRouting();
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
        object_configs_.clear();
        object_keyframes_.clear();
        object_path_labels_.clear();
        selected_object_index_ = 0;
        source_offset_seconds_ = 0.0;
        extra_source_offsets_seconds_.clear();
        extra_source_original_rates_.clear();
        refreshObjectConfigs();
        clearLayout();
        resetSourceLevels();
        emit sourceChanged();
        setStatus(QStringLiteral("Choose a WAV file, or record from a capture device."));
        return;
    }
    const auto extra_index = static_cast<std::size_t>(index - 1);
    if (!source_ || extra_index >= extra_sources_.size()) {
        return;
    }
    extra_sources_.erase(extra_sources_.begin() + static_cast<std::ptrdiff_t>(extra_index));
    if (extra_index < extra_source_offsets_seconds_.size()) {
        extra_source_offsets_seconds_.erase(extra_source_offsets_seconds_.begin() +
                                            static_cast<std::ptrdiff_t>(extra_index));
    }
    if (extra_index < extra_source_original_rates_.size()) {
        extra_source_original_rates_.erase(extra_source_original_rates_.begin() +
                                           static_cast<std::ptrdiff_t>(extra_index));
    }
    // The removed source's rows addressed positions by index, and every
    // later source's index just shifted down by one - there is no honest
    // way to guess which of its old rows survive at the new numbering, so
    // this clears and asks for the assignment to be redone rather than risk
    // silently misrouting a channel. Automatic routing resumes on its own
    // once exactly one source is left (see routingForSources).
    assignment_ = plan::Assignment{};
    touched_channels_.clear();
    has_explicit_assignment_ = !extra_sources_.empty();
    // Object state is keyed by (source, channel) identity (ObjectKey), not
    // by position - unlike the assignment table above, it does NOT need a
    // blanket clear. `index` here is exactly the removed source's own index
    // in that same addressing (sourceShapes()/Assignment's), so this drops
    // only its own channels' objects/keyframes and renumbers every later
    // source's entries down by one, the identical shift sourceShapes()
    // itself now applies - a surviving source's authored motion is
    // untouched, and reappears in the object list the moment its channels
    // are (re)assigned to "an object" again.
    const auto removed_source = static_cast<std::size_t>(index);
    rekey_after_source_removed(object_configs_, removed_source);
    rekey_after_source_removed(object_keyframes_, removed_source);
    rekey_after_source_removed(object_path_labels_, removed_source);
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
    // holds. A channel's own authored motion follows it either way (see
    // ObjectKey): reassigning it away from "obj" and back does not lose it.
    recomputeObjectCount();
    emit objectsChanged();
    emit sourceChanged();
    refreshRouting();
}

void EncoderController::setAssignmentTrim(int sourceIndex, int channel, double dbTrim) {
    if (!source_ || sourceIndex < 0 || channel < 0) {
        return;
    }
    const auto s = static_cast<std::size_t>(sourceIndex);
    const auto c = static_cast<std::size_t>(channel);
    auto dest = assignment_.at(s, c);
    if (dest.kind == plan::DestinationKind::kUnassigned) {
        return;  // "none" has no destination for a trim to ride
    }
    // The identical clamp-then-snap-to-a-tenth-of-a-dB-grid formula
    // assignment.cpp's own (private) snap_trim applies to a parsed "@"
    // suffix - see setAssignmentTrim's own header comment on why the two
    // must compute the same double.
    const double clamped = std::clamp(dbTrim, -24.0, 24.0);
    dest.trim_db = static_cast<double>(std::llround(clamped * 10.0)) / 10.0;
    assignment_.set(s, c, dest);
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
        emit encodeRefused(status_);
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
        emit encodeRefused(status_);
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
            emit encodeRefused(status_);
            return;
        }
        if (ndynamic + npinned > 15) {
            setStatus(QStringLiteral("%1 objects and %2 bed-fed channels exceed the sixteen-"
                                     "object programme cap (the bed's LFE is one of them) — "
                                     "assign fewer channels.")
                          .arg(static_cast<int>(ndynamic))
                          .arg(static_cast<int>(npinned)));
            emit encodeRefused(status_);
            return;
        }
    }

    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    output_path_ = path;
    output_eac3_ = object_mode || codec_ == plan::Codec::kEac3;
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
    // Each source's own start offset, baked in as leading silence - see
    // apply_channel_offsets' own comment on why this is the one place it
    // needs to happen for the file-encode paths.
    planes = apply_channel_offsets(std::move(planes), flatChannelOffsetSamples(sample_rate));

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
    clearClipLatches();

    const bool eac3 = p.codec == plan::Codec::kEac3;
    const bool keep_partial = keep_partial_output_;
    // See previewPlanMeters' identical comment: only ever non-empty for an
    // assignment-based routing, never the automatic single-source overload.
    const auto lfe_indices =
        has_explicit_assignment_ ? lfe_coded_indices(coded) : std::vector<std::size_t>{};
    std::ignore = QtConcurrent::run([this, path, p, routing, cp, sample_rate,
                                     eac3, label, keep_partial, lfe_indices,
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
        std::vector<ac3::dsp::LfeLowpass> lfe_filters;
        lfe_filters.reserve(lfe_indices.size());
        for (std::size_t i = 0; i < lfe_indices.size(); ++i) {
            lfe_filters.emplace_back(kLfeLowpassCornerHz, sample_rate);
        }

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
            for (std::size_t i = 0; i < lfe_indices.size(); ++i) {
                lfe_filters[i].process(std::span<float>(block[lfe_indices[i]]));
            }
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
        // dynamic[i]'s IDENTITY channel (its first - see
        // dynamicObjectChannels' own comment) - the same key every other
        // object accessor resolves through keyForObjectIndex; encode is
        // always file-mode (never live, see encodeTo's busy_ gate), so this
        // can go straight to the identity without that helper's
        // live-session branch.
        const auto object_key = sourceChannelForFlatIndex(dynamic[i].front());
        const auto authored = object_keyframes_.find(object_key);
        if (authored != object_keyframes_.end() && !authored->second.empty()) {
            auto created = ac3::oba::KeyframePath::create(authored->second);
            if (created) {
                paths.emplace_back(std::move(*created));
                continue;
            }
        }
        const auto config = map_value(object_configs_, object_key);
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
    // first (object i = the i-th "obj"/"objm"-assigned group, the
    // objectModel/config addressing, each built via buildObjectPlane so a
    // trim or an objm fold reaches the stream), then the pinned channels.
    // Channels assigned nowhere are dropped here, which is what
    // "Unassigned - it will not be heard" means.
    std::vector<std::vector<float>> object_planes;
    object_planes.reserve(nobjects);
    for (std::size_t i = 0; i < ndynamic; ++i) {
        object_planes.push_back(buildObjectPlane(dynamic[i], planes));
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
    clearClipLatches();

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
