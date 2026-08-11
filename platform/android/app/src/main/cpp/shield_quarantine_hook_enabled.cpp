#include "shield_quarantine_hook.hpp"

#include "quarantine/emdf_atmos_signer.hpp"

// Built only when -DAC3FORGE_QUARANTINE_SIGNER=ON, which
// platform/android/app/src/main/cpp/CMakeLists.txt only accepts once
// src/quarantine/ (the gitignored, local-only overlay) is actually present -
// same FATAL_ERROR guard as the repo root's own CMakeLists.txt. Reuses the
// exact same ac3::quarantine API src/cli/quarantine_hook_enabled.cpp calls -
// this is the same overlay, just a different (per-frame, not per-batch)
// call site.

namespace ac3shield {

bool maybe_sign_atmos_unit(std::vector<std::byte>& unit) {
    if (!ac3::quarantine::sign_requested()) {
        return false;
    }
    return ac3::quarantine::sign_atmos_stream(unit) > 0;
}

}  // namespace ac3shield
