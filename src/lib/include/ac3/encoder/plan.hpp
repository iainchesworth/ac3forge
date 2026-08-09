#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"

// What to encode, and how - the decisions a front end collects from a user and
// hands to the encoders, in one place.
//
// This exists because there are two front ends. Every name here was previously
// spelled out inside ac3cli: the layout table, the Annex E tool token, the
// widening of AC-3's coarse downmix levels into E-AC-3's finer ones. A GUI that
// re-derived any of them would be free to disagree with the command line about
// what "5.1.4" means or which tools "all" turns on, and nothing would catch it.
// So the layouts, the tokens and the config construction live here, and the
// front ends only collect and display.
//
// Nothing in this file encodes anything. It builds EncoderConfig and
// AccessUnitConfig values and says how a source's channels reach them; the
// encoders are unchanged and unaware of it.

namespace ac3::plan {

// --- codec ------------------------------------------------------------------

enum class Codec : std::uint8_t {
    kAc3,   // bsid 8, A/52 §5
    kEac3,  // bsid 16, A/52 Annex E
};

[[nodiscard]] constexpr std::string_view codec_name(Codec codec) {
    return codec == Codec::kAc3 ? "ac3" : "eac3";
}

[[nodiscard]] constexpr std::string_view codec_label(Codec codec) {
    return codec == Codec::kAc3 ? "AC-3" : "E-AC-3";
}

// The file extension a bare elementary stream of this codec conventionally
// takes. Both front ends name output files, and they must agree.
[[nodiscard]] constexpr std::string_view codec_suffix(Codec codec) {
    return codec == Codec::kAc3 ? "ac3" : "ec3";
}

// --- layouts ----------------------------------------------------------------

// Speaker layouts, named by the token the command line takes and the GUI keys
// its combo box on.
enum class LayoutId : std::uint8_t {
    kMono,
    kStereo,
    kDualMono,
    k51,
    k71,
    k512,
    k514,
    k714,
};

struct LayoutInfo {
    LayoutId id;
    std::string_view name;   // command-line token, and the GUI's stable key
    std::string_view label;  // what a person reads
    // Channels a decoder RENDERS. Not the same as the channels the encoder is
    // fed: a dependent substream may replace channels the bed already carries.
    int rendered;
    // Spans encode_access_unit() expects - every coded channel of every
    // substream, which is what makes 7.1 cost ten coded channels for eight
    // speakers.
    int transmitted;
    // Dependent substreams required. Any at all forces E-AC-3: AC-3 has no
    // substream layer and no coding mode wider than 3/2 + LFE.
    int dependents;
};

inline constexpr std::array<LayoutInfo, 8> kLayouts{{
    {LayoutId::kMono, "mono", "1/0 mono", 1, 1, 0},
    {LayoutId::kStereo, "stereo", "2/0 stereo", 2, 2, 0},
    // Two independent programmes sharing one syncframe, not a soundfield -
    // "rendered"/"transmitted" both count 2 only because that is how many
    // coded channels 1+1 carries, not because there are two speakers to fill.
    {LayoutId::kDualMono, "1+1", "1+1 dual mono", 2, 2, 0},
    {LayoutId::k51, "51", "5.1", 6, 6, 0},
    // The dependent replaces the bed's surrounds and adds the rears, so four
    // coded channels buy two new speakers.
    {LayoutId::k71, "71", "7.1", 8, 10, 1},
    {LayoutId::k512, "512", "5.1.2", 8, 8, 1},
    {LayoutId::k514, "514", "5.1.4", 10, 10, 1},
    // Six new channels, one more than a single dependent can hold.
    {LayoutId::k714, "714", "7.1.4", 12, 14, 2},
}};

[[nodiscard]] constexpr const LayoutInfo& layout(LayoutId id) {
    return kLayouts[static_cast<std::size_t>(id)];
}

[[nodiscard]] std::optional<LayoutId> parse_layout(std::string_view name);

// The layout a source of this width most naturally is, for a front end that
// must pick one before being told. Widths with no layout of their own (3, 4
// and 5 channels) answer with the narrowest layout that holds them, which
// leaves the channels they lack silent rather than inventing any.
[[nodiscard]] std::optional<LayoutId> layout_for_source(std::size_t wav_channels);

// "mono | stereo | 51 | ...", built from kLayouts so a usage line and the
// parser that rejects a bad token cannot list different sets.
[[nodiscard]] std::string layout_names(Codec codec = Codec::kEac3);

// Whether this codec can carry this layout at all: AC-3 stops at 5.1.
[[nodiscard]] constexpr bool carries(Codec codec, LayoutId id) {
    return codec == Codec::kEac3 || layout(id).dependents == 0;
}

// --- the general channel model -----------------------------------------------
//
// A LayoutId only ever names one of the combinations below. This is the
// general form underneath: a bed acmod/lfe plus however many dependent
// chanmaps it takes to render an arbitrary set of Table E2.5 locations
// (ac3::eac3::chanmap::allocate does the actual partitioning). Every function
// below that used to take only a LayoutId now also takes a ChannelPlan
// directly, and the LayoutId overload is a one-line lookup into it - so a
// named layout is a convenience shortcut for a specific plan, not a separate
// system.
using ChannelPlan = eac3::chanmap::ChannelPlan;
using ChannelPlanError = eac3::chanmap::AllocationError;

// The plan a named layout has always built: its bed's acmod/lfe and its
// dependents' chanmaps, unchanged from what LayoutId's own hand-picked
// constants (k71Rear, kTopQuad, k512Height) already gave it.
[[nodiscard]] ChannelPlan channel_plan_for(LayoutId id);

// Table E2.5 location names, comma-separated ("L,C,R,LFE,Vhl,Vhr"), as an
// alternative to a named layout for whatever combination the format allows
// but no LayoutId names. A pair location (Lc/Rc, Lrs/Rrs, Lsd/Rsd, Lw/Rw,
// Vhl/Vhr, Lts/Rts) must name both members - Table E2.5 has no bit for one
// alone. Returns nullopt on an unrecognised name, an unpaired pair member, or
// an empty list.
[[nodiscard]] std::optional<std::uint16_t> parse_channels(std::string_view text);

// The inverse, in Table E2.5 bit order. Round-trips through parse_channels,
// the way format_tools/parse_tools already do for coding tools.
[[nodiscard]] std::string format_channels(std::uint16_t locations);

// One coded channel of one substream, in transmission order.
struct CodedChannel {
    eac3::chanmap::Location location;
    // True for the independent substream's own channels. That substream is a
    // self-sufficient rendering of the whole programme (§E1.3.1), so its
    // channels are fed a rendering of the BED layout even when a dependent
    // will later replace them.
    bool bed;
    // Which substream carries it: 0 is the independent one.
    int substream;
};

// Every coded channel of a plan, in the order encode_access_unit() wants
// them. For a named layout, size is layout(id).transmitted.
[[nodiscard]] std::vector<CodedChannel> coded_channels(const ChannelPlan& plan);
[[nodiscard]] std::vector<CodedChannel> coded_channels(LayoutId id);

// Names for those channels, for meters and reports. A bed channel a dependent
// replaces is marked, because otherwise a 7.1 display shows "Ls" twice with
// different levels and no way to tell which is which.
[[nodiscard]] std::vector<std::string> coded_channel_names(const ChannelPlan& plan);
[[nodiscard]] std::vector<std::string> coded_channel_names(LayoutId id);

// The independent substream's own coding mode - the plan a decoder that
// ignores every dependent would play.
[[nodiscard]] Acmod bed_acmod(LayoutId id);
[[nodiscard]] bool bed_lfe(LayoutId id);

// Every distinct location the plan renders, bed and dependents combined -
// what layout(id).rendered counts for a named layout, generalised to any
// plan.
[[nodiscard]] int rendered_channel_count(const ChannelPlan& plan);

// Speaker locations reordered into the order a WAV file interleaves them
// (WAVE_FORMAT_EXTENSIBLE: FL FR FC LFE BL BR ...): entry i is the index in
// `locations` of the channel belonging at WAV position i. Locations that order
// does not name - E-AC-3's Lw/Rw, Lsd/Rsd and LFE2 - follow in bitstream order
// rather than being dropped.
//
// Exported because the decode side needs the same answer: a decoded stream is
// written out as a WAV, and if that used a different convention from the one
// the encode side reads, a file would not survive a round trip.
[[nodiscard]] std::vector<std::size_t> wav_order(
    std::span<const eac3::chanmap::Location> locations);

// --- Annex E coding tools ---------------------------------------------------

// Every tool is a trade rather than a free win, so they are selected rather
// than assumed - and encoding the same material with and without one is the
// only way to say whether it earned its place.
struct Tools {
    bool coupling = false;
    int cplbegf = -1;
    bool spx = false;
    int spxbegf = -1;
    bool spx_atten = true;
    int spxattencod = -1;
    bool aht = false;
    int gaqmod = -1;

    [[nodiscard]] bool any() const { return coupling || spx || aht; }
};

inline constexpr std::string_view kToolsSyntax =
    "none | cpl | spx | aht | all (cpl:N / spx:N pin a band edge, aht:N the gain mode)";

// The '+'-joined token: "none", "cpl", "cpl+spx", "all", "cpl:4+spx:5",
// "aht:0", "spx+noatten", "atten:12". Returns false on anything unrecognised
// or out of range, leaving `out` partially written - callers reject rather
// than continue, because a silently ignored tool looks exactly like a tool
// that did not help.
[[nodiscard]] bool parse_tools(std::string_view text, Tools& out);

// The inverse, so a front end can show what it is about to do in the same
// vocabulary the command line takes. Round-trips through parse_tools.
[[nodiscard]] std::string format_tools(const Tools& tools);

// --- dynamic range, loudness and downmix metadata ---------------------------

// The whole §7.7 / §7.8 / Table E1.2 group a front end collects. Everything
// defaults off or to the format's own default, so a plan that says nothing
// about metadata produces exactly the stream it produced before this existed.
struct Metadata {
    std::optional<meta::Profile> drc = std::nullopt;
    std::optional<meta::HeavyConfig> heavy = std::nullopt;
    meta::CentreMixLevel cmixlev = meta::CentreMixLevel::kMinus4_5dB;
    meta::SurroundMixLevel surmixlev = meta::SurroundMixLevel::kMinus6dB;
    int dialnorm = 31;
    // Measure BS.1770 integrated loudness over the whole programme and derive
    // dialnorm from it (§5.4.2.8). The measurement needs the programme, not a
    // frame, so a front end does it before the first frame is encoded and
    // writes the answer back into `dialnorm`.
    bool measure_dialnorm = false;
    // Ch2's own dialnorm, meaningful only when the plan's layout is 1+1 dual
    // mono - the two programmes are levelled independently, so `dialnorm`
    // alone cannot describe both.
    int dialnorm2 = 31;
    bool measure_dialnorm2 = false;
    // E-AC-3 only: emit the mixmdate group. AC-3 carries cmixlev/surmixlev in
    // bsi and has nowhere to put the rest.
    bool mixmeta = false;
    std::optional<int> lfemix = meta::kLfeMixLevelIdeal;
    meta::DownmixMode dmixmod = meta::DownmixMode::kLoRo;
};

// The mixmdate group these options imply.
[[nodiscard]] meta::MixMetadata mix_metadata(const Metadata& options);

// --- the plan ---------------------------------------------------------------

struct Plan {
    Codec codec = Codec::kAc3;
    LayoutId layout = LayoutId::kStereo;
    // A caller-built alternative to `layout`: when set, this OVERRIDES
    // `layout` entirely and the plan targets exactly these Table E2.5
    // locations (ac3::eac3::chanmap::allocate) instead of a named preset.
    std::optional<std::uint16_t> custom_locations = std::nullopt;
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    Tools tools{};
    Metadata meta{};
};

enum class PlanError : std::uint8_t {
    kLayoutNeedsEac3,   // an immersive layout (or channel selection) asked of AC-3
    kBitrateNotLegal,   // AC-3 takes only the 19 Table 5.18 rates
    kNoSourceLayout,    // no standard speaker layout has that many channels
    kInvalidChannels,   // custom_locations is not a channel selection allocate() can satisfy
};

[[nodiscard]] std::string_view describe(PlanError error);

// The plan's channel plan: custom_locations resolved through allocate() if
// set, else channel_plan_for(layout). Every function below that consumes a
// Plan's channels goes through this, so a custom selection and a named
// layout are built exactly the same way. Assumes `plan` already passed
// validate(), the way ac3_config/eac3_config already assume a valid bitrate.
[[nodiscard]] ChannelPlan resolve(const Plan& plan);

// AC-3 only; the caller has already checked carries(). Coupling comes from
// tools.coupling, which is the one Annex E selector A/52 §7.4 also defines for
// the base syntax.
[[nodiscard]] EncoderConfig ac3_config(const Plan& plan);

// E-AC-3, including the dependent substreams the layout needs. Each dependent
// gets its own slice of the rate rather than a share of the independent's:
// substreams occupy one frame period, not one frame.
[[nodiscard]] eac3::AccessUnitConfig eac3_config(const Plan& plan);

[[nodiscard]] std::optional<PlanError> validate(const Plan& plan);

// --- routing a source onto a plan -------------------------------------------

// How the channels of a source reach the channels an encoder is fed.
//
// A front end must not require the user's file to already be in the layout
// they picked - a microphone is two channels and will stay two channels
// however immersive the target is. So a source is placed onto the target's
// speakers by direction rather than by index.
struct Routing {
    int source_channels = 0;
    int coded_channels = 0;
    // Row-major [coded * source_channels + source]. Mostly zero.
    std::vector<double> gain;

    [[nodiscard]] double at(int coded, int source) const {
        return gain[static_cast<std::size_t>(coded) * static_cast<std::size_t>(source_channels) +
                    static_cast<std::size_t>(source)];
    }
    // True when the routing is a pure permutation - every coded channel takes
    // exactly one source channel at unity. A front end can then say the source
    // was carried rather than rendered.
    [[nodiscard]] bool is_permutation() const;
};

// `wav_channels` is a source in WAVE_FORMAT_EXTENSIBLE order (FL FR FC LFE BL
// BR SL SR ...), which is what read_wav yields and what a capture endpoint
// delivers. The downmix levels matter because folding a wide source into a
// narrow layout is §7.8's job, not a panner's, and §7.8 is defined in terms of
// exactly these two levels.
[[nodiscard]] std::optional<Routing> route(const ChannelPlan& target, std::size_t wav_channels,
                                           meta::CentreMixLevel clev,
                                           meta::SurroundMixLevel slev);
[[nodiscard]] std::optional<Routing> route(LayoutId target, std::size_t wav_channels,
                                           meta::CentreMixLevel clev,
                                           meta::SurroundMixLevel slev);

// Applies a routing to one frame. `source` holds source_channels spans of
// `samples` samples; `coded` holds coded_channels spans of the same length and
// is OVERWRITTEN. No allocation.
void render(const Routing& routing, std::span<const std::span<const float>> source,
            std::span<const std::span<float>> coded, std::size_t samples);

}  // namespace ac3::plan
