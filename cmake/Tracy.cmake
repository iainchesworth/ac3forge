# ---------------------------------------------------------------------------
# Tracy.cmake
#
# Defines an INTERFACE target `ac3::tracy` that, when AC3FORGE_ENABLE_TRACY is
# on, links Tracy's client library and defines AC3FORGE_TRACY_ENABLED, which
# src/lib/include/ac3/internal/profiling.hpp uses to turn AC3_ZONE_SCOPED()
# etc. into real Tracy zones instead of no-ops. Off by default, matching
# Coverage.cmake's shape: normal dev/CI builds pay no instrumentation cost and
# do not even need Tracy present.
#
# Tracy itself is an OPT-IN vcpkg manifest feature ("profiling" in
# vcpkg.json), not a base dependency - configure with
# -DVCPKG_MANIFEST_FEATURES=profiling to make it resolvable at all. The
# cli-tools feature is what this investigation actually needs: `capture` and
# `csvexport` record and dump a trace headlessly, without the GUI profiler.
# See docs/platforms/android.md's performance-investigation notes for how
# this was used to find the encode_frame() hotspot.
# ---------------------------------------------------------------------------

option(AC3FORGE_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)

add_library(ac3_tracy INTERFACE)
add_library(ac3::tracy ALIAS ac3_tracy)

if(AC3FORGE_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(ac3_tracy INTERFACE Tracy::TracyClient)
    target_compile_definitions(ac3_tracy INTERFACE AC3FORGE_TRACY_ENABLED)
endif()
