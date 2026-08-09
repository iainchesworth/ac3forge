#include "ac3/encoder/plan.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numbers>

#include "ac3/io/wav.hpp"
#include "ac3/spatial/spatial.hpp"

namespace ac3::plan {

namespace {

using Location = eac3::chanmap::Location;

[[nodiscard]] bool is_lfe(Location location) {
    return location == Location::kLfe || location == Location::kLfe2;
}

// The independent substream's channels, in AC-3 coded order (Table 5.8) with
// the LFE last. This IS acmod_map/expand: Table E2.5's first five bits agree
// with Table 5.8's coded order by construction (acmod_map's own static_assert
// already proves the count agrees for every acmod), so a bed never needs its
// own hand-written location list.
[[nodiscard]] std::vector<Location> bed_locations(const ChannelPlan& plan) {
    const auto expanded =
        eac3::chanmap::expand(eac3::chanmap::acmod_map(plan.bed_acmod, plan.bed_lfe));
    return {expanded.begin(), expanded.end()};
}

// What a decoder ends up with: the bed, with each dependent's channels either
// replacing a bed channel of the same location or appended as a new one. The
// LFE is kept last, where every layout in this project puts it.
[[nodiscard]] std::vector<Location> rendered_locations(const ChannelPlan& plan) {
    auto out = bed_locations(plan);
    const bool lfe = !out.empty() && is_lfe(out.back());
    if (lfe) {
        out.pop_back();
    }
    for (const auto mask : plan.dependents) {
        const auto expanded = eac3::chanmap::expand(mask);
        for (const auto location : expanded) {
            if (std::ranges::find(out, location) == out.end()) {
                out.push_back(location);
            }
        }
    }
    if (lfe) {
        out.push_back(Location::kLfe);
    }
    return out;
}

// The named-layout callers below only ever want channel_plan_for(id)'s
// answer; keeping this overload spares them writing that out.
[[nodiscard]] std::vector<Location> rendered_locations(LayoutId id) {
    return rendered_locations(channel_plan_for(id));
}

// --- geometry ---------------------------------------------------------------

struct Direction {
    double azimuth_deg = 0.0;    // counterclockwise from front, ITU-R BS.775
    double elevation_deg = 0.0;  // 0 on the listener's plane
};

// Nominal elevation of the upper layer. TS 103 420 renders heights well above
// the ring; 45° is the conventional Atmos ceiling angle and the only number
// the crossfade below needs.
constexpr double kHeightElevationDeg = 45.0;

// Where a location sits. The one context-dependent entry is the side surround
// pair: with rear surrounds also present they move forward to +/-90 and the
// rears take the +/-150 the 5.1 ring would have given them, which is the
// physical difference between 5.1 and 7.1 rather than a naming one.
[[nodiscard]] Direction direction_of(Location location, bool has_rears) {
    switch (location) {
        case Location::kLeft: return {30.0, 0.0};
        case Location::kCentre: return {0.0, 0.0};
        case Location::kRight: return {-30.0, 0.0};
        case Location::kLeftSurround: return {has_rears ? 90.0 : 110.0, 0.0};
        case Location::kRightSurround: return {has_rears ? -90.0 : -110.0, 0.0};
        case Location::kLc: return {15.0, 0.0};
        case Location::kRc: return {-15.0, 0.0};
        case Location::kLrs: return {150.0, 0.0};
        case Location::kRrs: return {-150.0, 0.0};
        case Location::kCs: return {180.0, 0.0};
        case Location::kTs: return {0.0, 90.0};
        case Location::kLsd: return {90.0, 0.0};
        case Location::kRsd: return {-90.0, 0.0};
        case Location::kLw: return {60.0, 0.0};
        case Location::kRw: return {-60.0, 0.0};
        case Location::kVhl: return {45.0, kHeightElevationDeg};
        case Location::kVhr: return {-45.0, kHeightElevationDeg};
        case Location::kVhc: return {0.0, kHeightElevationDeg};
        case Location::kLts: return {135.0, kHeightElevationDeg};
        case Location::kRts: return {-135.0, kHeightElevationDeg};
        case Location::kLfe:
        case Location::kLfe2: return {0.0, 0.0};
    }
    return {};
}

// The speakers a pan may place a source on: everything in `locations` that has
// a direction at all. The LFE is deliberately absent - it is not a point on
// the ring, and leaving it in would give it the azimuth of the centre channel,
// so a centre-panned source would land in the subwoofer and nowhere else.
struct PanTargets {
    std::vector<Location> locations;
    std::vector<Direction> directions;

    // Where a location sits in this set, or -1 if it takes no panned audio.
    [[nodiscard]] int index_of(Location location) const {
        const auto at = std::ranges::find(locations, location);
        return at == locations.end()
                   ? -1
                   : static_cast<int>(std::distance(locations.begin(), at));
    }
};

[[nodiscard]] PanTargets pan_targets(std::span<const Location> locations) {
    const bool has_rears = std::ranges::find(locations, Location::kLrs) != locations.end();
    PanTargets out;
    for (const auto location : locations) {
        if (is_lfe(location)) {
            continue;
        }
        out.locations.push_back(location);
        out.directions.push_back(direction_of(location, has_rears));
    }
    return out;
}

// Everything at or above this counts as the upper layer. Half way to the
// nominal height angle, so no real location is ambiguous.
constexpr double kHeightThresholdDeg = kHeightElevationDeg / 2.0;

// A gain below this is not a quiet signal, it is arithmetic: cos(pi/2) lands
// near 6e-17 rather than on zero, and -334 dB of leakage into a channel that
// should be silent is worse than useless - it makes "is this channel carrying
// anything?" unanswerable and costs a multiply per sample to stay wrong.
constexpr double kNegligibleGain = 1e-9;

// One source direction spread over a target speaker set. Two rings - the
// listener's plane and the ceiling - each panned by azimuth, crossfaded by
// elevation at constant power.
//
// A target with no upper layer takes the whole source at full level rather
// than a cosine-attenuated share: a 5.1 ring has no height speakers, and a
// legacy decoder has to hear everything or backward compatibility means
// nothing. That is the same rule spatial::pan_room states for the 5.1 bed.
void pan_direction(Direction source, std::span<const Direction> targets,
                   std::span<double> gains) {
    std::ranges::fill(gains, 0.0);

    std::vector<double> low_az;
    std::vector<std::size_t> low_index;
    std::vector<double> high_az;
    std::vector<std::size_t> high_index;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].elevation_deg >= kHeightThresholdDeg) {
            high_az.push_back(targets[i].azimuth_deg);
            high_index.push_back(i);
        } else {
            low_az.push_back(targets[i].azimuth_deg);
            low_index.push_back(i);
        }
    }

    double weight_low = 1.0;
    double weight_high = 0.0;
    if (!high_az.empty() && !low_az.empty()) {
        const double t =
            std::clamp(source.elevation_deg / kHeightElevationDeg, 0.0, 1.0);
        weight_low = std::cos(t * std::numbers::pi / 2.0);
        weight_high = std::sin(t * std::numbers::pi / 2.0);
    } else if (low_az.empty()) {
        weight_low = 0.0;
        weight_high = 1.0;
    }

    if (weight_low > kNegligibleGain && !low_az.empty()) {
        std::vector<double> ring(low_az.size());
        spatial::pan_ring(source.azimuth_deg, low_az, ring);
        for (std::size_t i = 0; i < ring.size(); ++i) {
            gains[low_index[i]] += weight_low * ring[i];
        }
    }
    if (weight_high > kNegligibleGain && !high_az.empty()) {
        std::vector<double> ring(high_az.size());
        spatial::pan_ring(source.azimuth_deg, high_az, ring);
        for (std::size_t i = 0; i < ring.size(); ++i) {
            gains[high_index[i]] += weight_high * ring[i];
        }
    }
    for (auto& gain : gains) {
        if (gain < kNegligibleGain) {
            gain = 0.0;
        }
    }
}

// --- source layouts ---------------------------------------------------------

// Standard WAVEFORMATEXTENSIBLE speaker order, as far as it and Table E2.5
// overlap. Three E-AC-3 locations (Lw/Rw, Lsd/Rsd, LFE2) have no slot in that
// order at all; they follow in bitstream order rather than being dropped.
constexpr std::array<Location, 17> kWavSpeakerOrder = {
    Location::kLeft,           // FL
    Location::kRight,          // FR
    Location::kCentre,         // FC
    Location::kLfe,            // LFE
    Location::kLrs,            // BL
    Location::kRrs,            // BR
    Location::kLc,             // FLC
    Location::kRc,             // FRC
    Location::kCs,             // BC
    Location::kLeftSurround,   // SL
    Location::kRightSurround,  // SR
    Location::kTs,             // TC
    Location::kVhl,            // TFL
    Location::kVhc,            // TFC
    Location::kVhr,            // TFR
    Location::kLts,            // TBL
    Location::kRts,            // TBR
};

// A set of locations sorted into the order a WAV file interleaves them.
[[nodiscard]] std::vector<Location> in_wav_order(std::span<const Location> locations) {
    std::vector<Location> out;
    out.reserve(locations.size());
    for (const auto index : wav_order(locations)) {
        out.push_back(locations[index]);
    }
    return out;
}

// What a WAV of this width most likely holds, when its width does not match
// the layout being encoded. The counts that are ambiguous - eight channels is
// 7.1 or 5.1.2, ten is 5.1.4 or 7.1.2 - resolve to the commoner delivery
// layout; a source that really is the other one only has to be encoded to the
// matching target, which takes the exact-match path above instead.
[[nodiscard]] std::optional<std::vector<Location>> generic_wav_layout(std::size_t channels) {
    switch (channels) {
        case 1: return std::vector<Location>{Location::kCentre};
        case 2: return std::vector<Location>{Location::kLeft, Location::kRight};
        case 3:
            return std::vector<Location>{Location::kLeft, Location::kRight, Location::kCentre};
        case 4:
            return std::vector<Location>{Location::kLeft, Location::kRight,
                                         Location::kLeftSurround, Location::kRightSurround};
        case 5:
            return std::vector<Location>{Location::kLeft, Location::kRight, Location::kCentre,
                                         Location::kLeftSurround, Location::kRightSurround};
        case 6:
            return std::vector<Location>{Location::kLeft,  Location::kRight,
                                         Location::kCentre, Location::kLfe,
                                         Location::kLeftSurround, Location::kRightSurround};
        case 8: return in_wav_order(rendered_locations(LayoutId::k71));
        case 10: return in_wav_order(rendered_locations(LayoutId::k514));
        case 12: return in_wav_order(rendered_locations(LayoutId::k714));
        default: return std::nullopt;
    }
}

// --- fold-down --------------------------------------------------------------

// Folding a wide source into one or two channels has a specified answer
// (§7.8), and it is not what a panner would do: Lo/Ro sends each surround to
// its own side only, while a pairwise pan bleeds it across both. So the spec's
// coefficients are used wherever they are defined - which is wherever the
// source fits an acmod - and the panner handles everything else.
[[nodiscard]] bool fold_down(std::span<const Location> source, const ChannelPlan& target,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev,
                             Routing& out) {
    // Fold-down only has an answer (§7.8) when the whole target IS the bed
    // and the bed is mono or stereo - a target with any dependent, or an LFE
    // channel neither mono nor stereo layouts ever carried, has no entry.
    if (!target.dependents.empty() || target.bed_lfe) {
        return false;
    }
    const bool mono = target.bed_acmod == Acmod::k1_0;
    const bool stereo = target.bed_acmod == Acmod::k2_0;
    if (!mono && !stereo) {
        return false;
    }
    const auto source_layout = io::ac3_layout_for(source.size());
    if (!source_layout) {
        return false;  // §7.8 is defined per acmod; a wider source has no entry
    }
    const int fbw = fullbw_channel_count(source_layout->acmod);
    if (fbw <= (mono ? 1 : 2)) {
        return false;  // nothing to fold; the panner's identity is fine
    }

    const double c = meta::coefficient(clev);
    const double s = meta::coefficient(slev);
    if (mono) {
        const auto mono_gains = meta::mono_downmix(source_layout->acmod, c, s);
        for (int k = 0; k < fbw; ++k) {
            const auto wav = source_layout->wav_index[static_cast<std::size_t>(k)];
            out.gain[wav] = mono_gains[static_cast<std::size_t>(k)];
        }
        return true;
    }
    const auto stereo_gains = meta::stereo_downmix(source_layout->acmod, c, s);
    for (int k = 0; k < fbw; ++k) {
        const auto wav = source_layout->wav_index[static_cast<std::size_t>(k)];
        out.gain[wav] = stereo_gains.left[static_cast<std::size_t>(k)];
        out.gain[static_cast<std::size_t>(out.source_channels) + wav] =
            stereo_gains.right[static_cast<std::size_t>(k)];
    }
    return true;
}

}  // namespace

// --- layouts ----------------------------------------------------------------

std::optional<LayoutId> parse_layout(std::string_view name) {
    for (const auto& info : kLayouts) {
        if (info.name == name) {
            return info.id;
        }
    }
    return std::nullopt;
}

std::optional<LayoutId> layout_for_source(std::size_t wav_channels) {
    switch (wav_channels) {
        case 1: return LayoutId::kMono;
        case 2: return LayoutId::kStereo;
        // 3, 4 and 5 are legal acmods with no layout entry of their own. 5.1
        // holds all of them, and route() leaves the positions they do not fill
        // silent - which is what a source without a centre or an LFE means.
        case 3:
        case 4:
        case 5:
        case 6: return LayoutId::k51;
        case 8: return LayoutId::k71;
        case 10: return LayoutId::k514;
        case 12: return LayoutId::k714;
        default: return std::nullopt;
    }
}

std::string layout_names(Codec codec) {
    std::string out;
    for (const auto& info : kLayouts) {
        if (!carries(codec, info.id)) {
            continue;
        }
        if (!out.empty()) {
            out += " | ";
        }
        out += info.name;
    }
    return out;
}

ChannelPlan channel_plan_for(LayoutId id) {
    switch (id) {
        case LayoutId::kMono:
            return {.bed_acmod = Acmod::k1_0, .bed_lfe = false, .dependents = {}};
        case LayoutId::kStereo:
            return {.bed_acmod = Acmod::k2_0, .bed_lfe = false, .dependents = {}};
        case LayoutId::k51:
            return {.bed_acmod = Acmod::k3_2, .bed_lfe = true, .dependents = {}};
        case LayoutId::k71:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::k71Rear}};
        case LayoutId::k512:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::k512Height}};
        case LayoutId::k514:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::kTopQuad}};
        case LayoutId::k714:
            return {.bed_acmod = Acmod::k3_2,
                    .bed_lfe = true,
                    .dependents = {eac3::chanmap::k71Rear, eac3::chanmap::kTopQuad}};
    }
    return {};
}

std::optional<std::uint16_t> parse_channels(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<Location> wanted;
    while (!text.empty()) {
        const auto split = text.find(',');
        // A trailing separator with nothing after it means a location was
        // meant and did not survive whatever produced the string - the same
        // reasoning parse_tools already applies to '+'.
        if (split != std::string_view::npos && split + 1 == text.size()) {
            return std::nullopt;
        }
        const auto token = text.substr(0, split);
        if (token.empty()) {
            return std::nullopt;
        }
        const auto location = eac3::chanmap::parse_location(token);
        if (!location) {
            return std::nullopt;
        }
        wanted.push_back(*location);
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    }

    // Every Table E2.5 bit, so a pair location can be checked as a whole:
    // both members present sets the bit, one alone is rejected rather than
    // silently dropped or silently completed.
    constexpr std::array<std::uint16_t, 16> kBits = {
        eac3::chanmap::kLeftBit,     eac3::chanmap::kCentreBit,   eac3::chanmap::kRightBit,
        eac3::chanmap::kLeftSurroundBit, eac3::chanmap::kRightSurroundBit, eac3::chanmap::kLcRcBit,
        eac3::chanmap::kLrsRrsBit,   eac3::chanmap::kCsBit,       eac3::chanmap::kTsBit,
        eac3::chanmap::kLsdRsdBit,   eac3::chanmap::kLwRwBit,     eac3::chanmap::kVhlVhrBit,
        eac3::chanmap::kVhcBit,      eac3::chanmap::kLtsRtsBit,   eac3::chanmap::kLfe2Bit,
        eac3::chanmap::kLfeBit,
    };
    std::uint16_t mask = 0;
    for (const auto bit : kBits) {
        const auto expanded = eac3::chanmap::expand(bit);
        const auto present = static_cast<int>(std::ranges::count_if(
            expanded, [&](Location loc) { return std::ranges::find(wanted, loc) != wanted.end(); }));
        if (present == 0) {
            continue;
        }
        if (present != expanded.count) {
            return std::nullopt;  // named one half of a pair location, not both
        }
        mask = static_cast<std::uint16_t>(mask | bit);
    }
    // Catches a name repeated in `text`: it would otherwise vanish silently,
    // since the bit it belongs to is already accounted for by its first
    // appearance.
    if (eac3::chanmap::channel_count(mask) != static_cast<int>(wanted.size())) {
        return std::nullopt;
    }
    return mask;
}

std::string format_channels(std::uint16_t locations) {
    std::string out;
    for (const auto location : eac3::chanmap::expand(locations)) {
        if (!out.empty()) {
            out += ',';
        }
        out += eac3::chanmap::name(location);
    }
    return out;
}

std::vector<CodedChannel> coded_channels(const ChannelPlan& plan) {
    std::vector<CodedChannel> out;
    for (const auto location : bed_locations(plan)) {
        out.push_back({.location = location, .bed = true, .substream = 0});
    }
    int substream = 1;
    for (const auto mask : plan.dependents) {
        for (const auto location : eac3::chanmap::expand(mask)) {
            out.push_back({.location = location, .bed = false, .substream = substream});
        }
        ++substream;
    }
    return out;
}

std::vector<CodedChannel> coded_channels(LayoutId id) {
    return coded_channels(channel_plan_for(id));
}

std::vector<std::string> coded_channel_names(const ChannelPlan& plan) {
    const auto coded = coded_channels(plan);
    std::vector<std::string> out;
    out.reserve(coded.size());
    for (const auto& channel : coded) {
        std::string name{eac3::chanmap::name(channel.location)};
        // A bed channel a dependent overwrites still exists, still carries
        // audio and still reaches a 5.1 decoder - but a display that showed
        // "Ls" twice with different levels would give no way to tell which of
        // the two a reading belonged to.
        const bool replaced =
            channel.bed && std::ranges::any_of(coded, [&](const CodedChannel& other) {
                return !other.bed && other.location == channel.location;
            });
        if (replaced) {
            name += " (bed)";
        }
        out.push_back(std::move(name));
    }
    return out;
}

std::vector<std::string> coded_channel_names(LayoutId id) {
    return coded_channel_names(channel_plan_for(id));
}

Acmod bed_acmod(LayoutId id) { return channel_plan_for(id).bed_acmod; }

bool bed_lfe(LayoutId id) { return channel_plan_for(id).bed_lfe; }

int rendered_channel_count(const ChannelPlan& plan) {
    std::uint16_t occupied = eac3::chanmap::acmod_map(plan.bed_acmod, plan.bed_lfe);
    for (const auto mask : plan.dependents) {
        occupied |= mask;
    }
    return eac3::chanmap::expand(occupied).count;
}

std::vector<std::size_t> wav_order(std::span<const eac3::chanmap::Location> locations) {
    std::vector<std::size_t> out;
    out.reserve(locations.size());
    std::vector<bool> placed(locations.size(), false);
    for (const auto speaker : kWavSpeakerOrder) {
        const auto at = std::ranges::find(locations, speaker);
        if (at == locations.end()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(std::distance(locations.begin(), at));
        out.push_back(index);
        placed[index] = true;
    }
    for (std::size_t i = 0; i < locations.size(); ++i) {
        if (!placed[i]) {
            out.push_back(i);
        }
    }
    return out;
}

// --- tools ------------------------------------------------------------------

namespace {

[[nodiscard]] int parse_index(std::string_view text, int limit) {
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() ||
        value > static_cast<unsigned>(limit)) {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace

bool parse_tools(std::string_view text, Tools& out) {
    if (text.empty() || text == "none") {
        return true;
    }
    while (!text.empty()) {
        const auto split = text.find('+');
        // A separator with nothing after it means a token was meant and did
        // not survive whatever produced the string. Accepting it would turn a
        // truncated selection into a silently smaller one.
        if (split != std::string_view::npos && split + 1 == text.size()) {
            return false;
        }
        const auto token = text.substr(0, split);
        if (token.starts_with("cpl:")) {
            // Pinning the band edge is how a coupling-frequency question gets
            // answered by experiment rather than by argument.
            out.coupling = true;
            out.cplbegf = parse_index(token.substr(4), 15);
            if (out.cplbegf < 0) {
                return false;
            }
        } else if (token.starts_with("spx:")) {
            out.spx = true;
            out.spxbegf = parse_index(token.substr(4), 7);
            if (out.spxbegf < 0) {
                return false;
            }
        } else if (token == "cpl") {
            out.coupling = true;
        } else if (token == "spx") {
            out.spx = true;
        } else if (token == "noatten") {
            out.spx_atten = false;  // spectral extension without its seam notch
        } else if (token.starts_with("atten:")) {
            out.spxattencod = parse_index(token.substr(6), 31);
            if (out.spxattencod < 0) {
                return false;
            }
        } else if (token.starts_with("aht:")) {
            // "aht:0" is AHT with gain-adaptive quantization switched off,
            // which is how GAQ's own contribution gets measured.
            out.aht = true;
            out.gaqmod = parse_index(token.substr(4), 3);
            if (out.gaqmod < 0) {
                return false;
            }
        } else if (token == "aht") {
            out.aht = true;
        } else if (token == "all") {
            out.coupling = true;
            out.spx = true;
            out.aht = true;
        } else {
            return false;
        }
        text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    }
    return true;
}

std::string format_tools(const Tools& tools) {
    std::string out;
    const auto add = [&out](std::string_view token) {
        if (!out.empty()) {
            out += '+';
        }
        out += token;
    };
    if (tools.coupling) {
        add(tools.cplbegf >= 0 ? "cpl:" + std::to_string(tools.cplbegf) : std::string{"cpl"});
    }
    if (tools.spx) {
        add(tools.spxbegf >= 0 ? "spx:" + std::to_string(tools.spxbegf) : std::string{"spx"});
        if (!tools.spx_atten) {
            add("noatten");
        } else if (tools.spxattencod >= 0) {
            add("atten:" + std::to_string(tools.spxattencod));
        }
    }
    if (tools.aht) {
        add(tools.gaqmod >= 0 ? "aht:" + std::to_string(tools.gaqmod) : std::string{"aht"});
    }
    return out.empty() ? std::string{"none"} : out;
}

// --- metadata ---------------------------------------------------------------

namespace {

// The two coarse AC-3 levels have no exact 3-bit twin for every value, but
// each one they do have is the same coefficient, so an E-AC-3 stream asked for
// "-4.5 dB centre" gets the level a listener would measure either way.
[[nodiscard]] meta::MixLevel widen(meta::CentreMixLevel value) {
    switch (value) {
        case meta::CentreMixLevel::kMinus3dB: return meta::MixLevel::kMinus3dB;
        case meta::CentreMixLevel::kMinus4_5dB: return meta::MixLevel::kMinus4_5dB;
        case meta::CentreMixLevel::kMinus6dB: return meta::MixLevel::kMinus6dB;
    }
    return meta::MixLevel::kMinus4_5dB;
}

[[nodiscard]] meta::MixLevel widen(meta::SurroundMixLevel value) {
    switch (value) {
        case meta::SurroundMixLevel::kMinus3dB: return meta::MixLevel::kMinus3dB;
        case meta::SurroundMixLevel::kMinus6dB: return meta::MixLevel::kMinus6dB;
        case meta::SurroundMixLevel::kSilent: return meta::MixLevel::kSilent;
    }
    return meta::MixLevel::kMinus6dB;
}

}  // namespace

meta::MixMetadata mix_metadata(const Metadata& options) {
    return {.dmixmod = options.dmixmod,
            // Lt/Rt folds down into a matrix that will be re-decoded, so the
            // centre traditionally sits 1.5 dB hotter there than in Lo/Ro.
            .ltrtcmixlev = meta::MixLevel::kMinus3dB,
            .lorocmixlev = widen(options.cmixlev),
            .ltrtsurmixlev = meta::MixLevel::kMinus3dB,
            .lorosurmixlev = widen(options.surmixlev),
            .lfemixlevcod = options.lfemix};
}

// --- configs ----------------------------------------------------------------

std::string_view describe(PlanError error) {
    switch (error) {
        case PlanError::kLayoutNeedsEac3:
            return "that channel selection needs dependent substreams, which only E-AC-3 has "
                   "(AC-3 codes nothing wider than 3/2 + LFE)";
        case PlanError::kBitrateNotLegal:
            return "AC-3 takes only the 19 nominal rates of Table 5.18";
        case PlanError::kNoSourceLayout:
            return "no standard speaker layout has that many channels";
        case PlanError::kInvalidChannels:
            return "that channel selection is not one A/52 Annex E can express";
    }
    return "";
}

std::optional<PlanError> validate(const Plan& plan) {
    if (plan.custom_locations) {
        const auto allocated = eac3::chanmap::allocate(*plan.custom_locations);
        if (!allocated) {
            return PlanError::kInvalidChannels;
        }
        if (plan.codec == Codec::kAc3 && !allocated->dependents.empty()) {
            return PlanError::kLayoutNeedsEac3;
        }
    } else if (!carries(plan.codec, plan.layout)) {
        return PlanError::kLayoutNeedsEac3;
    }
    // E-AC-3 signals frmsiz directly, so any rate is expressible there; AC-3
    // indexes Table 5.18 and cannot say anything else.
    if (plan.codec == Codec::kAc3 && !is_valid_bitrate(plan.bitrate_kbps)) {
        return PlanError::kBitrateNotLegal;
    }
    return std::nullopt;
}

ChannelPlan resolve(const Plan& plan) {
    if (plan.custom_locations) {
        return eac3::chanmap::allocate(*plan.custom_locations).value_or(ChannelPlan{});
    }
    return channel_plan_for(plan.layout);
}

EncoderConfig ac3_config(const Plan& plan) {
    const auto cp = resolve(plan);
    return {.sample_rate = plan.sample_rate,
            .bitrate_kbps = plan.bitrate_kbps,
            .dialnorm = plan.meta.dialnorm,
            .acmod = cp.bed_acmod,
            .lfe = cp.bed_lfe,
            // Coupling shares coefficients between full-bandwidth channels
            // (§7.4), so a mono programme has nothing to share it with.
            .coupling = plan.tools.coupling && fullbw_channel_count(cp.bed_acmod) >= 2,
            .cplbegf = plan.tools.cplbegf,
            .drc = plan.meta.drc,
            .heavy = plan.meta.heavy,
            .cmixlev = plan.meta.cmixlev,
            .surmixlev = plan.meta.surmixlev};
}

namespace {

void apply_tools(const Tools& tools, eac3::FrameConfig& config) {
    config.coupling = tools.coupling && fullbw_channel_count(config.acmod) >= 2;
    config.cplbegf = tools.cplbegf;
    config.spx = tools.spx;
    config.spxbegf = tools.spxbegf;
    config.spx_atten = tools.spx_atten;
    config.spxattencod = tools.spxattencod;
    config.aht = tools.aht;
    config.gaqmod = tools.gaqmod;
}

}  // namespace

eac3::AccessUnitConfig eac3_config(const Plan& plan) {
    const auto cp = resolve(plan);
    eac3::AccessUnitConfig out;
    auto& independent = out.independent;
    independent.sample_rate = plan.sample_rate;
    independent.bitrate_kbps = plan.bitrate_kbps;
    independent.acmod = cp.bed_acmod;
    independent.lfe = cp.bed_lfe;
    independent.dialnorm = plan.meta.dialnorm;
    independent.drc = plan.meta.drc;
    independent.heavy = plan.meta.heavy;
    if (plan.meta.mixmeta) {
        independent.mixing = mix_metadata(plan.meta);
    }
    apply_tools(plan.tools, independent);

    // A dependent gets its own slice of the rate rather than a share of the
    // independent's - substreams occupy one frame period, not one frame.
    const std::uint32_t dependent_kbps = plan.bitrate_kbps / 2;
    for (const auto mask : cp.dependents) {
        eac3::FrameConfig dependent{};
        dependent.sample_rate = plan.sample_rate;
        dependent.bitrate_kbps = dependent_kbps;
        dependent.chanmap = mask;
        // `mask` came from a ChannelPlan that channel_plan_for/allocate()
        // already built to satisfy exactly one (acmod, lfeon) - it cannot
        // fail here.
        const auto fit = eac3::chanmap::acmod_for_chanmap(mask);
        dependent.acmod = fit->first;
        dependent.lfe = fit->second;
        apply_tools(plan.tools, dependent);
        out.dependents.push_back(dependent);
    }
    return out;
}

// --- routing ----------------------------------------------------------------

bool Routing::is_permutation() const {
    if (source_channels != coded_channels) {
        return false;
    }
    // Compared with a tolerance rather than exactly: a pan that lands on a
    // speaker's own direction solves to unity through a 2x2 system, and the
    // last bit of that is not something a caller should have to reason about.
    std::vector<int> used(static_cast<std::size_t>(source_channels), 0);
    for (int coded = 0; coded < coded_channels; ++coded) {
        int taken = -1;
        for (int source = 0; source < source_channels; ++source) {
            const double g = at(coded, source);
            if (g <= kNegligibleGain) {
                continue;
            }
            if (std::abs(g - 1.0) > kNegligibleGain || taken >= 0) {
                return false;
            }
            taken = source;
        }
        if (taken < 0 || used[static_cast<std::size_t>(taken)]++ != 0) {
            return false;
        }
    }
    return true;
}

std::optional<Routing> route(const ChannelPlan& target, std::size_t wav_channels,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev) {
    if (wav_channels == 0) {
        return std::nullopt;
    }
    // A source exactly as wide as the target is taken to BE the target, in WAV
    // speaker order. Nothing else can distinguish 7.1 from 5.1.2 at eight
    // channels, and a user who picked 7.1 for an eight-channel file has said
    // which one it is.
    std::vector<Location> source;
    if (wav_channels == static_cast<std::size_t>(rendered_channel_count(target))) {
        source = in_wav_order(rendered_locations(target));
    } else {
        const auto generic = generic_wav_layout(wav_channels);
        if (!generic) {
            return std::nullopt;
        }
        source = *generic;
    }

    const auto coded = coded_channels(target);
    Routing out{.source_channels = static_cast<int>(source.size()),
                .coded_channels = static_cast<int>(coded.size()),
                .gain = std::vector<double>(source.size() * coded.size(), 0.0)};

    // The LFE is not a direction and never takes part in a pan: §7.8 makes its
    // downmix optional and objects reach it only by an explicit send.
    for (std::size_t c = 0; c < coded.size(); ++c) {
        if (!is_lfe(coded[c].location)) {
            continue;
        }
        for (std::size_t s = 0; s < source.size(); ++s) {
            if (is_lfe(source[s])) {
                out.gain[c * source.size() + s] = 1.0;
            }
        }
    }

    if (fold_down(source, target, clev, slev, out)) {
        return out;
    }

    // Two target sets, because a bed channel and a dependent channel of the
    // same name need not carry the same thing.
    //
    // A dependent either REPLACES a bed channel (7.1's side surrounds) or ADDS
    // one (the heights), and which it does decides what the bed underneath is
    // fed:
    //
    //   Replaced - the bed channel is fed a rendering of the BED layout, so a
    //   7.1 source's sides and rears both fold into the 5.1 surround a legacy
    //   decoder plays. Nothing is heard twice, because a full decoder throws
    //   that channel away and takes the dependent's instead (§E3.8.2).
    //
    //   Not replaced - the bed channel is fed a rendering of the FULL layout,
    //   so height content lands only on the height speakers. Folding it into
    //   the bed as well would have a 5.1.4 decoder play it twice, once from
    //   above and once from the ring.
    //
    // The cost of the second rule is that a 5.1 decoder does not hear the
    // height layer at all. That is what a channel-based height extension is:
    // carrying it in both places is not an option, and the object path (JOC,
    // TS 103 420) is what exists for the case where it has to survive.
    const auto bed = pan_targets(bed_locations(target));
    const auto rendered = pan_targets(rendered_locations(target));

    std::vector<bool> replaced(coded.size(), false);
    for (std::size_t c = 0; c < coded.size(); ++c) {
        replaced[c] = coded[c].bed &&
                      std::ranges::any_of(coded, [&](const CodedChannel& other) {
                          return !other.bed && other.location == coded[c].location;
                      });
    }

    std::vector<double> bed_gains(bed.directions.size());
    std::vector<double> rendered_gains(rendered.directions.size());
    const bool source_has_rears =
        std::ranges::find(source, Location::kLrs) != source.end();

    for (std::size_t s = 0; s < source.size(); ++s) {
        if (is_lfe(source[s])) {
            continue;
        }
        const auto direction = direction_of(source[s], source_has_rears);
        pan_direction(direction, bed.directions, bed_gains);
        pan_direction(direction, rendered.directions, rendered_gains);

        for (std::size_t c = 0; c < coded.size(); ++c) {
            if (is_lfe(coded[c].location)) {
                continue;
            }
            const auto& set = replaced[c] ? bed : rendered;
            const auto& gains = replaced[c] ? bed_gains : rendered_gains;
            const int index = set.index_of(coded[c].location);
            if (index >= 0) {
                out.gain[c * source.size() + s] = gains[static_cast<std::size_t>(index)];
            }
        }
    }

    // §7.8.1's normalisation: where several source channels land on one
    // speaker their coefficients sum, and a sum above unity clips. A 5.1.4
    // source rendered into 7.1.4 folds the ceiling into the bed's surrounds
    // and measured +1.5 dBFS before this existed.
    //
    // Applied per SUBSTREAM rather than across the whole access unit. §7.8's
    // "attenuating all downmix coefficients equally" is about one downmix, and
    // the independent substream is the only downmix here - the dependents
    // carry the discrete rendering, at unity, with nothing folded into them.
    // Scaling everything together would drop that rendering by as much as
    // 12 dB to protect a fallback nobody with a wide decoder ever hears, which
    // is a worse answer than the clipping it prevents.
    //
    // Within the bed it IS one factor rather than one per speaker: scaling
    // only the oversubscribed speaker would change the balance between
    // speakers, which is a different mix rather than a quieter one.
    int substreams = 0;
    for (const auto& channel : coded) {
        substreams = std::max(substreams, channel.substream);
    }
    for (int substream = 0; substream <= substreams; ++substream) {
        double loudest = 1.0;
        for (std::size_t c = 0; c < coded.size(); ++c) {
            if (coded[c].substream != substream) {
                continue;
            }
            double sum = 0.0;
            for (std::size_t s = 0; s < source.size(); ++s) {
                sum += std::abs(out.gain[c * source.size() + s]);
            }
            loudest = std::max(loudest, sum);
        }
        if (loudest <= 1.0) {
            continue;
        }
        for (std::size_t c = 0; c < coded.size(); ++c) {
            if (coded[c].substream != substream) {
                continue;
            }
            for (std::size_t s = 0; s < source.size(); ++s) {
                out.gain[c * source.size() + s] /= loudest;
            }
        }
    }
    return out;
}

std::optional<Routing> route(LayoutId target, std::size_t wav_channels,
                             meta::CentreMixLevel clev, meta::SurroundMixLevel slev) {
    return route(channel_plan_for(target), wav_channels, clev, slev);
}

void render(const Routing& routing, std::span<const std::span<const float>> source,
            std::span<const std::span<float>> coded, std::size_t samples) {
    for (int c = 0; c < routing.coded_channels; ++c) {
        auto out = coded[static_cast<std::size_t>(c)];
        std::fill_n(out.begin(), samples, 0.0f);
        for (int s = 0; s < routing.source_channels; ++s) {
            const double gain = routing.at(c, s);
            if (gain == 0.0) {
                continue;
            }
            const auto in = source[static_cast<std::size_t>(s)];
            const auto n = std::min(samples, in.size());
            for (std::size_t i = 0; i < n; ++i) {
                out[i] += static_cast<float>(gain * static_cast<double>(in[i]));
            }
        }
    }
}

}  // namespace ac3::plan
