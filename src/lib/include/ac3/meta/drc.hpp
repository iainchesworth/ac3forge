#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "ac3/core/tables.hpp"

// Dynamic range metadata — A/52 §7.7.
//
// Two control signals with genuinely different jobs, so this file generates
// them from different measurements rather than deriving one from the other:
//
//   dynrng (§7.7.1) sits in audblk, so it resolves to a block (5.3 ms). Its
//   job is "subjectively pleasing dynamic range reduction" — an RMS-detected
//   compressor riding a static curve anchored at dialogue level, with attack
//   and release slow enough that the gain itself is inaudible.
//
//   compr (§7.7.2) sits in bsi, so it resolves only to a syncframe (32 ms).
//   Its job is an assured peak ceiling in the MONO DOWNMIX, for links that
//   overmodulate rather than merely sound loud. §7.7.2.1 is explicit that
//   dynrng gives "no assurance that it will control instantaneous signal
//   peaks", which is why compr cannot be a rescaled dynrng: it is driven by
//   the true peak of the §7.8 downmix, not by programme RMS, and it attacks
//   instantaneously, because a ceiling met one frame late is not a ceiling.

namespace ac3::meta {

namespace detail {

// 2^e for the small exponents these formats use. std::exp2 is not constexpr,
// and repeated doubling is exact in binary floating point anyway.
[[nodiscard]] constexpr double pow2(int e) {
    double value = 1.0;
    for (; e > 0; --e) {
        value *= 2.0;
    }
    for (; e < 0; ++e) {
        value *= 0.5;
    }
    return value;
}

}  // namespace detail

// --- the two wire formats --------------------------------------------------

// §7.7.1: dynrng is XXX.YYYYY. X is a 3-bit SIGNED integer (−4..3) worth
// (X+1) × 6.02 dB; Y is a 5-bit unsigned fraction read with an implicit
// leading one, 0.1YYYYY, hence (32+Y)/64 ∈ [1/2, 63/64]. So
//     gain = 2^(X+1) × (32+Y)/64,
// spanning +23.95 dB down to −24.08 dB.
[[nodiscard]] constexpr double dynrng_gain(std::uint8_t word) {
    const int x = static_cast<int>(word >> 5) - ((word & 0x80) != 0 ? 8 : 0);
    const int y = static_cast<int>(word & 0x1F);
    return detail::pow2(x + 1) * (static_cast<double>(32 + y) / 64.0);
}

// §7.7.2: compr is XXXX.YYYY — twice the range at half the resolution. X is a
// 4-bit signed integer (−8..7), Y a 4-bit fraction 0.1YYYY, so
//     gain = 2^(X+1) × (16+Y)/32,
// spanning +47.89 dB down to −48.16 dB.
[[nodiscard]] constexpr double compr_gain(std::uint8_t word) {
    const int x = static_cast<int>(word >> 4) - ((word & 0x80) != 0 ? 16 : 0);
    const int y = static_cast<int>(word & 0x0F);
    return detail::pow2(x + 1) * (static_cast<double>(16 + y) / 32.0);
}

// '0000 0000' is X = 0 and Y = 0 in both formats: 2 × 1/2 = unity. §7.7.1
// names it directly as what an encoder applying no compression sends.
inline constexpr std::uint8_t kDynrngUnity = 0x00;
inline constexpr std::uint8_t kComprUnity = 0x00;
static_assert(dynrng_gain(kDynrngUnity) == 1.0);
static_assert(compr_gain(kComprUnity) == 1.0);

// The representable extremes, as the spec states them.
static_assert(dynrng_gain(0x7F) == 15.75);      // X = 3, Y = 31: +23.95 dB
static_assert(dynrng_gain(0x80) == 0.0625);     // X = −4, Y = 0: −24.08 dB
static_assert(compr_gain(0x7F) == 248.0);       // X = 7, Y = 15: +47.89 dB
static_assert(compr_gain(0x80) == 1.0 / 256.0); // X = −8, Y = 0: −48.16 dB

// Nearest representable word for a gain in dB. Nearest is measured on the
// LINEAR gain, because that is where both formats quantise uniformly; the
// difference from nearest-in-dB is under a thousandth of a dB.
[[nodiscard]] std::uint8_t encode_dynrng(double gain_db);
[[nodiscard]] std::uint8_t encode_compr(double gain_db);

// The largest representable gain that does NOT EXCEED gain_db. Rounding to
// nearest can round up by half a step — 0.14 dB for compr — and §7.7.2 exists
// to give "an assured upper limit of instantaneous peak reproduced signal
// level". A ceiling exceeded by 0.14 dB is not assured, so the heavy
// compressor rounds down and gives up a fraction of a dB of level instead.
// dynrng has no such promise to keep and takes the nearest value.
//
// A gain_db above the format's maximum still clamps to the maximum: that
// exceeds the request, but the alternative is to mute the programme.
[[nodiscard]] std::uint8_t encode_compr_at_most(double gain_db);

[[nodiscard]] double to_db(double linear_gain);

// --- the compression characteristic ---------------------------------------

// A/52 fixes the wire format and the intent, never the curve: §7.7.1.1 leaves
// the compression characteristic to the program provider. These are the
// conventional Dolby DRC profiles, written as one continuous piecewise-linear
// static curve.
//
// Levels are dBFS on a programme normalised so dialogue sits at −31 dBFS,
// which is why every null band starts at −31 or below. The encoder performs
// that normalisation from dialnorm, so a profile behaves the same whatever
// level the source was mastered at — that is the whole point of anchoring
// compression to dialogue rather than to full scale.
//
// The boost region is stated as a ratio plus a ceiling rather than as an
// explicit lower edge: the published tables give the edge too, but deriving
// it (edge = null_low − max_boost × boost_ratio) is the only way to keep the
// curve continuous there, and a step in a gain curve is audible.
struct Profile {
    double null_low_db = -31.0;      // unity band, low edge
    double null_high_db = -26.0;     // unity band, high edge
    double boost_ratio = 2.0;        // n:1 upward expansion below the null band
    double max_boost_db = 6.0;       // ceiling on the boost
    double early_cut_ratio = 2.0;    // n:1 immediately above the null band
    double early_cut_end_db = -16.0; // where the early cut hands over
    double cut_ratio = 20.0;         // n:1 above that — effectively a limiter
    // A/52 says nothing about timing either. 10 ms is short enough to catch a
    // transient inside two 5.3 ms blocks; a release near a second is what
    // keeps the gain from pumping on speech gaps.
    double attack_ms = 10.0;
    double release_ms = 1000.0;
};

enum class ProfileId : std::uint8_t {
    kFilmStandard,
    kFilmLight,
    kMusicStandard,
    kMusicLight,
    kSpeech,
};

// Derived boost edges, for checking against the published tables:
//   film standard  −31 − 6×2  = −43     music standard −31 − 12×2 = −55
//   film light     −41 − 6×2  = −53     music light    −41 − 12×2 = −65
[[nodiscard]] constexpr Profile profile(ProfileId id) {
    switch (id) {
        case ProfileId::kFilmStandard:
            return {};  // the struct's defaults ARE film standard
        case ProfileId::kFilmLight:
            return {.null_low_db = -41.0,
                    .null_high_db = -21.0,
                    .early_cut_end_db = -11.0};
        case ProfileId::kMusicStandard:
            return {.max_boost_db = 12.0};
        case ProfileId::kMusicLight:
            return {.null_low_db = -41.0,
                    .null_high_db = -21.0,
                    .max_boost_db = 12.0,
                    .early_cut_end_db = -11.0};
        case ProfileId::kSpeech:
            // The gentler 5:1 boost and higher ceiling suit a source whose
            // quiet passages are still meant to be intelligible.
            return {.boost_ratio = 5.0, .max_boost_db = 15.0};
    }
    return {};
}

[[nodiscard]] constexpr std::string_view profile_name(ProfileId id) {
    switch (id) {
        case ProfileId::kFilmStandard: return "film-standard";
        case ProfileId::kFilmLight: return "film-light";
        case ProfileId::kMusicStandard: return "music-standard";
        case ProfileId::kMusicLight: return "music-light";
        case ProfileId::kSpeech: return "speech";
    }
    return "";
}

// Names accepted on the command line, in ProfileId order.
inline constexpr std::string_view kProfileNames =
    "film-standard | film-light | music-standard | music-light | speech";

[[nodiscard]] bool parse_profile(std::string_view name, ProfileId& out);

// The static curve: gain in dB for a dialogue-referenced level in dBFS.
// Monotonically non-increasing, continuous, and exactly zero across the null
// band — a signal at dialogue level is left alone (§7.7.1.1).
[[nodiscard]] constexpr double static_gain_db(const Profile& p, double level_db) {
    if (level_db < p.null_low_db) {
        const double boost = (p.null_low_db - level_db) / p.boost_ratio;
        return boost < p.max_boost_db ? boost : p.max_boost_db;
    }
    if (level_db <= p.null_high_db) {
        return 0.0;
    }
    if (level_db <= p.early_cut_end_db) {
        return -(level_db - p.null_high_db) / p.early_cut_ratio;
    }
    const double early = (p.early_cut_end_db - p.null_high_db) / p.early_cut_ratio;
    return -early - (level_db - p.early_cut_end_db) / p.cut_ratio;
}

// --- the generators --------------------------------------------------------

// Programme level of one audio block: the unweighted power sum across the
// spans given, in dBFS. Callers pass the full-bandwidth channels only — LFE
// is excluded here for the same reason BS.1770 excludes it, that a subwoofer
// channel's energy is not proportional to what the programme sounds like.
// Returns a large negative number for digital silence rather than −inf.
[[nodiscard]] double level_dbfs(std::span<const std::span<const float>> channels);

// One dynrng word per audio block. State carries across blocks AND frames:
// the smoothing filter has no idea where a syncframe boundary is, and it must
// not, or every 32 ms the gain would jump.
class RangeController {
public:
    RangeController(const Profile& profile, SampleRate rate);

    // level: the block's programme level in dBFS, from level_dbfs() above.
    // dialnorm re-references it onto the profile's −31 dBFS dialogue anchor.
    [[nodiscard]] std::uint8_t next(double level, int dialnorm);

    [[nodiscard]] double gain_db() const { return gain_db_; }

private:
    Profile profile_;
    double attack_coeff_;
    double release_coeff_;
    double gain_db_ = 0.0;
};

// §7.7.2. Every field is about the ceiling, because the ceiling is the only
// thing compr promises.
struct HeavyConfig {
    // Where dialogue lands after heavy compression. −20 dBFS against a
    // dialnorm of 31 is the classic RF-mode +11 dB line-up.
    double dialogue_target_dbfs = -20.0;
    // The promise: the §7.8 mono downmix will not exceed this.
    //
    // Nominal, and unavoidably so. It is measured on the encoder's INPUT
    // downmix, while the ceiling a listener meets applies to a decoder's
    // reconstruction, and the two differ by the coding error - a few
    // hundredths of a dB at useful rates, more at 32 kbit/s. The encoder
    // cannot close that gap without decoding its own output. Hence the default
    // sits a little below full scale rather than at it: the point of §7.7.2 is
    // that an RF modulator does not overmodulate, and half a dB of real
    // headroom serves that better than an exact bound on the wrong signal.
    double peak_ceiling_dbfs = -0.5;
    // How fast the gain is allowed back up once a peak has passed. Downward
    // movement is not rate-limited at all — see the class comment.
    double release_db_per_second = 20.0;
};

// A limiter, not a compressor. Attack is instantaneous because the ceiling is
// a guarantee about instantaneous peaks and this control signal only updates
// once per 32 ms frame: a peak that arrives in a frame must be caught by that
// frame's word or it is not caught at all.
class HeavyCompressor {
public:
    HeavyCompressor(const HeavyConfig& config, SampleRate rate);

    // peak: the frame's true peak of the §7.8 mono downmix, in dBFS.
    [[nodiscard]] std::uint8_t next(double peak, int dialnorm);

    [[nodiscard]] double gain_db() const { return gain_db_; }

private:
    HeavyConfig config_;
    double release_step_db_;
    double gain_db_ = 0.0;
    bool primed_ = false;
};

}  // namespace ac3::meta
