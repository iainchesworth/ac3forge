#include "ac3/oba/atmos.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/internal/profiling.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/joc_tables.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

namespace ac3::oba {

namespace {

constexpr int kChannels = joc::kNumChannels5X;

// AC-3 codes 3/2 as L, C, R, Ls, Rs (Table 5.8) and spatial::PanGains follows
// it. JOC indexes its downmix as L, R, C, Ls, Rs (Table 53). C and R swap.
constexpr std::array<int, kChannels> kAc3FromJoc = {0, 2, 1, 3, 4};

// Regularization for the reconstruction solve, relative to the downmix's own
// energy. Without it, objects that landed on the same bed channels make the
// covariance singular and the matrix runs away to values the quantizer cannot
// express; with it, the solve degrades into splitting their shared energy in
// proportion to their power, which is the right answer to an unanswerable
// question.
constexpr double kRelativeRegularization = 1e-3;
constexpr double kAbsoluteFloor = 1e-20;

// Gauss-Jordan with partial pivoting over a 5x5. Small, symmetric and
// positive definite once regularized, so this is never the interesting part.
[[nodiscard]] bool invert(std::array<std::array<double, kChannels>, kChannels>& m) {
    std::array<std::array<double, kChannels>, kChannels> inverse{};
    for (int i = 0; i < kChannels; ++i) {
        inverse[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = 1.0;
    }
    for (int col = 0; col < kChannels; ++col) {
        int pivot = col;
        for (int row = col + 1; row < kChannels; ++row) {
            if (std::abs(m[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)]) >
                std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(col)])) {
                pivot = row;
            }
        }
        // pivot is only ever col itself or a row from the loop above, both
        // bounded to [0, kChannels) by their own for-loop conditions - so
        // pivot is always in range here. MSVC /analyze's C28020 doesn't
        // track that a variable's bound is inherited from the two loop
        // variables it was assigned from, and flags the std::array subscript
        // below as unproven. #pragma warning(suppress: 28020) would silence
        // /analyze too, but it is not a portable pragma - GCC/clang both
        // treat an unrecognized #pragma as -Wunknown-pragmas, and this
        // project builds with -Werror, so emitting it here would fail every
        // non-MSVC leg. The C28020 alert is dismissed separately with this
        // same justification instead.
        assert(pivot >= 0 && pivot < kChannels);
        if (std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(col)]) <
            kAbsoluteFloor) {
            return false;
        }
        std::swap(m[static_cast<std::size_t>(pivot)], m[static_cast<std::size_t>(col)]);
        std::swap(inverse[static_cast<std::size_t>(pivot)], inverse[static_cast<std::size_t>(col)]);

        const double scale =
            1.0 / m[static_cast<std::size_t>(col)][static_cast<std::size_t>(col)];
        for (int k = 0; k < kChannels; ++k) {
            m[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)] *= scale;
            inverse[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)] *= scale;
        }
        for (int row = 0; row < kChannels; ++row) {
            if (row == col) {
                continue;
            }
            const double factor =
                m[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
            if (factor == 0.0) {
                continue;
            }
            for (int k = 0; k < kChannels; ++k) {
                m[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] -=
                    factor * m[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)];
                inverse[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] -=
                    factor * inverse[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)];
            }
        }
    }
    m = inverse;
    return true;
}

}  // namespace

// Energy of one object per JOC parameter band, over the whole frame.
//
// Nothing about this is normative. TS 103 420 specifies the bitstream and what
// a decoder does with it; how an encoder arrives at the numbers is entirely
// its own business, and §7's QMF is the DECODER's analysis, not a required
// encoder one. So this reuses the transform the encoder already runs on every
// channel: the 512-sample MDCT gives 256 bins across the same band the QMF
// splits into 64 subbands, so four bins fall in each subband exactly, and
// Table 54 groups the subbands into parameter bands from there.
//
// Given external linkage (declared in atmos.hpp) rather than staying
// anonymous-namespace-local like `invert` above it: kernel-level
// benchmarking needs to call this in isolation. It is not part of the
// object-encoding API AtmosEncoder exposes and no caller outside this
// library should need it directly.
void band_energy(std::span<const float> signal, std::span<const std::uint8_t, 64> mapping,
                 std::span<double> out, bool fast) {
    AC3_ZONE_SCOPED_N("band_energy");
    std::ranges::fill(out, 0.0);
    // The frame's own six blocks, without the previous frame's overlap: this
    // is an energy estimate, not a transform that has to reconstruct.
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        std::array<double, 512> time{};
        for (int n = 0; n < 512; ++n) {
            const int index = block * 256 + n - 256;
            time[static_cast<std::size_t>(n)] =
                index < 0 ? 0.0
                          : static_cast<double>(signal[static_cast<std::size_t>(index)]);
        }
        std::array<double, 512> windowed{};
        apply_analysis_window(time, windowed);
        std::array<double, 256> coeffs{};
        mdct512_forward(windowed, coeffs, fast);
        for (int bin = 0; bin < 256; ++bin) {
            const auto band = mapping[static_cast<std::size_t>(bin / 4)];
            out[band] += coeffs[static_cast<std::size_t>(bin)] *
                         coeffs[static_cast<std::size_t>(bin)];
        }
    }
}

AtmosEncoder::AtmosEncoder(const AtmosConfig& config, int objects)
    : config_(config),
      objects_(objects),
      program_{.dynamic_only = true, .lfe = true, .dynamic_objects = objects},
      encoder_(eac3::AccessUnitConfig{
          .independent = {.sample_rate = config.sample_rate,
                          .bitrate_kbps = config.bitrate_kbps,
                          .acmod = Acmod::k3_2,
                          .lfe = true,
                          .dialnorm = config.dialnorm,
                          .fast_mdct = config.fast_mdct,
                          // §8.3.2.2: the object count, bed included.
                          .oba_complexity_index = object_count(program_)}}),
      gains_(static_cast<std::size_t>(objects)),
      lfe_gains_(static_cast<std::size_t>(objects), 0.0),
      bed_(6, std::vector<float>(kSamplesPerFrame)) {
    // §5.6.4.8 orders the program's objects bed-first, and the bed here is the
    // LFE alone - so program object 0 is the LFE and the dynamic objects
    // follow it. §6.3.2.2 bypasses the LFE rather than matrixing it, so it
    // costs no JOC output and JOC object j is program object j + 1.
    params_.objects = joc_object_count(program_);
    params_.channels = kChannels;
    params_.num_bands_idx = config.num_bands_idx;
    params_.fine_quant = config.fine_quant;
    params_.matrix.assign(params_.coefficient_count(), 0.0);
}

std::expected<eac3::AccessUnit, FrameError> AtmosEncoder::encode_frame(
    std::span<const std::span<const float>> objects,
    std::span<const ObjectPlacement> placement) {
    AC3_ZONE_SCOPED_N("AtmosEncoder::encode_frame");
    assert(static_cast<int>(objects.size()) == objects_);
    assert(static_cast<int>(placement.size()) == objects_);

    const auto count = static_cast<std::size_t>(objects_);
    const int bands = params_.bands();
    const auto& mapping =
        joc::kSubbandToBand[static_cast<std::size_t>(config_.num_bands_idx)];

    // --- 1. Where each object ends the frame ------------------------------
    // Two matrices come out of this and they are deliberately different. The
    // BED gets the panning gains times the object's gain, because that is the
    // mix. The reconstruction solve gets the panning gains alone, and the
    // object's gain is folded into its power instead - so what JOC hands back
    // is the object already at its intended level and object_gain can stay at
    // 0 dB. The alternative, reconstructing the raw essence and sending the
    // gain as metadata, would push it through Table 19's 1 dB steps for no
    // reason.
    std::vector<std::array<double, kChannels>> pan(count);
    std::vector<std::array<double, kChannels>> target(count);
    std::vector<double> scale(count);
    std::vector<double> target_lfe(count);
    for (std::size_t object = 0; object < count; ++object) {
        const auto& place = placement[object];
        const auto ring = spatial::pan_room(place.position.x, place.position.y);
        for (int channel = 0; channel < kChannels; ++channel) {
            const double g =
                ring[static_cast<std::size_t>(kAc3FromJoc[static_cast<std::size_t>(channel)])];
            pan[object][static_cast<std::size_t>(channel)] = g;
            target[object][static_cast<std::size_t>(channel)] = g * place.gain;
        }
        scale[object] = place.gain;
        target_lfe[object] = place.lfe_send * place.gain;
    }
    if (!primed_) {
        gains_ = target;
        lfe_gains_ = target_lfe;
        primed_ = true;
    }

    // --- 2. The bed ---------------------------------------------------------
    // The ramp runs across the WHOLE frame, not per 256-sample block, because
    // both metadata layers say it does: OAMD sends one update per frame with a
    // 1 536-sample ramp_duration, and §6.6.5 interpolates the JOC matrix from
    // the previous frame's across every QMF timeslot in this one. A bed that
    // moved on a different schedule from the matrix that inverts it would
    // leave the reconstruction chasing the downmix.
    AC3_ZONE_BEGIN(zone_bed, "step2_bed_render");
    for (auto& channel : bed_) {
        std::ranges::fill(channel, 0.0f);
    }
    for (std::size_t object = 0; object < count; ++object) {
        const auto& source = objects[object];
        assert(source.size() == kSamplesPerFrame);
        for (int channel = 0; channel < kChannels; ++channel) {
            const double from = gains_[object][static_cast<std::size_t>(channel)];
            const double to = target[object][static_cast<std::size_t>(channel)];
            if (from == 0.0 && to == 0.0) {
                continue;
            }
            auto& out = bed_[static_cast<std::size_t>(
                kAc3FromJoc[static_cast<std::size_t>(channel)])];
            for (int n = 0; n < kSamplesPerFrame; ++n) {
                const double g = from + (to - from) * (n + 1) / kSamplesPerFrame;
                out[static_cast<std::size_t>(n)] += static_cast<float>(
                    g * static_cast<double>(source[static_cast<std::size_t>(n)]));
            }
        }
        if (lfe_gains_[object] != 0.0 || target_lfe[object] != 0.0) {
            auto& lfe = bed_[5];
            for (int n = 0; n < kSamplesPerFrame; ++n) {
                const double g = lfe_gains_[object] +
                                 (target_lfe[object] - lfe_gains_[object]) *
                                     (n + 1) / kSamplesPerFrame;
                lfe[static_cast<std::size_t>(n)] += static_cast<float>(
                    g * static_cast<double>(source[static_cast<std::size_t>(n)]));
            }
        }
    }
    AC3_ZONE_END(zone_bed);

    // --- 3. Per-band object energy -----------------------------------------
    std::vector<double> power(count * static_cast<std::size_t>(bands));
    for (std::size_t object = 0; object < count; ++object) {
        const auto slot = std::span{power}.subspan(
            object * static_cast<std::size_t>(bands), static_cast<std::size_t>(bands));
        band_energy(objects[object], mapping, slot, config_.fast_mdct);
        // The signal being reconstructed is the object AT ITS GAIN, so its
        // power carries the gain squared and the geometry stays in `pan`.
        const double squared = scale[object] * scale[object];
        for (auto& value : slot) {
            value *= squared;
        }
    }

    // --- 4. The reconstruction matrix ---------------------------------------
    // Minimum mean-square estimate of each object from the downmix. With
    // downmix = D s for known panning gains D and objects s of per-band power
    // p, the estimator that minimises the error is
    //     M = P D^T (D P D^T + eps I)^-1
    // which for well-separated objects is just D's left inverse - exact, not
    // approximate, because this encoder built the downmix and knows D exactly
    // rather than having to estimate it from the signals.
    AC3_ZONE_BEGIN(zone_joc_invert, "step4_joc_covariance_invert");
    for (int band = 0; band < bands; ++band) {
        std::array<std::array<double, kChannels>, kChannels> covariance{};
        for (std::size_t object = 0; object < count; ++object) {
            const double p = power[object * static_cast<std::size_t>(bands) +
                                   static_cast<std::size_t>(band)];
            if (p <= 0.0) {
                continue;
            }
            for (int a = 0; a < kChannels; ++a) {
                const double ga = pan[object][static_cast<std::size_t>(a)];
                if (ga == 0.0) {
                    continue;
                }
                for (int b = 0; b < kChannels; ++b) {
                    covariance[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] +=
                        p * ga * pan[object][static_cast<std::size_t>(b)];
                }
            }
        }
        double trace = 0.0;
        for (int c = 0; c < kChannels; ++c) {
            trace += covariance[static_cast<std::size_t>(c)][static_cast<std::size_t>(c)];
        }
        const double epsilon =
            std::max(kRelativeRegularization * trace / kChannels, kAbsoluteFloor);
        for (int c = 0; c < kChannels; ++c) {
            covariance[static_cast<std::size_t>(c)][static_cast<std::size_t>(c)] += epsilon;
        }
        const bool invertible = invert(covariance);

        for (std::size_t object = 0; object < count; ++object) {
            const double p = power[object * static_cast<std::size_t>(bands) +
                                   static_cast<std::size_t>(band)];
            for (int channel = 0; channel < kChannels; ++channel) {
                double value = 0.0;
                if (invertible && p > 0.0) {
                    for (int k = 0; k < kChannels; ++k) {
                        value += pan[object][static_cast<std::size_t>(k)] *
                                 covariance[static_cast<std::size_t>(k)]
                                           [static_cast<std::size_t>(channel)];
                    }
                    value *= p;
                }
                // The quantizer tops out at about +/-9,6 (§6.6.4). Clamping
                // here rather than letting quantize() do it silently keeps the
                // transmitted matrix and the one this encoder believes it sent
                // the same object.
                params_.at(static_cast<int>(object), channel, band) =
                    std::clamp(value, -9.5, 9.4);
            }
        }
    }
    AC3_ZONE_END(zone_joc_invert);

    // --- 5. Metadata --------------------------------------------------------
    std::vector<DynamicObject> described(count);
    for (std::size_t object = 0; object < count; ++object) {
        described[object].position = placement[object].position;
        // The gain is inside the reconstructed essence (see step 1), so the
        // renderer must not apply it a second time.
        described[object].gain_db = 0.0;
    }
    // §6.3.3.3: 0 marks the first frame, after which the counter runs 1..1023
    // and wraps to 1 rather than to 0 - a decoder reads 0 as a splice and
    // stops interpolating from a matrix that no longer means anything.
    params_.seq_count =
        frames_ == 0 ? 0 : static_cast<int>((frames_ - 1) % 1023 + 1);

    // The container is what carries the objects - and, on a decoder that
    // validates the emdf_protection field, it is also what commits that decoder
    // to object decoding: the moment its sync word is found in the skip field
    // and the container parses, that decoder must accept the protection field or
    // reject the whole access unit; there is no tolerant middle path that keeps
    // the bed. So a stream this encoder cannot make such a field validate for
    // either carries objects (and is refused by that decoder) or omits the
    // container and plays as the 5.1 bed - never both. config_.emit_object_metadata
    // picks which. The float bed built below (views) is identical regardless;
    // the encoded output is not bit-identical across the two, because dropping
    // the container hands its skip-field bytes back to the mantissas.
    std::vector<std::byte> container;
    if (config_.emit_object_metadata) {
        const auto oamd = build_payload(program_, described);
        const auto joc_payload = joc::build_payload(params_);
        const std::array<emdf::Payload, 2> payloads{{
            {.id = emdf::kPayloadIdOamd, .bytes = oamd},
            {.id = emdf::kPayloadIdJoc, .bytes = joc_payload},
        }};
        container = emdf::build_container(payloads);
    }

    // --- 6. The stream ------------------------------------------------------
    std::array<std::span<const float>, 6> views{};
    for (std::size_t channel = 0; channel < views.size(); ++channel) {
        views[channel] = bed_[channel];
    }
    auto unit = encoder_.encode_access_unit(views, container);
    if (!unit) {
        return std::unexpected(unit.error());
    }

    gains_ = target;
    lfe_gains_ = target_lfe;
    ++frames_;
    return unit;
}

}  // namespace ac3::oba
