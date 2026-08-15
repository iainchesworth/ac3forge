# ---------------------------------------------------------------------------
# Included by cpack itself once per generator during a multi-generator run
# (CPACK_PROJECT_CONFIG_FILE, wired up in Packaging.cmake) - CPACK_GENERATOR
# is set to the single generator currently being packaged when this runs, so
# variables here apply to that pass only, not the whole cpack invocation.
#
# Why this file exists: CPACK_COMPONENTS_GROUPING IGNORE (Packaging.cmake) is
# what makes the archive generators (ZIP/TGZ) split into one independent
# runtime/library download each - the deliberate design those generators
# use. But that setting is global CPack state, not archive-specific, and
# DragNDrop (macOS) reads the exact same value - confirmed on real macOS CI
# during v0.3.0-beta.1's dry run, where it kept splitting into a -runtime.dmg
# and a -library.dmg even with CPACK_DMG_COMPONENT_INSTALL explicitly OFF.
# That variable alone does not override CPACK_COMPONENTS_GROUPING here.
# Forcing monolithic installation just for DragNDrop's own pass restores the
# one-dmg-bundles-everything shape this project has always intended for it,
# without touching the archive generators' split.
# ---------------------------------------------------------------------------

if(CPACK_GENERATOR STREQUAL "DragNDrop")
    set(CPACK_MONOLITHIC_INSTALL ON)
endif()

# DEB/RPM: the opposite override from DragNDrop above. Packaging.cmake's
# CPACK_COMPONENT_LIBRARY_GROUP/CPACK_COMPONENT_LIBRUNTIME_GROUP "dev" merges
# library+libruntime into one archive for ZIP/TGZ - correct there, but DEB/
# RPM want those same two components to stay three independent .deb/.rpm
# files (runtime, libac3forge0, libac3forge-dev/ac3forge-devel), which is the
# entire reason they're split into their own CPack components in the first
# place (see cmake/InstallLibrary.cmake). IGNORE - one package per component,
# not per group - restores that for exactly these two generators' own passes,
# without touching the group-based merge every other generator still uses.
if(CPACK_GENERATOR STREQUAL "DEB" OR CPACK_GENERATOR STREQUAL "RPM")
    set(CPACK_COMPONENTS_GROUPING IGNORE)
endif()
