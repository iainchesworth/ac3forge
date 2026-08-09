#include "quarantine_hook.hpp"

// Built whenever AC3FORGE_QUARANTINE_SIGNER is OFF (the default, and every
// public build) - see quarantine_hook.hpp for why this is a separate
// translation unit rather than an #ifdef.

namespace ac3cli {

int maybe_sign_atmos_units(std::vector<std::vector<std::byte>>& /*units*/) { return 0; }

}  // namespace ac3cli
