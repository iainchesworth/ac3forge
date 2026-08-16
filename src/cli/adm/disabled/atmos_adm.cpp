#include "../atmos_adm.hpp"

// Compiled only when AC3FORGE_BUILD_ADM did NOT turn ac3adm::ac3adm/ac3::admbridge on (see
// src/cli/CMakeLists.txt) - see ../atmos_adm.hpp's own top comment for why this file, rather than
// a preprocessor conditional inside main.cpp, is the mechanism. This translation unit links
// neither ac3adm::ac3adm nor ac3::admbridge and includes neither of their headers - it cannot,
// since in this build neither target was ever add_subdirectory()'d at all.

namespace ac3cli {

const ac3::platform::Capability& adm_capability() {
    static constexpr ac3::platform::Capability kUnavailable{
        .available = false,
        .reason = "this build was not configured with -DAC3FORGE_BUILD_ADM=ON "
                  "(ac3adm::ac3adm/ac3::admbridge were not linked in)"};
    return kUnavailable;
}

std::expected<AdmAtmosSource, std::string> load_adm_atmos_source(std::string_view /*path*/,
                                                                  std::string_view /*programme_id*/) {
    // Unreachable in practice: main.cpp's dispatch loop checks adm_capability() (kCommands'
    // Needs::kAdm row) before ever calling run_atmos_adm, the only caller of this function - see
    // main.cpp's own unmet()/kCommands. Still a real, defined function rather than an abort()  or
    // an unreachable()  marker, in case that gate is ever bypassed or this function gains another
    // caller - the same "never silently do nothing" standard this project's other refusal paths
    // hold themselves to.
    return std::unexpected(std::string(adm_capability().reason));
}

}  // namespace ac3cli
