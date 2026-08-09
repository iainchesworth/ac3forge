#include "ac3/core/eac3_tables.hpp"

#include <array>

// The dynamic channel allocator (A/52 Annex E, Table E2.5 / Table 5.8). See
// eac3_tables.hpp for what each piece is for; this file is where the
// combinatorics that only make sense as ordinary (non-constexpr) code live.

namespace ac3::eac3::chanmap {

namespace {

// Every non-dual-mono acmod (1+1 codes two programmes, not a layout - see the
// comment on kMaxSubstreamChannels' neighbours), widest full-bandwidth count
// first. Where two acmods share a count (3/1 and 2/2 both code four), this
// order picks 2/2 - the choice the named 7.1 and 7.1.4 layouts already make
// for their own dependents, so routing them through this table rather than
// their old ad hoc arithmetic changes nothing about what they emit.
constexpr std::array<Acmod, 7> kBedCandidates = {
    Acmod::k3_2, Acmod::k2_2, Acmod::k3_1, Acmod::k3_0, Acmod::k2_1, Acmod::k2_0, Acmod::k1_0,
};

// The widest acmod whose own full-bandwidth locations are all present in
// `locations`. Only a dependent may carry a custom chanmap (§E2.3.1.7), so
// the bed can never be more than one of Table 5.8's eight shapes - this is
// the bed those shapes compete for.
[[nodiscard]] std::optional<Acmod> bed_for(std::uint16_t locations) {
    for (const auto acmod : kBedCandidates) {
        const auto own = acmod_map(acmod, false);
        if ((own & ~locations) == 0) {
            return acmod;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view describe(AllocationError error) {
    switch (error) {
        case AllocationError::kTooManyChannels:
            return "a single programme can render at most 16 channels (A/52 Annex E, §E3.8.2)";
        case AllocationError::kNoBedFit:
            return "no Table 5.8 coding mode covers any of the requested front channels";
        case AllocationError::kOrphanLfe2:
            return "LFE2 needs a full-bandwidth channel sharing its substream, and none is left";
    }
    return "";
}

std::optional<std::pair<Acmod, bool>> acmod_for_chanmap(std::uint16_t mask) {
    // lfeon is a single bit: a substream has room for one LFE-type channel,
    // never both LFE and LFE2 at once.
    if ((mask & kLfe) != 0 && (mask & kLfe2) != 0) {
        return std::nullopt;
    }
    const bool lfe = (mask & (kLfe | kLfe2)) != 0;
    const int fullbw = channel_count(mask) - (lfe ? 1 : 0);
    for (const auto acmod : kBedCandidates) {
        if (fullbw_channel_count(acmod) == fullbw) {
            return std::pair{acmod, lfe};
        }
    }
    // fullbw is 0 (an LFE-type location with no companion) or above 5 (Table
    // 5.8 has nothing wider than 3/2) - no acmod codes that many channels.
    return std::nullopt;
}

std::optional<Location> parse_location(std::string_view text) {
    for (int i = 0; i < kMaxChannels; ++i) {
        const auto location = static_cast<Location>(i);
        if (name(location) == text) {
            return location;
        }
    }
    return std::nullopt;
}

std::expected<ChannelPlan, AllocationError> allocate(std::uint16_t locations) {
    // Bed and dependents partition `locations` disjointly by construction
    // (dependents only ever claim what's left after the bed), so this one
    // upfront count IS the §E3.8.2 total.
    if (channel_count(locations) > 16) {
        return std::unexpected(AllocationError::kTooManyChannels);
    }
    const auto bed_acmod = bed_for(locations);
    if (!bed_acmod) {
        return std::unexpected(AllocationError::kNoBedFit);
    }

    ChannelPlan plan;
    plan.bed_acmod = *bed_acmod;
    plan.bed_lfe = (locations & kLfe) != 0;
    auto remaining =
        static_cast<std::uint16_t>(locations & ~acmod_map(*bed_acmod, plan.bed_lfe));

    // LFE2 is the one location that structurally needs a companion (acmod
    // always contributes at least one full-bandwidth channel, so it can never
    // be a dependent's entire content) - held back and placed last, once
    // every other dependent's own shape is settled.
    const bool needs_lfe2 = (remaining & kLfe2) != 0;
    remaining = static_cast<std::uint16_t>(remaining & ~kLfe2);

    // Bin-pack the rest in Table E2.5 bit order: keep adding to the current
    // dependent until the next location would push it past
    // kMaxSubstreamFullbw, then start a new one. Every bit left in `remaining`
    // is full-bandwidth-only (LFE2 was already pulled out above, and LFE
    // itself is never in `remaining` - it is either the bed's, via bed_lfe,
    // or absent), so the per-dependent cap here is five, not
    // kMaxSubstreamChannels' six: that sixth channel exists only for the one
    // LFE-type location a substream may carry, which this loop never packs.
    // Bed and dependents already sum to at most 16 (checked above) and each
    // dependent holds at least one location, so this can never need more
    // than a handful of dependents - nowhere near the format's ceiling of
    // eight.
    for (int bit = 0; bit < 16; ++bit) {
        const auto flag = static_cast<std::uint16_t>(0x8000u >> bit);
        if ((remaining & flag) == 0) {
            continue;
        }
        const int added = channel_count(flag);
        if (plan.dependents.empty() ||
            channel_count(plan.dependents.back()) + added > kMaxSubstreamFullbw) {
            plan.dependents.push_back(0);
        }
        plan.dependents.back() = static_cast<std::uint16_t>(plan.dependents.back() | flag);
    }

    if (needs_lfe2) {
        bool placed = false;
        for (auto& dependent : plan.dependents) {
            if (channel_count(dependent) < kMaxSubstreamChannels) {
                dependent = static_cast<std::uint16_t>(dependent | kLfe2);
                placed = true;
                break;
            }
        }
        if (!placed) {
            return std::unexpected(AllocationError::kOrphanLfe2);
        }
    }

    return plan;
}

}  // namespace ac3::eac3::chanmap
