#include "ac3/meta/drc.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ac3::meta {

namespace {

// Both formats are a floating-point number: a power-of-two exponent and a
// mantissa with an implicit leading one. std::frexp splits a double the same
// way, so quantising is a rounding of the mantissa and a clamp of the
// exponent — no search, and exact at every representable point.
//
// mantissa_bits is 5 for dynrng and 4 for compr; exp_low/exp_high bound the
// SIGNED exponent field X. round_down truncates the mantissa instead of
// rounding it, which is what encode_compr_at_most needs.
std::uint8_t quantize(double gain_db, int mantissa_bits, int exp_low, int exp_high,
                      bool round_down = false) {
    const double scale = static_cast<double>(1 << mantissa_bits);
    const double implicit = scale;  // the leading one, in mantissa units
    const double gain = std::pow(10.0, gain_db / 20.0);

    int exponent = 0;
    // frexp gives gain = fraction * 2^exponent with fraction in [0.5, 1); the
    // stored mantissa (implicit+Y)/(2*scale) covers exactly [0.5, 1), so the
    // stored exponent field is X = exponent - 1.
    const double fraction = std::frexp(gain, &exponent);
    int x = exponent - 1;
    const double mantissa = fraction * 2.0 * scale - implicit;
    // Truncating cannot overflow the field - fraction < 1 bounds the mantissa
    // below scale - so only the rounding path needs a renormalisation. The
    // epsilon keeps an exactly representable gain from truncating a code low
    // on the last bit of the pow/frexp round trip.
    auto y = static_cast<int>(round_down ? std::floor(mantissa + 1e-9)
                                         : static_cast<double>(std::lround(mantissa)));
    if (y > static_cast<int>(scale) - 1) {
        // The mantissa rounded up past its top: renormalise into the next
        // exponent rather than emitting an out-of-range field.
        ++x;
        y = 0;
    }
    if (x < exp_low) {
        x = exp_low;
        y = 0;
    } else if (x > exp_high) {
        x = exp_high;
        y = static_cast<int>(scale) - 1;
    }
    const int field = x < 0 ? x + 2 * (exp_high + 1) : x;
    return static_cast<std::uint8_t>((field << mantissa_bits) | y);
}

}  // namespace

std::uint8_t encode_dynrng(double gain_db) {
    return quantize(gain_db, 5, -4, 3);
}

std::uint8_t encode_compr(double gain_db) {
    return quantize(gain_db, 4, -8, 7);
}

std::uint8_t encode_compr_at_most(double gain_db) {
    return quantize(gain_db, 4, -8, 7, /*round_down=*/true);
}

double to_db(double linear_gain) {
    // −200 dB stands in for silence: every consumer here goes on to compare
    // or clamp, and −inf poisons both.
    return linear_gain > 1e-10 ? 20.0 * std::log10(linear_gain) : -200.0;
}

double level_dbfs(std::span<const std::span<const float>> channels) {
    double power = 0.0;
    for (const auto& channel : channels) {
        if (channel.empty()) {
            continue;
        }
        double sum = 0.0;
        for (const float sample : channel) {
            const double value = static_cast<double>(sample);
            sum += value * value;
        }
        power += sum / static_cast<double>(channel.size());
    }
    return power > 1e-20 ? 10.0 * std::log10(power) : -200.0;
}

double channel_peak_dbfs(std::span<const double> history, std::span<const float> samples) {
    double peak = 0.0;
    for (const double value : history) {
        peak = std::max(peak, std::abs(value));
    }
    for (const float value : samples) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    return to_db(peak);
}

namespace {

// One-pole smoothing coefficient for a time constant, at the block rate. A
// block is 256 samples whatever the sample rate, so 32 kHz blocks last 8 ms
// against 48 kHz's 5.3 ms and the same profile still yields the same
// millisecond behaviour.
double smoothing_coeff(double time_ms, SampleRate rate) {
    const double block_ms =
        1000.0 * kSamplesPerBlock / static_cast<double>(sample_rate_hz(rate));
    if (time_ms <= 0.0) {
        return 1.0;
    }
    return 1.0 - std::exp(-block_ms / time_ms);
}

}  // namespace

RangeController::RangeController(const Profile& config, SampleRate rate)
    : profile_(config),
      attack_coeff_(smoothing_coeff(config.attack_ms, rate)),
      release_coeff_(smoothing_coeff(config.release_ms, rate)) {}

std::uint8_t RangeController::next(double level, int dialnorm) {
    // §7.6: dialnorm says dialogue sits dialnorm dB below full scale. Shifting
    // the measured level by (dialnorm − 31) puts dialogue at the −31 dBFS the
    // profile curves are drawn against, so a quiet master and a hot one get
    // the same treatment.
    const double referenced = level + static_cast<double>(dialnorm) - 31.0;
    const double target = static_gain_db(profile_, referenced);
    // Attack is the direction that takes gain DOWN — a rising signal needing
    // more attenuation. Getting these the wrong way round is inaudible on a
    // steady tone and obvious on speech.
    const double coeff = target < gain_db_ ? attack_coeff_ : release_coeff_;
    gain_db_ += (target - gain_db_) * coeff;
    return encode_dynrng(gain_db_);
}

HeavyCompressor::HeavyCompressor(const HeavyConfig& config, SampleRate rate)
    : config_(config) {
    const double frame_s =
        static_cast<double>(kSamplesPerFrame) / static_cast<double>(sample_rate_hz(rate));
    release_step_db_ = config.release_db_per_second * frame_s;
}

std::uint8_t HeavyCompressor::next(double peak, int dialnorm) {
    // Two constraints, and the tighter one wins. The make-up brings dialogue
    // to the line-up level heavy compression exists to hit; the ceiling is the
    // guarantee, so it can only ever reduce the make-up, never raise it.
    const double makeup = static_cast<double>(dialnorm) + config_.dialogue_target_dbfs;
    const double allowed = config_.peak_ceiling_dbfs - peak;
    const double target = std::min(makeup, allowed);

    if (!primed_) {
        gain_db_ = target;
        primed_ = true;
    } else if (target <= gain_db_) {
        gain_db_ = target;  // instantaneous attack: the ceiling is a promise
    } else {
        gain_db_ = std::min(target, gain_db_ + release_step_db_);
    }
    return encode_compr_at_most(gain_db_);
}

bool parse_profile(std::string_view name, ProfileId& out) {
    constexpr std::array<ProfileId, 5> ids = {
        ProfileId::kFilmStandard, ProfileId::kFilmLight, ProfileId::kMusicStandard,
        ProfileId::kMusicLight, ProfileId::kSpeech,
    };
    for (const auto id : ids) {
        if (profile_name(id) == name) {
            out = id;
            return true;
        }
    }
    return false;
}

}  // namespace ac3::meta
