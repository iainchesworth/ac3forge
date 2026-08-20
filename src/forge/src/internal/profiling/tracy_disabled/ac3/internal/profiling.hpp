#pragma once

// Zone macros for Tracy instrumentation, no-op variant - see the
// tracy_enabled sibling directory's identically-pathed profiling.hpp for the
// real-Tracy variant and why CMake (not #ifdef) selects between the two.
// Every AC3_ZONE_SCOPED()/AC3_ZONE_SCOPED_N() call site expands to nothing at
// all here, so instrumented source compiles identically (down to the object
// code) to how it would with no instrumentation at all - this is the default
// (AC3FORGE_ENABLE_TRACY is OFF unless explicitly turned on).

#define AC3_ZONE_SCOPED()
#define AC3_ZONE_SCOPED_N(name)
#define AC3_ZONE_BEGIN(var, name)
#define AC3_ZONE_END(var)
