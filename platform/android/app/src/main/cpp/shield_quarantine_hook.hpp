#pragma once

#include <cstddef>
#include <vector>

// The optional, non-clean-room EMDF Atmos signer overlay (src/quarantine) is
// gitignored and local-only, so live_cursor.cpp cannot include its header
// unconditionally. Rather than an #ifdef around the call site (forbidden by
// scripts/check-platform-macros.ps1 - a feature flag is as unwelcome there
// as a platform one), this follows the exact same pattern
// src/cli/quarantine_hook.hpp already uses for ac3cli: exactly one of
// shield_quarantine_hook_stub.cpp (default) and
// shield_quarantine_hook_enabled.cpp (built only when
// -DAC3FORGE_QUARANTINE_SIGNER=ON, see
// platform/android/app/src/main/cpp/CMakeLists.txt) is ever compiled, so no
// translation unit here has to ask which build it is in.
//
// Per-frame here, not the CLI's whole-batch maybe_sign_atmos_units: this app
// streams live rather than writing a file, so there is no batch to sign
// after the fact - this is called once per encode_frame() output, right
// before IEC61937 wrapping (live_cursor.cpp's run_loop).
//
// This build stays personal/local-only even with the option on - see
// docs/platforms/android.md and [[joc-decoder-auth-gate]] in project memory:
// the signer is what makes object audio actually unlock on a real
// Dolby-licensed decoder, but it embeds a key extracted from Dolby's binary
// and must never reach a public remote, same posture as
// local/quarantine-signer.

namespace ac3shield {

// Signs one access unit's EMDF protection field in place when this build
// includes the overlay and it reports signing was requested. Returns true
// if signed, false when the overlay is absent, declined to sign, or this
// unit carries no EMDF container to sign at all.
[[nodiscard]] bool maybe_sign_atmos_unit(std::vector<std::byte>& unit);

// Whether this build can sign at all - true only in the enabled TU (compiled
// only when the gitignored src/quarantine overlay is present and
// -DAC3FORGE_QUARANTINE_SIGNER=ON). live_cursor.cpp reads this once at
// startup to decide whether to emit the OAMD/JOC object container in the
// first place: per AtmosConfig::emit_object_metadata's own comment, carrying
// an unsigned container is not a safe degraded mode on a decoder that
// validates the emdf_protection field - it is a hard refusal, not a
// fallback-to-5.1. A public build (this function returning false) omits the
// container entirely instead, the same bed51 mode ac3cli's --mode flag
// exposes, so the released app is always safe to play on any receiver.
[[nodiscard]] bool signing_available();

}  // namespace ac3shield
