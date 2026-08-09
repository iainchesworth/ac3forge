#pragma once

#include <cstddef>
#include <vector>

// The optional, non-clean-room EMDF Atmos signer overlay (src/quarantine) is
// gitignored and local-only, so main.cpp cannot include its header
// unconditionally. Rather than an #ifdef around the call site (forbidden by
// scripts/check-platform-macros.ps1 - a feature flag is as unwelcome there as
// a platform one), this follows the same pattern src/lib/CMakeLists.txt uses
// to select a platform backend: exactly one of quarantine_hook_stub.cpp
// (default) and quarantine_hook_enabled.cpp (built only when
// -DAC3FORGE_QUARANTINE_SIGNER=ON, see src/cli/CMakeLists.txt) is ever
// compiled, so no translation unit here has to ask which build it is in.

namespace ac3cli {

// Signs each unit's EMDF protection field in place when this build includes
// the overlay and it reports signing was requested. Returns the number of
// frames signed (0 when the overlay is absent or declined to sign).
[[nodiscard]] int maybe_sign_atmos_units(std::vector<std::vector<std::byte>>& units);

}  // namespace ac3cli
