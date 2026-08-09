#include "ac3/io/elementary.hpp"

#include <bit>

#include "ac3/core/bitreader.hpp"
#include "ac3/encoder/eac3_frame.hpp"  // kBsid, StreamType, chanmap

namespace ac3::io {

namespace {

constexpr int kAc3MaxBsid = 10;   // §5.4.1.3: 8 for the base standard, 10 with annexes
constexpr int kBsidBitOffset = 40;

// Table 5.8 channel positions as chanmap locations, so an E-AC-3 bed and its
// dependents' maps can be unioned in one vocabulary rather than compared as
// two different kinds of thing.
[[nodiscard]] std::uint16_t bed_locations(Acmod acmod, bool lfe) {
    using namespace eac3::chanmap;
    std::uint16_t map = 0;
    switch (acmod) {
        // 1+1 has no Table E2.5 location - Ch1 and Ch2 are independent
        // programmes, not directions - but this function's only consumer
        // wants a channel COUNT, so the same placeholder acmod_map() uses for
        // that reason stands in here too: two bits, for two coded channels.
        case Acmod::kDualMono:
            map = kLeftBit | kRightBit;
            break;
        case Acmod::k1_0:
            map = kCentreBit;
            break;
        case Acmod::k2_0:
            map = kLeftBit | kRightBit;
            break;
        case Acmod::k3_0:
            map = kLeftBit | kCentreBit | kRightBit;
            break;
        case Acmod::k2_1:
            map = kLeftBit | kRightBit | kCsBit;
            break;
        case Acmod::k3_1:
            map = kLeftBit | kCentreBit | kRightBit | kCsBit;
            break;
        case Acmod::k2_2:
            map = kLeftBit | kRightBit | kLeftSurroundBit | kRightSurroundBit;
            break;
        case Acmod::k3_2:
            map = kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit;
            break;
    }
    return static_cast<std::uint16_t>(map | (lfe ? kLfeBit : 0));
}

[[nodiscard]] bool sync_at(std::span<const std::byte> stream, std::size_t offset) {
    return offset + 2 <= stream.size() &&
           std::to_integer<std::uint8_t>(stream[offset]) == 0x0B &&
           std::to_integer<std::uint8_t>(stream[offset + 1]) == 0x77;
}

// --- AC-3 ------------------------------------------------------------------

// Walk bsi far enough to reach lfeon, whose position depends on which of
// cmixlev, surmixlev and dsurmod acmod brought with it (§5.4.2).
std::expected<void, ScanError> read_ac3_bsi(BitReader& r, Acmod& acmod, bool& lfe) {
    r.skip(5 + 3);  // bsid, bsmod
    const auto raw = r.read(3);
    acmod = static_cast<Acmod>(raw);
    if ((raw & 0x1) && raw != 0x1) {
        r.skip(2);  // cmixlev
    }
    if (raw & 0x4) {
        r.skip(2);  // surmixlev
    }
    if (raw == 0x2) {
        r.skip(2);  // dsurmod
    }
    lfe = r.read(1) != 0;
    return r.overflowed() ? std::unexpected(ScanError::kTruncated)
                          : std::expected<void, ScanError>{};
}

std::expected<ScannedStream, ScanError> scan_ac3(std::span<const std::byte> stream) {
    ScannedStream out{.kind = StreamKind::kAc3, .substreams_per_unit = 1};
    std::size_t offset = 0;
    bool first = true;
    while (offset < stream.size()) {
        if (!sync_at(stream, offset) || offset + 5 > stream.size()) {
            return std::unexpected(ScanError::kLostSync);
        }
        // syncinfo: fscod and frmsizecod share byte 4, and together index
        // Table 5.18 for the frame size.
        const auto byte4 = std::to_integer<std::uint32_t>(stream[offset + 4]);
        const auto fscod = byte4 >> 6;
        const auto frmsizecod = byte4 & 0x3F;
        if (fscod == 3 || frmsizecod > 37) {
            return std::unexpected(ScanError::kReservedValue);
        }
        const auto rate = static_cast<SampleRate>(fscod);
        const auto bytes =
            frame_size_bytes(rate, kBitratesKbps[frmsizecod >> 1], (frmsizecod & 1) != 0);
        if (!bytes || offset + *bytes > stream.size()) {
            return std::unexpected(ScanError::kTruncated);
        }
        if (first) {
            BitReader r{stream.subspan(offset)};
            r.skip(kBsidBitOffset);
            Acmod acmod = Acmod::k2_0;
            bool lfe = false;
            if (const auto ok = read_ac3_bsi(r, acmod, lfe); !ok) {
                return std::unexpected(ok.error());
            }
            out.sample_rate = rate;
            out.acmod = acmod;
            out.lfe = lfe;
            out.channels = fullbw_channel_count(acmod) + (lfe ? 1 : 0);
            first = false;
        }
        out.access_units.push_back(stream.subspan(offset, *bytes));
        offset += *bytes;
    }
    return out;
}

// --- E-AC-3 ----------------------------------------------------------------

struct Substream {
    int strmtyp = 0;
    std::size_t bytes = 0;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    std::uint16_t chanmap = 0;  // 0 when chanmape was clear
};

std::expected<Substream, ScanError> read_eac3_substream(std::span<const std::byte> at) {
    BitReader r{at};
    r.skip(16);  // syncword
    Substream s;
    s.strmtyp = static_cast<int>(r.read(2));
    r.skip(3);  // substreamid
    s.bytes = (static_cast<std::size_t>(r.read(11)) + 1) * 2;
    const auto fscod = r.read(2);
    if (fscod == 0x3) {
        // fscod2: the half-rate sample rates, which this project does not
        // produce and cannot name in SampleRate.
        return std::unexpected(ScanError::kReservedValue);
    }
    s.sample_rate = static_cast<SampleRate>(fscod);
    r.skip(2);  // numblkscod
    const auto acmod = r.read(3);
    s.acmod = static_cast<Acmod>(acmod);
    s.lfe = r.read(1) != 0;
    r.skip(5 + 5);  // bsid, dialnorm
    if (r.read(1)) {
        r.skip(8);  // compr
    }
    if (acmod == 0x0) {
        r.skip(5);  // dialnorm2
        if (r.read(1)) {
            r.skip(8);  // compr2
        }
    }
    if (s.strmtyp == static_cast<int>(eac3::StreamType::kDependent)) {
        if (r.read(1)) {  // chanmape
            s.chanmap = static_cast<std::uint16_t>(r.read(16));
        }
    }
    if (r.overflowed()) {
        return std::unexpected(ScanError::kTruncated);
    }
    return s;
}

std::expected<ScannedStream, ScanError> scan_eac3(std::span<const std::byte> stream) {
    ScannedStream out{.kind = StreamKind::kEac3};
    std::size_t offset = 0;
    std::size_t unit_start = 0;
    std::size_t substreams = 0;
    std::uint16_t locations = 0;
    bool first_unit = true;

    const auto close_unit = [&](std::size_t end) {
        if (end > unit_start) {
            out.access_units.push_back(stream.subspan(unit_start, end - unit_start));
            if (first_unit) {
                out.substreams_per_unit = substreams;
                first_unit = false;
            }
        }
    };

    while (offset < stream.size()) {
        if (!sync_at(stream, offset)) {
            return std::unexpected(ScanError::kLostSync);
        }
        const auto sub = read_eac3_substream(stream.subspan(offset));
        if (!sub) {
            return std::unexpected(sub.error());
        }
        if (offset + sub->bytes > stream.size()) {
            return std::unexpected(ScanError::kTruncated);
        }
        // An independent substream begins a new access unit; dependents join
        // the one in progress.
        if (sub->strmtyp == static_cast<int>(eac3::StreamType::kIndependent)) {
            close_unit(offset);
            unit_start = offset;
            substreams = 0;
            if (out.access_units.empty()) {
                out.sample_rate = sub->sample_rate;
                out.acmod = sub->acmod;
                out.lfe = sub->lfe;
                locations = bed_locations(sub->acmod, sub->lfe);
            }
        } else if (first_unit) {
            // §E3.8.2: a dependent's channels overwrite the bed's where they
            // correspond and extend the layout where they do not, so unioning
            // locations - not adding counts - is what gives the rendered
            // channel count.
            locations = static_cast<std::uint16_t>(locations | sub->chanmap);
        }
        ++substreams;
        offset += sub->bytes;
    }
    close_unit(offset);
    if (out.access_units.empty()) {
        return std::unexpected(ScanError::kEmpty);
    }
    out.channels = eac3::chanmap::channel_count(locations);
    return out;
}

}  // namespace

std::string_view describe(ScanError error) {
    switch (error) {
        case ScanError::kEmpty:
            return "no frames in stream";
        case ScanError::kLostSync:
            return "lost sync: expected 0x0B77";
        case ScanError::kUnsupportedBsid:
            return "unsupported bsid (expected AC-3 <= 10 or E-AC-3 16)";
        case ScanError::kReservedValue:
            return "reserved value in syncinfo";
        case ScanError::kTruncated:
            return "stream ends mid-frame";
    }
    return "unknown error";
}

std::expected<ScannedStream, ScanError> scan(std::span<const std::byte> stream) {
    if (stream.size() < 6) {
        return std::unexpected(ScanError::kEmpty);
    }
    if (!sync_at(stream, 0)) {
        return std::unexpected(ScanError::kLostSync);
    }
    // Both formats spend exactly 40 bits before bsid, which is what lets a
    // reader identify the stream without knowing its kind in advance.
    BitReader probe{stream};
    probe.skip(kBsidBitOffset);
    const auto bsid = static_cast<int>(probe.read(5));
    if (bsid <= kAc3MaxBsid) {
        return scan_ac3(stream);
    }
    if (bsid == eac3::kBsid) {
        return scan_eac3(stream);
    }
    return std::unexpected(ScanError::kUnsupportedBsid);
}

}  // namespace ac3::io
