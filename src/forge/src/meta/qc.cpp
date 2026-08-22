#include "ac3/meta/qc.hpp"

#include <cmath>
#include <optional>
#include <string_view>

namespace ac3::meta {

bool parse_qc_preset(std::string_view name, QcPresetId& out) {
    if (name == "ebu-r128-s2") {
        out = QcPresetId::kEbuR128S2;
        return true;
    }
    if (name == "atsc-a85") {
        out = QcPresetId::kAtscA85;
        return true;
    }
    if (name == "netflix") {
        out = QcPresetId::kNetflix;
        return true;
    }
    return false;
}

QcVerdict evaluate_qc_gate(const QcPreset& preset, std::optional<double> integrated_lkfs,
                           std::optional<double> true_peak_dbtp) {
    QcVerdict verdict;
    if (integrated_lkfs) {
        verdict.loudness_delta_lu = *integrated_lkfs - preset.target_lkfs;
        verdict.loudness_pass = std::abs(*verdict.loudness_delta_lu) <= preset.tolerance_lu;
    }
    if (true_peak_dbtp) {
        verdict.true_peak_margin_dbtp = preset.max_true_peak_dbtp - *true_peak_dbtp;
        verdict.true_peak_pass = *true_peak_dbtp <= preset.max_true_peak_dbtp;
    }
    return verdict;
}

}  // namespace ac3::meta
