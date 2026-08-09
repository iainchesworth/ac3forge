#include "quarantine_hook.hpp"

#include "quarantine/emdf_atmos_signer.hpp"

// Built only when -DAC3FORGE_QUARANTINE_SIGNER=ON, which src/cli/CMakeLists.txt
// only accepts once src/quarantine/ (the gitignored, local-only overlay) is
// actually present - see CMakeLists.txt's own FATAL_ERROR guard.

namespace ac3cli {

int maybe_sign_atmos_units(std::vector<std::vector<std::byte>>& units) {
    if (!ac3::quarantine::sign_requested()) {
        return 0;
    }
    int signed_count = 0;
    for (auto& unit : units) {
        signed_count += ac3::quarantine::sign_atmos_stream(unit);
    }
    return signed_count;
}

}  // namespace ac3cli
