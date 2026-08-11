#include "shield_quarantine_hook.hpp"

// Built whenever AC3FORGE_QUARANTINE_SIGNER is OFF (the default, and every
// public build) - see shield_quarantine_hook.hpp for why this is a separate
// translation unit rather than an #ifdef.

namespace ac3shield {

bool maybe_sign_atmos_unit(std::vector<std::byte>& /*unit*/) { return false; }

bool signing_available() { return false; }

}  // namespace ac3shield
