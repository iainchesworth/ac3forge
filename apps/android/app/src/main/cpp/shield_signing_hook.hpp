#pragma once

#include <cstddef>
#include <vector>

// The Shield app's per-frame object-signing seam over ac3::signing.
//
// Unlike the desktop CLI (which resolves a key from a path or env var), this
// app signs on-device, so it reads the key from a bundled asset - `signing.key`
// - loaded once through the same AAssetManager the lead-voice asset already
// uses. That asset is written into the APK at build time from a CI secret
// (.github/workflows/_build.yml) and is gitignored, so it is never committed;
// a build without it produces the safe, unsigned bed51 app exactly as before.
//
// Because signing runs on-device, a signed-build APK necessarily CONTAINS the
// key as that asset - anyone with the APK can extract it - so a signed APK must
// never be distributed, the same posture docs/platforms/android.md already
// states. See docs/concepts/object-signing.md and [[joc-decoder-auth-gate]].
//
// This replaces the old shield_quarantine_hook stub/enabled split: the signer
// is committed clean-room now (only the key is external), so there is no build
// variant to select and no #ifdef - one committed translation unit, always
// compiled.

struct AAssetManager;

namespace ac3shield {

// Loads the bundled `signing.key` asset into this unit's key, once, if present.
// Must be called before signing_available()/maybe_sign_atmos_unit(). A missing
// or empty asset (or a null manager) leaves signing unavailable - not an error.
void init_signing(AAssetManager* asset_manager);

// Whether a key was loaded. run_loop() reads this once to decide whether to
// emit the OAMD/JOC object container at all: an unsigned-but-present container
// is a hard refusal on a validating decoder, not a graceful 5.1 fallback (see
// AtmosConfig::emit_object_metadata), so an unsigned build omits it entirely
// and streams the bed51-equivalent, always safe on any receiver.
[[nodiscard]] bool signing_available();

// Signs one access unit's EMDF protection field in place with the loaded key.
// Returns true if signed, false when no key is loaded or the unit carries no
// EMDF container to sign.
[[nodiscard]] bool maybe_sign_atmos_unit(std::vector<std::byte>& unit);

}  // namespace ac3shield
