#include "shield_quarantine_hook.hpp"

#include <cstdlib>

#include "quarantine/emdf_atmos_signer.hpp"

// Built only when -DAC3FORGE_QUARANTINE_SIGNER=ON, which
// platform/android/app/src/main/cpp/CMakeLists.txt only accepts once
// src/quarantine/ (the gitignored, local-only overlay) is actually present -
// same FATAL_ERROR guard as the repo root's own CMakeLists.txt. Reuses the
// exact same ac3::quarantine API src/cli/quarantine_hook_enabled.cpp calls -
// this is the same overlay, just a different (per-frame, not per-batch)
// call site.

namespace ac3shield {

namespace {

// ac3::quarantine::sign_requested() is a deliberate second, runtime gate on
// top of the CMake option (see emdf_atmos_signer.hpp/README.md): even a
// build with the overlay compiled in stays unsigned unless AC3FORGE_SIGN is
// also set in the process environment. ac3cli gets that from its own shell
// invocation; an Android app launched via `am start` has no shell to inherit
// it from at all (zygote forks app_process from its own boot-time
// environment, not the adb client's) - so, only in this already-quarantined
// translation unit, set it once for our own process before ever asking. This
// keeps the same two-gate shape (compile-time option + explicit opt-in) the
// CLI has, rather than silently dropping the runtime half of it because the
// usual mechanism for supplying it doesn't exist on this platform.
void ensure_sign_requested_env() {
    static const bool done = [] {
        setenv("AC3FORGE_SIGN", "1", 0);
        return true;
    }();
    (void)done;
}

}  // namespace

bool maybe_sign_atmos_unit(std::vector<std::byte>& unit) {
    ensure_sign_requested_env();
    if (!ac3::quarantine::sign_requested()) {
        return false;
    }
    return ac3::quarantine::sign_atmos_stream(unit) > 0;
}

bool signing_available() {
    ensure_sign_requested_env();
    return ac3::quarantine::sign_requested();
}

}  // namespace ac3shield
