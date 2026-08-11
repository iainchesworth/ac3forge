#pragma once

// Zone macros for Tracy instrumentation - see cmake/Tracy.cmake for how
// AC3FORGE_TRACY_ENABLED gets defined (AC3FORGE_ENABLE_TRACY=ON, which needs
// vcpkg's "profiling" manifest feature resolved first). Internal, not
// installed: this is a build-diagnostics tool, not part of the library's
// public interface.
//
// The single #ifdef lives HERE and nowhere else - every call site uses
// AC3_ZONE_SCOPED()/AC3_ZONE_SCOPED_N() unconditionally, which expand to
// nothing at all when profiling is off, so instrumented source compiles
// identically (down to the object code) whether or not Tracy is enabled.

#ifdef AC3FORGE_TRACY_ENABLED
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
#else
#define AC3_ZONE_SCOPED()
#define AC3_ZONE_SCOPED_N(name)
#define AC3_ZONE_BEGIN(var, name)
#define AC3_ZONE_END(var)
#endif
