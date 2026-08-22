#pragma once

#include <cstdint>

// Base constants of the Dolby TrueHD (MLP) FBA syntax, transcribed from Dolby
// Laboratories' "Dolby TrueHD (MLP) high-level bitstream description"
// (7 February 2018) - the closest thing to a public standard this bitstream
// has. Every entry cites the section or table it comes from, the same
// discipline core/tables.hpp and core/eac3_tables.hpp already follow for
// ATSC A/52.
//
// This header covers only the framing/metadata syntax that document actually
// specifies (mlp_sync, major_sync_info, channel_meaning). It does NOT cover
// block_data()'s matrixing/prediction/entropy-coding algorithm - neither that
// document nor its ISOBMFF-muxing companion describes it, and unlike A/52,
// no public standard for it exists at all. See docs/concepts/truehd-mlp.md
// for the full account of what is and isn't covered, and where the core
// algorithm will be sourced from instead (academic/patent literature, cited
// the same way, never FFmpeg's source - CONTRIBUTING.md's clean-room rule).

namespace ac3::mlp {

// §2.6/§4.2.1: only the FBA syntax (Blu-ray Disc applications) is
// transcribed here; the FBB syntax used by DVD-Audio is out of scope.
inline constexpr std::uint32_t kFormatSync = 0xF8726FBA;

// §4.2.3: confirms a major sync beyond format_sync alone.
inline constexpr std::uint16_t kSignature = 0xB752;

// §4.6.2 / §4.6.5: mark the final access unit's last substream segment.
inline constexpr std::uint32_t kTerminatorA = 0x348D3;  // v(18)
inline constexpr std::uint16_t kTerminatorB = 0x1234;   // v(13)

// §2.7: "It is the encoder's responsibility to ensure ... rate does not
// exceed peakDataRate where peakDataRate = 18 Mbit/s for FBA streams", with
// the effective rate defined per access unit as size / (input_timing[n+1] -
// input_timing[n]). The minimum decoder FIFO the same section requires
// bounds how far ahead of its presentation time an access unit may be
// delivered.
inline constexpr std::uint32_t kPeakDataRateBitsPerSecond = 18'000'000;
inline constexpr std::uint32_t kMinFifoBufferBytes = 120'000;

// §4.7.2, Table 20: restart_sync_word identifies which substream (0-3) a
// restart header belongs to. Substream 1 may use either value.
inline constexpr std::uint16_t kRestartSyncWordSubstream0 = 0x31EA;
inline constexpr std::uint16_t kRestartSyncWordSubstream1Alt = 0x31EB;
inline constexpr std::uint16_t kRestartSyncWordSubstream2 = 0x31EB;
inline constexpr std::uint16_t kRestartSyncWordSubstream3 = 0x31EC;

// §4.6.6 / §4.8.5: substream_parity and EXTRA_DATA_parity are both an XOR of
// every covered byte, then XORed with this constant - "to force the check to
// fail in the event of the stream consisting entirely of zeroes."
inline constexpr std::uint8_t kParityXorConstant = 0xA9;

// §4.5.1 Table 20 note: substream 1 accepts either restart_sync_word value;
// every other substream has exactly one legal value.
[[nodiscard]] constexpr bool is_restart_sync_word_valid(int substream_index,
                                                         std::uint16_t word) {
    switch (substream_index) {
        case 0: return word == kRestartSyncWordSubstream0;
        case 1: return word == kRestartSyncWordSubstream0 || word == kRestartSyncWordSubstream1Alt;
        case 2: return word == kRestartSyncWordSubstream2;
        case 3: return word == kRestartSyncWordSubstream3;
        default: return false;
    }
}

// §4.2.2, Table 3 - audio_sampling_frequency, u(4). Enumerator values equal
// the 4-bit field's own code so packing needs no lookup table; the two
// families (48 kHz and 44.1 kHz multiples) are not contiguous codes, which is
// why this isn't a plain 0..5 sequence the way ac3::SampleRate is.
enum class SampleRate : std::uint8_t {
    k48000 = 0b0000,
    k96000 = 0b0001,
    k192000 = 0b0010,
    k44100 = 0b1000,
    k88200 = 0b1001,
    k176400 = 0b1010,
};

[[nodiscard]] constexpr std::uint32_t sample_rate_hz(SampleRate sr) {
    switch (sr) {
        case SampleRate::k48000: return 48000;
        case SampleRate::k96000: return 96000;
        case SampleRate::k192000: return 192000;
        case SampleRate::k44100: return 44100;
        case SampleRate::k88200: return 88200;
        case SampleRate::k176400: return 176400;
    }
    return 0;
}

// §2.2: "An audio frame will ... have a duration of 40 multichannel samples
// at 48 kHz, 80 multichannel samples at 96 kHz and 160 multichannel samples
// at 192 kHz" - and the analogous 40/80/160 scaling for the 44.1 kHz family.
// This is the access unit's sample count, not its byte size (that's variable
// - lossless compression has no fixed frame size).
[[nodiscard]] constexpr int samples_per_access_unit(SampleRate sr) {
    switch (sr) {
        case SampleRate::k48000:
        case SampleRate::k44100: return 40;
        case SampleRate::k96000:
        case SampleRate::k88200: return 80;
        case SampleRate::k192000:
        case SampleRate::k176400: return 160;
    }
    return 0;
}

// §4.2.6: the FBA channel ceiling in peak_data_rate's own units (1/16 bit
// per sample period), rounded up - what a single-pass encoder writes when
// the stream's true measured peak isn't known yet. Fits u(15) at every
// rate (6531 at 44.1 kHz is the largest).
[[nodiscard]] constexpr std::uint32_t peak_data_rate_ceiling_16ths(SampleRate sr) {
    const auto hz = sample_rate_hz(sr);
    return static_cast<std::uint32_t>((kPeakDataRateBitsPerSecond * 16ull + hz - 1) / hz);
}

// §4.2.2, Table 4 (2-channel) and the "exactly two channels" cases of Tables
// 5/8 (6-/8-channel content carrying only Main L/R) - all three presentation
// tiers share this same 2-bit content-type vocabulary.
enum class TwoChannelContent : std::uint8_t {
    kStereo = 0b00,
    kLtRt = 0b01,
    kLbinRbin = 0b10,
    kMono = 0b11,
};

// §4.2.2, Tables 6/9: the 6-/8-channel presentation's modifier when Ls/Rs
// (Surround) is the only surround pair present. Table 9's wording is
// identical to Table 6's for the 8-channel case with no other surrounds.
enum class SurroundModifier : std::uint8_t {
    kNotIndicated = 0b00,
    kNotSurroundEncoded = 0b01,       // not Surround EX / Pro Logic IIx / Pro Logic IIz
    kSurroundExOrProLogicIIx = 0b10,
    kProLogicIIz = 0b11,
};

// §4.2.2, Tables 7/10: the channel-assignment bit positions shared by the
// 6-channel presentation (bits 0-4 only, Table 7) and the primary 8-channel
// interpretation (bits 0-12, Table 10, selected when flags bit 11 == 0).
// Table 7 is exactly the first five entries of Table 10, which is why one
// enum serves both tiers - PresentationAssignment below masks to whichever
// field width the caller is packing.
enum class Location : std::uint8_t {
    kMain,            // bit 0:  L/R              (2 ch)
    kCentre,          // bit 1:  C                (1 ch)
    kLfe,             // bit 2:  LFE               (1 ch)
    kSurround,        // bit 3:  Ls/Rs            (2 ch)
    kTopFront,        // bit 4:  Tfl/Tfr          (2 ch) - 6ch's last bit
    kSideScreen,      // bit 5:  Lsc/Rsc          (2 ch) - 8ch (Table 10) only, from here down
    kBack,            // bit 6:  Lb/Rb            (2 ch)
    kBackCentre,      // bit 7:  Cb               (1 ch)
    kTopCentre,       // bit 8:  Tc               (1 ch)
    kSurroundDirect,  // bit 9:  Lsd/Rsd          (2 ch)
    kWide,            // bit 10: Lw/Rw            (2 ch)
    kTopFrontCentre,  // bit 11: Tfc              (1 ch)
    kLfe2,            // bit 12: LFE2             (1 ch)
};

inline constexpr int kSixChannelAssignmentBits = 5;    // Table 7
inline constexpr int kEightChannelAssignmentBits = 13; // Table 10

// 1 for the mono slots (C, Cb, Tc, Tfc, LFE, LFE2), 2 for every paired slot.
[[nodiscard]] constexpr int channel_width(Location loc) {
    switch (loc) {
        case Location::kCentre:
        case Location::kLfe:
        case Location::kBackCentre:
        case Location::kTopCentre:
        case Location::kTopFrontCentre:
        case Location::kLfe2: return 1;
        default: return 2;
    }
}

// A bitmask over Location, one bit per slot, in the field's own bit order
// (bit 0 == Location::kMain). Callers build one with `|=` against
// `1u << static_cast<int>(location)` and pass it to bits()/channel_count()
// below alongside the field width (kSixChannelAssignmentBits or
// kEightChannelAssignmentBits) their presentation tier uses.
using Assignment = std::uint16_t;

[[nodiscard]] constexpr bool has(Assignment assignment, Location loc) {
    return (assignment & (Assignment{1} << static_cast<int>(loc))) != 0;
}

[[nodiscard]] constexpr Assignment with(Assignment assignment, Location loc) {
    return static_cast<Assignment>(assignment | (Assignment{1} << static_cast<int>(loc)));
}

// Table 7's own note: "Bit allocations shall not cause the number of
// channels allocated to exceed six" (and Table 10's equivalent note: eight).
// This is the count that check is against - the caller compares it to the
// presentation tier's own ceiling.
[[nodiscard]] constexpr int channel_count(Assignment assignment, int field_width) {
    int total = 0;
    for (int bit = 0; bit < field_width; ++bit) {
        if ((assignment & (Assignment{1} << bit)) != 0) {
            total += channel_width(static_cast<Location>(bit));
        }
    }
    return total;
}

// §4.2.2, Table 11: the alternate 8-channel interpretation, selected by
// flags bit 11 == 1, "only when there are more than six channels in the
// 8-channel presentation." A distinct, smaller bit field - Table 11's bit 4
// means Tsl/Tsr where Table 10's bit 4 means Tfl/Tfr, so this is modelled
// separately from Location/Assignment rather than aliased onto it.
struct AlternateEightChannelAssignment {
    bool main = true;      // bit 0: L/R    (2 ch)
    bool centre = false;   // bit 1: C      (1 ch)
    bool lfe = false;      // bit 2: LFE    (1 ch)
    bool surround = false; // bit 3: Ls/Rs  (2 ch)
    bool top_side = false; // bit 4: Tsl/Tsr (2 ch)

    [[nodiscard]] constexpr std::uint16_t bits() const {
        return static_cast<std::uint16_t>((main ? 0b00001 : 0) | (centre ? 0b00010 : 0) |
                                           (lfe ? 0b00100 : 0) | (surround ? 0b01000 : 0) |
                                           (top_side ? 0b10000 : 0));
    }
    [[nodiscard]] constexpr int channel_count() const {
        return (main ? 2 : 0) + (centre ? 1 : 0) + (lfe ? 1 : 0) + (surround ? 2 : 0) +
               (top_side ? 2 : 0);
    }
};

}  // namespace ac3::mlp
