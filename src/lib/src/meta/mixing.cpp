#include "ac3/meta/mixing.hpp"

#include <algorithm>
#include <cmath>

#include "ac3/meta/drc.hpp"  // to_db

namespace ac3::meta {

namespace {

// Where each acmod's channels sit in the coded order of Table 5.8. −1 means
// the layout has no such channel.
struct Layout {
    int left = -1;
    int centre = -1;
    int right = -1;
    int surround = -1;        // the single surround of 2/1 and 3/1
    int left_surround = -1;
    int right_surround = -1;
};

Layout layout_of(Acmod acmod) {
    switch (acmod) {
        case Acmod::k1_0: return {.centre = 0};
        case Acmod::k2_0: return {.left = 0, .right = 1};
        case Acmod::k3_0: return {.left = 0, .centre = 1, .right = 2};
        case Acmod::k2_1: return {.left = 0, .right = 1, .surround = 2};
        case Acmod::k3_1: return {.left = 0, .centre = 1, .right = 2, .surround = 3};
        case Acmod::k2_2:
            return {.left = 0, .right = 1, .left_surround = 2, .right_surround = 3};
        case Acmod::k3_2:
            return {.left = 0, .centre = 1, .right = 2, .left_surround = 3,
                    .right_surround = 4};
        case Acmod::kDualMono:
            // 1+1 is two programmes, not a layout; §7.8's dual-mono branch
            // depends on which programme the listener asked for, so there is
            // no single set of coefficients to return.
            return {};
    }
    return {};
}

// §7.8.1: "Normalization is achieved by attenuating all downmix coefficients
// equally, such that the sum of coefficients used to create any single output
// channel never exceeds 1."
//
// ATTENUATING, and only to satisfy a bound. A sum already at or below 1 is left
// alone rather than scaled up to 1: the clause exists to prevent overload, not
// to equalise gain. It matters for a 1/0 source folded to two speakers, where
// scaling up would put the centre channel at unity in BOTH outputs and so add
// 3 dB of acoustic power that was not in the programme.
void normalize(std::array<double, 5>& coeffs) {
    double sum = 0.0;
    for (const double c : coeffs) {
        sum += c;
    }
    if (sum <= 1.0 || sum <= 0.0) {
        return;
    }
    for (double& c : coeffs) {
        c /= sum;
    }
}

}  // namespace

DownmixCoefficients stereo_downmix(Acmod acmod, double clev, double slev) {
    const Layout layout = layout_of(acmod);
    DownmixCoefficients out;
    const auto set = [&](int index, double left, double right) {
        if (index < 0) {
            return;
        }
        out.left[static_cast<std::size_t>(index)] = left;
        out.right[static_cast<std::size_t>(index)] = right;
    };

    // "route input_chan[i] into output_chan[i]" for the channels that exist in
    // both, then the mixes §7.8's output_nfront == 2 branch adds.
    set(layout.left, 1.0, 0.0);
    set(layout.right, 0.0, 1.0);
    // A 1/0 source has no left or right at all: its centre becomes both, at
    // −3 dB, which normalisation then lifts back to unity.
    set(layout.centre, layout.left < 0 ? level::kMinus3dB : clev,
        layout.left < 0 ? level::kMinus3dB : clev);
    // A single coded surround with no surround loudspeakers spreads across
    // both fronts at slev − 3 dB; a coded pair keeps its sides.
    set(layout.surround, slev * level::kMinus3dB, slev * level::kMinus3dB);
    set(layout.left_surround, slev, 0.0);
    set(layout.right_surround, 0.0, slev);

    normalize(out.left);
    normalize(out.right);
    return out;
}

std::array<double, 5> mono_downmix(Acmod acmod, double clev, double slev) {
    const Layout layout = layout_of(acmod);
    std::array<double, 5> out{};
    const auto set = [&](int index, double value) {
        if (index >= 0) {
            out[static_cast<std::size_t>(index)] = value;
        }
    };

    // §7.8's output_mode == 1/0 branch, term by term.
    set(layout.left, level::kMinus3dB);
    set(layout.right, level::kMinus3dB);
    // "mix center into center using clev and +3 dB gain" only when the source
    // has three front channels; a 1/0 source routes its centre straight
    // through.
    set(layout.centre, layout.left < 0 ? 1.0 : clev * level::kPlus3dB);
    set(layout.surround, slev * level::kMinus3dB);
    set(layout.left_surround, slev * level::kMinus3dB);
    set(layout.right_surround, slev * level::kMinus3dB);

    normalize(out);
    return out;
}

double mono_downmix_peak_dbfs(std::span<const std::array<double, 256>> history,
                              std::span<const std::span<const float>> channels, Acmod acmod,
                              double clev, double slev) {
    const auto coeffs = mono_downmix(acmod, clev, slev);
    const int nfchans = fullbw_channel_count(acmod);
    const auto usable =
        std::min(static_cast<std::size_t>(nfchans), channels.size());

    double peak = 0.0;
    // The history block first, then the frame: one continuous signal as far as
    // the peak is concerned.
    if (!history.empty()) {
        for (std::size_t n = 0; n < history[0].size(); ++n) {
            double sum = 0.0;
            for (std::size_t ch = 0; ch < usable && ch < history.size(); ++ch) {
                sum += coeffs[ch] * history[ch][n];
            }
            peak = std::max(peak, std::abs(sum));
        }
    }
    std::size_t length = 0;
    for (std::size_t ch = 0; ch < usable; ++ch) {
        length = std::max(length, channels[ch].size());
    }
    for (std::size_t n = 0; n < length; ++n) {
        double sum = 0.0;
        for (std::size_t ch = 0; ch < usable; ++ch) {
            if (n < channels[ch].size()) {
                sum += coeffs[ch] * static_cast<double>(channels[ch][n]);
            }
        }
        peak = std::max(peak, std::abs(sum));
    }
    return to_db(peak);
}

double mono_downmix_peak_dbfs(std::span<const std::span<const float>> channels, Acmod acmod,
                              double clev, double slev) {
    return mono_downmix_peak_dbfs({}, channels, acmod, clev, slev);
}

}  // namespace ac3::meta
