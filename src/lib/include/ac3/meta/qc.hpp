#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ac3/export.hpp"

// Named loudness/true-peak delivery gates a decoded stream's measurement can
// be checked against - roadmap item C2 (`ac3cli qc`). Each preset states a
// target integrated loudness, a symmetric tolerance around it (in LU) and a
// true peak ceiling, taken straight from the delivery specification's own
// numbers - see qc_preset()'s own comment on each case for the exact clause
// cited; every number here was read out of the primary document, not
// recalled from memory.
//
// Deliberately the same shape ac3::meta::Profile/ProfileId (drc.hpp) uses for
// the §7.7.1 DRC profile table: a small enum naming the presets, a constexpr
// accessor returning the numbers, and a name<->id parser - so a caller
// (ac3cli qc, and eventually the GUI's own QC panel, roadmap C3) reads one
// table instead of hand-copying the same magic numbers more than once.

namespace ac3::meta {

struct QcPreset {
    double target_lkfs = 0.0;
    double tolerance_lu = 0.0;        // +/- around target_lkfs
    double max_true_peak_dbtp = 0.0;  // a ceiling, not a tolerance band
};

enum class QcPresetId : std::uint8_t {
    kEbuR128S2,
    kAtscA85,
    kNetflix,
};

// The numbers themselves, each with its primary source cited beside the
// value it backs - the same layout drc.hpp's profile() uses for its derived
// boost edges.
[[nodiscard]] constexpr QcPreset qc_preset(QcPresetId id) {
    switch (id) {
        case QcPresetId::kEbuR128S2:
            // EBU R 128 s2 "Loudness in Streaming" (Geneva, November 2023,
            // v3) recommendation (e): "programmes should be streamed
            // unchanged, that is at -23.0 LUFS" - s2 itself defers tolerance
            // and true peak to the parent recommendation ("for production
            // and QC tolerances, also refer to [1]"). EBU R 128 (Geneva,
            // November 2023, v5) recommendation (h): "a tolerance of +/-1.0
            // LU is permitted" where hitting the Target Level is "not
            // achievable practically"; recommendation (m): "the True Peak
            // Level of a programme shall not exceed -1 dBTP during
            // production".
            return {.target_lkfs = -23.0, .tolerance_lu = 1.0, .max_true_peak_dbtp = -1.0};
        case QcPresetId::kAtscA85:
            // ATSC A/85:2013 (with Corrigendum No. 1, 11 February 2021) §6
            // "Target Loudness and True Peak Levels for Content Delivery or
            // Exchange": "the Target Loudness value should be -24 LKFS.
            // Minor measurement variations of up to approximately +/-2 dB
            // about this value are anticipated, due to measurement
            // uncertainty, and are acceptable... The true-peak level should
            // be kept below -2 dB TP".
            return {.target_lkfs = -24.0, .tolerance_lu = 2.0, .max_true_peak_dbtp = -2.0};
        case QcPresetId::kNetflix:
            // Netflix "Sound Mix Specifications & Best Practices" v1.6
            // (partnerhelp.netflixstudios.com), Near-field Audio
            // Prerequisites for Mix Facilities: "Set average loudness at -27
            // LKFS with a tolerance of +/-2 LU, dialog-gated. Peaks must not
            // exceed -2dB True Peak."
            return {.target_lkfs = -27.0, .tolerance_lu = 2.0, .max_true_peak_dbtp = -2.0};
    }
    return {};
}

[[nodiscard]] constexpr std::string_view qc_preset_name(QcPresetId id) {
    switch (id) {
        case QcPresetId::kEbuR128S2: return "ebu-r128-s2";
        case QcPresetId::kAtscA85: return "atsc-a85";
        case QcPresetId::kNetflix: return "netflix";
    }
    return "";
}

// Names accepted on the command line, in QcPresetId order.
inline constexpr std::string_view kQcPresetNames = "ebu-r128-s2 | atsc-a85 | netflix";

[[nodiscard]] AC3FORGE_EXPORT bool parse_qc_preset(std::string_view name, QcPresetId& out);

// Every preset, in declaration order - for a caller that wants to check a
// measurement against all of them (ac3cli qc's own preset=all).
inline constexpr std::array<QcPresetId, 3> kQcPresetIds{
    QcPresetId::kEbuR128S2, QcPresetId::kAtscA85, QcPresetId::kNetflix};

// One preset's verdict against one measurement. Loudness gates on
// |measured - target| against the preset's tolerance (a band); true peak
// gates on not exceeding the ceiling (a one-sided limit, not a band, matching
// every source cited in qc_preset() above). Either half is left at its
// not-passing default when the corresponding measurement itself is
// std::nullopt - LoudnessMeter's own "no meaningful loudness"/"no sample yet"
// stance on material this gate cannot actually judge.
struct QcVerdict {
    std::optional<double> loudness_delta_lu;      // measured - target
    bool loudness_pass = false;
    std::optional<double> true_peak_margin_dbtp;  // ceiling - measured; >= 0 passes
    bool true_peak_pass = false;

    [[nodiscard]] bool pass() const { return loudness_pass && true_peak_pass; }
};

[[nodiscard]] AC3FORGE_EXPORT QcVerdict evaluate_qc_gate(const QcPreset& preset,
                                                         std::optional<double> integrated_lkfs,
                                                         std::optional<double> true_peak_dbtp);

}  // namespace ac3::meta
