#pragma once

// Zone macros for Tracy instrumentation, real-Tracy variant - selected by
// CMake (src/lib/CMakeLists.txt adds this directory, not the tracy_disabled
// sibling, to forge_objects's private include path when AC3FORGE_ENABLE_TRACY
// is on - see cmake/Tracy.cmake) rather than an #ifdef, per the project's
// platform/feature-isolation rule (scripts/check-platform-macros.ps1):
// exactly one of this file and tracy_disabled/ac3/internal/profiling.hpp is
// ever compiled, both at this same "ac3/internal/profiling.hpp" relative
// path, so every call site's #include "ac3/internal/profiling.hpp" and every
// AC3_ZONE_SCOPED()/AC3_ZONE_SCOPED_N() use compiles unchanged either way.
//
// Internal, not installed: this is a build-diagnostics tool, not part of the
// library's public interface - hence living under src/, not include/.

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#define AC3_ZONE_SCOPED() ZoneScoped
#define AC3_ZONE_SCOPED_N(name) ZoneScopedN(name)
// Manual (non-lexically-scoped) begin/end pair, for marking a span that does
// not correspond to a single C++ scope - e.g. one numbered "section" inside
// an existing, already-large function this profiling pass does not want to
// restructure into nested blocks just to give each section its own scope.
// `var` names the TracyCZoneCtx local these two calls share.
#define AC3_ZONE_BEGIN(var, name) TracyCZoneN(var, name, true)
#define AC3_ZONE_END(var) TracyCZoneEnd(var)
