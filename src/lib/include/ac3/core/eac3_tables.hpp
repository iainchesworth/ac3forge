#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"

// The Annex E tables that both sides of the codec need. An encoder writes a
// chanmap and a decoder reads one; a frame exponent strategy is a table
// lookup in one direction and the same lookup in the other. Keeping them here
// rather than in the encoder is what stops the two from drifting into
// disagreement about what a bit pattern means.

namespace ac3::eac3 {

// §E2.3.1.6: bit streams compliant with Annex E carry bsid 16. Values 11-15
// are earlier E-AC-3 versions a version-16 decoder is required to decode too;
// they use this same syntax, so both ends here treat them alike.
inline constexpr int kBsid = 16;
inline constexpr int kMinDecodableBsid = 11;

// §E2.3.1.1, Table E2.1.
enum class StreamType : std::uint8_t {
    kIndependent = 0,  // decodable alone; begins an access unit
    kDependent = 1,    // extends the independent substream it follows
    kConvertible = 2,  // independent, and previously coded as AC-3
    kReserved = 3,
};

// Table E2.4. Fewer than six blocks shortens the syncframe rather than the
// sample rate, so a frame carries numblks * 256 samples per channel.
[[nodiscard]] constexpr int blocks_per_syncframe(int numblkscod) {
    constexpr std::array<int, 4> counts = {1, 2, 3, 6};
    return counts[static_cast<std::size_t>(numblkscod & 0x3)];
}

// §E2.3.2.27: the width of blkstrtinfo. words_per_frame is frmsiz + 1, and
// bit_width(n - 1) is ceil(log2(n)) for every n >= 1.
[[nodiscard]] constexpr int block_start_info_bits(int numblks, std::uint32_t words_per_frame) {
    return (numblks - 1) * (4 + static_cast<int>(std::bit_width(words_per_frame - 1)));
}

// Table E2.10: one 5-bit code fixes the exponent strategy of all six blocks,
// as the raw chexpstr codes (0 reuse, 1 D15, 2 D25, 3 D45). Transcribed row
// for row; the comment is the spec's own notation.
inline constexpr std::array<std::array<std::uint8_t, 6>, 32> kFrameExpStrategies = {{
    {1, 0, 0, 0, 0, 0},  //  0  D15 R   R   R   R   R
    {1, 0, 0, 0, 0, 3},  //  1  D15 R   R   R   R   D45
    {1, 0, 0, 0, 2, 0},  //  2  D15 R   R   R   D25 R
    {1, 0, 0, 0, 3, 3},  //  3  D15 R   R   R   D45 D45
    {2, 0, 0, 2, 0, 0},  //  4  D25 R   R   D25 R   R
    {2, 0, 0, 2, 0, 3},  //  5  D25 R   R   D25 R   D45
    {2, 0, 0, 3, 2, 0},  //  6  D25 R   R   D45 D25 R
    {2, 0, 0, 3, 3, 3},  //  7  D25 R   R   D45 D45 D45
    {2, 0, 1, 0, 0, 0},  //  8  D25 R   D15 R   R   R
    {2, 0, 2, 0, 0, 3},  //  9  D25 R   D25 R   R   D45
    {2, 0, 2, 0, 2, 0},  // 10  D25 R   D25 R   D25 R
    {2, 0, 2, 0, 3, 3},  // 11  D25 R   D25 R   D45 D45
    {2, 0, 3, 2, 0, 0},  // 12  D25 R   D45 D25 R   R
    {2, 0, 3, 2, 0, 3},  // 13  D25 R   D45 D25 R   D45
    {2, 0, 3, 3, 2, 0},  // 14  D25 R   D45 D45 D25 R
    {2, 0, 3, 3, 3, 3},  // 15  D25 R   D45 D45 D45 D45
    {3, 1, 0, 0, 0, 0},  // 16  D45 D15 R   R   R   R
    {3, 1, 0, 0, 0, 3},  // 17  D45 D15 R   R   R   D45
    {3, 2, 0, 0, 2, 0},  // 18  D45 D25 R   R   D25 R
    {3, 2, 0, 0, 3, 3},  // 19  D45 D25 R   R   D45 D45
    {3, 2, 0, 2, 0, 0},  // 20  D45 D25 R   D25 R   R
    {3, 2, 0, 2, 0, 3},  // 21  D45 D25 R   D25 R   D45
    {3, 2, 0, 3, 2, 0},  // 22  D45 D25 R   D45 D25 R
    {3, 2, 0, 3, 3, 3},  // 23  D45 D25 R   D45 D45 D45
    {3, 3, 1, 0, 0, 0},  // 24  D45 D45 D15 R   R   R
    {3, 3, 2, 0, 0, 3},  // 25  D45 D45 D25 R   R   D45
    {3, 3, 2, 0, 2, 0},  // 26  D45 D45 D25 R   D25 R
    {3, 3, 2, 0, 3, 3},  // 27  D45 D45 D25 R   D45 D45
    {3, 3, 3, 2, 0, 0},  // 28  D45 D45 D45 D25 R   R
    {3, 3, 3, 2, 0, 3},  // 29  D45 D45 D45 D25 R   D45
    {3, 3, 3, 3, 2, 0},  // 30  D45 D45 D45 D45 D25 R
    {3, 3, 3, 3, 3, 3},  // 31  D45 D45 D45 D45 D45 D45
}};

[[nodiscard]] constexpr ExpStrategy frame_exp_strategy(int code, int block) {
    return static_cast<ExpStrategy>(
        kFrameExpStrategies[static_cast<std::size_t>(code)][static_cast<std::size_t>(block)]);
}

// Every row must start with a real strategy: block 0 has nothing to reuse.
static_assert([] {
    for (const auto& row : kFrameExpStrategies) {
        if (row[0] == 0) {
            return false;
        }
    }
    return true;
}());

// §E2.3.1.8, Table E2.5 - the custom channel map. Bit 0 is stored in the MOST
// significant bit of the 16-bit field ("Bit 0, which indicates the presence of
// the left channel, is stored in the most significant bit"), so location n has
// mask 1 << (15 - n). The spec's own worked example - bits 3, 4 and 6 giving
// Ls, Rs, Lrs, Rrs - is 0x1A00, which only comes out right under that
// numbering.
//
// Six of the sixteen locations name a PAIR of channels rather than one, so a
// map's population count is not its channel count.
namespace chanmap {

// "Bit" suffix distinguishes these mask constants from the same-named
// Location enumerators below - GCC's -Wshadow (unlike Clang's) flags an enum
// class's enumerators against outer-scope declarations of the same name even
// though they are never reachable unqualified, so the two vocabularies need
// distinct spellings to build warning-clean everywhere.
inline constexpr std::uint16_t kLeftBit = 0x8000;          // bit 0
inline constexpr std::uint16_t kCentreBit = 0x4000;        // bit 1
inline constexpr std::uint16_t kRightBit = 0x2000;         // bit 2
inline constexpr std::uint16_t kLeftSurroundBit = 0x1000;  // bit 3
inline constexpr std::uint16_t kRightSurroundBit = 0x0800; // bit 4
inline constexpr std::uint16_t kLcRcBit = 0x0400;          // bit 5  (pair)
inline constexpr std::uint16_t kLrsRrsBit = 0x0200;        // bit 6  (pair)
inline constexpr std::uint16_t kCsBit = 0x0100;            // bit 7
inline constexpr std::uint16_t kTsBit = 0x0080;            // bit 8
inline constexpr std::uint16_t kLsdRsdBit = 0x0040;        // bit 9  (pair)
inline constexpr std::uint16_t kLwRwBit = 0x0020;          // bit 10 (pair)
inline constexpr std::uint16_t kVhlVhrBit = 0x0010;        // bit 11 (pair)
inline constexpr std::uint16_t kVhcBit = 0x0008;           // bit 12
inline constexpr std::uint16_t kLtsRtsBit = 0x0004;        // bit 13 (pair)
inline constexpr std::uint16_t kLfe2Bit = 0x0002;          // bit 14
inline constexpr std::uint16_t kLfeBit = 0x0001;           // bit 15

inline constexpr std::uint16_t kPairs =
    kLcRcBit | kLrsRrsBit | kLsdRsdBit | kLwRwBit | kVhlVhrBit | kLtsRtsBit;

// Coded channels a map accounts for. §E2.3.1.8 requires this to equal the
// channels the substream's acmod and lfeon code, and the coded order to
// follow the enabled bits from bit 0 downwards.
[[nodiscard]] constexpr int channel_count(std::uint16_t map) {
    return std::popcount(map) +
           std::popcount(static_cast<std::uint16_t>(map & kPairs));
}

// One speaker feed. A pair location expands to two adjacent enumerators, in
// that order, which is what lets the expansion below be a single sweep.
enum class Location : std::uint8_t {
    kLeft,
    kCentre,
    kRight,
    kLeftSurround,
    kRightSurround,
    kLc,
    kRc,
    kLrs,
    kRrs,
    kCs,
    kTs,
    kLsd,
    kRsd,
    kLw,
    kRw,
    kVhl,
    kVhr,
    kVhc,
    kLts,
    kRts,
    kLfe2,
    kLfe,
};

// Sixteen locations, six of which name two channels.
inline constexpr int kMaxChannels = 22;

[[nodiscard]] constexpr std::string_view name(Location location) {
    constexpr std::array<std::string_view, kMaxChannels> names = {
        "L",   "C",   "R",   "Ls",  "Rs",  "Lc",  "Rc",  "Lrs",
        "Rrs", "Cs",  "Ts",  "Lsd", "Rsd", "Lw",  "Rw",  "Vhl",
        "Vhr", "Vhc", "Lts", "Rts", "LFE2", "LFE"};
    return names[static_cast<std::size_t>(location)];
}

// A map's locations in coded order, which §E2.3.1.8 defines as bit order.
struct Layout {
    std::array<Location, kMaxChannels> items{};
    int count = 0;

    [[nodiscard]] constexpr Location operator[](int index) const {
        return items[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] constexpr auto begin() const { return items.begin(); }
    [[nodiscard]] constexpr auto end() const {
        return std::next(items.begin(), count);
    }
    // Where a location sits in this layout, or -1.
    [[nodiscard]] constexpr int index_of(Location location) const {
        for (int i = 0; i < count; ++i) {
            if (items[static_cast<std::size_t>(i)] == location) {
                return i;
            }
        }
        return -1;
    }
};

[[nodiscard]] constexpr Layout expand(std::uint16_t map) {
    Layout out;
    for (int bit = 0; bit < 16; ++bit) {
        if ((map & (0x8000u >> bit)) == 0) {
            continue;
        }
        // The enumerators run in bit order with each pair's two members
        // adjacent, so the first location of bit n is n plus the number of
        // pair bits below it.
        const auto above = static_cast<std::uint16_t>(~(0xFFFFu >> bit));
        const auto lower_pairs =
            std::popcount(static_cast<std::uint16_t>(kPairs & above));
        auto location = static_cast<Location>(bit + lower_pairs);
        out.items[static_cast<std::size_t>(out.count++)] = location;
        if ((kPairs & (0x8000u >> bit)) != 0) {
            location = static_cast<Location>(static_cast<std::uint8_t>(location) + 1);
            out.items[static_cast<std::size_t>(out.count++)] = location;
        }
    }
    return out;
}

// §E3.8.2: a dependent substream with chanmape clear is described by acmod and
// lfeon alone. Table 5.8's coded order and Table E2.5's bit order agree for
// every acmod, so turning acmod into a map lets both kinds of substream go
// through the one expansion above. 1+1 is two programs rather than a layout
// and is rejected before this is ever consulted.
[[nodiscard]] constexpr std::uint16_t acmod_map(Acmod acmod, bool lfe) {
    constexpr std::array<std::uint16_t, 8> fbw = {
        kLeftBit | kRightBit,                                             // 1+1 (not a layout)
        kCentreBit,                                                       // 1/0
        kLeftBit | kRightBit,                                             // 2/0
        kLeftBit | kCentreBit | kRightBit,                                // 3/0
        kLeftBit | kRightBit | kCsBit,                                    // 2/1
        kLeftBit | kCentreBit | kRightBit | kCsBit,                       // 3/1
        kLeftBit | kRightBit | kLeftSurroundBit | kRightSurroundBit,      // 2/2
        kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit,  // 3/2
    };
    return static_cast<std::uint16_t>(fbw[static_cast<std::uint8_t>(acmod)] |
                                      (lfe ? kLfeBit : 0));
}

// Canonical 7.1: the dependent replaces the bed's surrounds and adds the two
// rear surrounds. This is the spec's own example (bits 3, 4, 6 with acmod 2/2).
inline constexpr std::uint16_t k71Rear = kLeftSurroundBit | kRightSurroundBit | kLrsRrsBit;
// 5.1.2: two height channels supplementing an untouched 5.1 bed.
inline constexpr std::uint16_t k512Height = kVhlVhrBit;
// Four ceiling channels - front and rear height. Both are PAIR locations, so
// two bits account for four channels.
inline constexpr std::uint16_t kTopQuad = kVhlVhrBit | kLtsRtsBit;

static_assert(k71Rear == 0x1A00, "Table E2.5 bit 0 must be the MSB");
static_assert(channel_count(k71Rear) == 4);
static_assert(channel_count(k512Height) == 2);
static_assert(channel_count(kTopQuad) == 4);

// The expansion must agree with the count, and must place the spec's worked
// example in the order it spells out.
static_assert(expand(k71Rear).count == 4);
static_assert(expand(k71Rear)[0] == Location::kLeftSurround);
static_assert(expand(k71Rear)[1] == Location::kRightSurround);
static_assert(expand(k71Rear)[2] == Location::kLrs);
static_assert(expand(k71Rear)[3] == Location::kRrs);
static_assert(expand(kTopQuad)[0] == Location::kVhl);
static_assert(expand(kTopQuad)[3] == Location::kRts);
static_assert(expand(kLfeBit)[0] == Location::kLfe);
static_assert(expand(kLfe2Bit)[0] == Location::kLfe2);
static_assert(expand(0xFFFF).count == kMaxChannels);

// An acmod's map has to describe exactly the channels that acmod codes, or a
// dependent without a chanmap would land its audio in the wrong speakers.
static_assert([] {
    for (int value = 1; value < 8; ++value) {
        const auto acmod = static_cast<Acmod>(value);
        for (const bool lfe : {false, true}) {
            const int coded = fullbw_channel_count(acmod) + (lfe ? 1 : 0);
            if (channel_count(acmod_map(acmod, lfe)) != coded) {
                return false;
            }
        }
    }
    return true;
}());

// A substream codes at most 3/2 plus LFE (Table 5.8), so ONE dependent adds at
// most five full-bandwidth channels. chanmap does not lift that: §E2.3.1.8
// requires the locations it names to equal the coded channel count, so a pair
// bit spends two coded channels rather than conjuring one. Hence 5.1.4 needs
// four new channels and fits a single dependent, while 7.1.4 needs six and
// cannot - it is the reason kTopQuad rides beside k71Rear in a second
// dependent rather than merging into one.

// --- dynamic channel allocation ---------------------------------------------
//
// A fixed LayoutId only ever hand-picks a few of the combinations Table E2.5
// can express. The general problem underneath - partition an arbitrary set of
// desired locations into a bed and however many dependents it takes - is what
// this section solves, so a caller can ask for anything the format allows
// rather than one of a short hand-picked list.

// Table 5.8 caps a single acmod at 3/2: five full-bandwidth channels, never
// six, whether or not the substream also carries an LFE-type channel.
inline constexpr int kMaxSubstreamFullbw = 5;

// The acmod field always contributes at least one full-bandwidth channel
// (Table 5.8's narrowest mode, 1/0, is one channel; there is no zero-channel
// acmod), so an all-LFE-type substream can never exist: a substream carrying
// LFE2 must also carry at least one real speaker channel. Adding lfeon's one
// LFE-type slot to kMaxSubstreamFullbw is the widest a substream ever gets.
inline constexpr int kMaxSubstreamChannels = kMaxSubstreamFullbw + 1;

enum class AllocationError : std::uint8_t {
    kTooManyChannels,  // §E3.8.2: the request needs more than 16 rendered channels
    kNoBedFit,         // no Table 5.8 acmod's own channels are a subset of the request
    kOrphanLfe2,       // LFE2 was requested with no full-bandwidth channel left to share its substream
};

[[nodiscard]] std::string_view describe(AllocationError error);

// The Table 5.8 acmod/lfeon that code exactly the channels `mask` names, or
// nullopt if no combination does - which happens only when `mask` asks for
// zero full-bandwidth channels (an LFE-type location with no companion) or
// more than five. Two acmods can share a full-bandwidth count (3/1 and 2/2
// both code four), so a fixed preference (documented at the definition)
// breaks the tie; existing named layouts are built to agree with that choice,
// so this is not a free-standing decision, it is what they already assume.
[[nodiscard]] std::optional<std::pair<Acmod, bool>> acmod_for_chanmap(std::uint16_t mask);

// The inverse of name(): the Table E2.5 location a display name (as name()
// prints it, e.g. "Ls", "LFE2") stands for, or nullopt for anything else.
[[nodiscard]] std::optional<Location> parse_location(std::string_view name);

// A concrete, general E-AC-3 channel plan: the independent substream's own
// acmod/lfeon (Table 5.8 - only a dependent may carry a custom chanmap, so
// the bed is never anything but one of its eight modes), and the chanmap each
// dependent carries, in transmission order.
struct ChannelPlan {
    Acmod bed_acmod = Acmod::k2_0;
    bool bed_lfe = false;
    std::vector<std::uint16_t> dependents;
};

// Partitions `locations` - every Table E2.5 location the whole programme
// should render - into a bed and however many dependents the remainder
// needs. The bed is the WIDEST Table 5.8 acmod whose own locations are all
// present in `locations`: only a dependent may customise its channel map, so
// the bed can never be asked to render a location outside the eight Table 5.8
// shapes, and among those that fit, the widest one leaves the least for
// dependents to carry. Everything `locations` asks for that the bed cannot
// express is packed into dependents of at most kMaxSubstreamChannels each.
[[nodiscard]] std::expected<ChannelPlan, AllocationError> allocate(std::uint16_t locations);

}  // namespace chanmap

}  // namespace ac3::eac3
