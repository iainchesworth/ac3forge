# ---------------------------------------------------------------------------
# InstallLibrary.cmake
#
# install() rules + package config for distributing ac3::forge, matroska::matroska and mp4::mp4
# independently, consumable via find_package(ac3forge). ac3::audio (src/audio/) is deliberately
# NOT installed/exported here - it is a CLI/GUI implementation detail, not part of the
# distributed package; see docs/library/index.md.
#
# include()'d from the root CMakeLists.txt after add_subdirectory(src/forge) and, for each
# optional component, its own guarded add_subdirectory(src/matroska|mp4|mpegts), before
# include(Packaging) - CPack's own library component (cmake/Packaging.cmake) packages exactly
# what gets install()'d here.
#
# matroska::matroska, mp4::mp4 and mpegts::mpegts are all optional components, off-able via
# their own AC3FORGE_BUILD_MATROSKA/AC3FORGE_BUILD_MP4/AC3FORGE_BUILD_MPEGTS option (root
# CMakeLists.txt) - each its own AC3FORGE_BUILD_<NAME> option, its own guarded
# add_subdirectory(), and its own guarded block below. Each maps 1:1 onto its own vcpkg
# feature (packaging/vcpkg-port/ac3forge/vcpkg.json's "matroska"/"mp4"/"mpegts", wired
# through portfile.cmake's vcpkg_check_features()), so a vcpkg install only gets the ones its
# feature selection actually asked for.
#
# Every install() rule below carries COMPONENT library: without one, CPack
# files it under its own "Unspecified" component, inconsistent once
# component-based packaging is on (see cmake/Packaging.cmake) - same reason
# apps/cli/CMakeLists.txt's ac3cli install() carries COMPONENT runtime.
#
# The LIBRARY DESTINATION rules below additionally carry NAMELINK_COMPONENT
# library, splitting them from COMPONENT libruntime. On Unix, a versioned
# shared library install produces two files - the real
# libac3forge.so.<version> and an unversioned libac3forge.so symlink (the
# "namelink") a linker resolves -l against - and NAMELINK_COMPONENT is CMake's
# own mechanism for filing those two files under different CPack components:
# COMPONENT names the real .so, NAMELINK_COMPONENT names the symlink. Confirmed
# empirically (see cmake/Packaging.cmake's DEB/RPM comment) that today's
# monolithic .deb bundles ac3cli together with the full SDK - headers, static
# archives, CMake package config, .so and symlink alike - because CPack's DEB/
# RPM generators ignore CPACK_COMPONENTS_ALL entirely unless *_COMPONENT_INSTALL
# is explicitly turned on for them. This split is what makes a real
# runtime/-dev separation possible there: libruntime becomes a small
# "just the .so a linked binary needs at runtime" package, while library
# keeps everything only a builder needs (headers, static archives, CMake
# config, and the symlink you link against, -l style). RUNTIME/ARCHIVE (the
# Windows .dll/.lib pair, and the static archives on every platform) stay
# under COMPONENT library throughout: NAMELINK_COMPONENT only ever affects the
# LIBRARY DESTINATION install, i.e. Unix .so installs - Windows has no
# namelink concept at all, so this is a no-op there and the Windows dev ZIP is
# unaffected.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# OFF is what a vcpkg port needs: vcpkg's per-triplet linkage policy (and its post-build lint)
# expects a port to ship only the variant matching that triplet's VCPKG_LIBRARY_LINKAGE, not
# both. ON (the default) keeps today's direct-build/CPack SDK behaviour unchanged - both
# variants installed and exported, same as before this option existed. forge_static/forge_shared
# and their matroska/mp4/mpegts equivalents still get *built* either way - only what gets
# install()'d/exported is filtered by this option, so nothing above this point in the tree
# needs touching for it to take effect.
option(AC3FORGE_INSTALL_BOTH_LINKAGES "Install/export both static and shared library variants (OFF installs only the BUILD_SHARED_LIBS-selected one)" ON)

if(AC3FORGE_INSTALL_BOTH_LINKAGES)
    set(_ac3forge_forge_install_targets forge_objects forge_static forge_shared)
    set(_ac3forge_matroska_install_targets matroska_objects matroska_static matroska_shared)
    set(_ac3forge_mp4_install_targets mp4_objects mp4_static mp4_shared)
    set(_ac3forge_mpegts_install_targets mpegts_objects mpegts_static mpegts_shared)
    set(_ac3forge_capi_install_targets forge_c_objects forge_c_static forge_c_shared)
elseif(BUILD_SHARED_LIBS)
    # ac3::forge_c (src/capi/CMakeLists.txt) statically embeds ac3::forge_static PRIVATE
    # unconditionally, regardless of BUILD_SHARED_LIBS - see that file's header comment for why
    # (a self-contained C ABI, not one that depends on a separately-shipped forge shared
    # library). forge_c_objects is an OBJECT library, so that PRIVATE dependency still ends up in
    # forge_c_objects's own INTERFACE_LINK_LIBRARIES (OBJECT libraries have no link step of their
    # own to hide it behind) - and since forge_c_objects is itself part of capiTargets whenever
    # AC3FORGE_BUILD_CAPI is ON, forge_static must be in an export set too, or install(EXPORT
    # capiTargets) fails with "requires target forge_static that is not in any export set."
    # forge_shared has no such requirement, so it doesn't need the same treatment here.
    if(AC3FORGE_BUILD_CAPI)
        set(_ac3forge_forge_install_targets forge_objects forge_static forge_shared)
    else()
        set(_ac3forge_forge_install_targets forge_objects forge_shared)
    endif()
    set(_ac3forge_matroska_install_targets matroska_objects matroska_shared)
    set(_ac3forge_mp4_install_targets mp4_objects mp4_shared)
    set(_ac3forge_mpegts_install_targets mpegts_objects mpegts_shared)
    set(_ac3forge_capi_install_targets forge_c_objects forge_c_shared)
else()
    set(_ac3forge_forge_install_targets forge_objects forge_static)
    set(_ac3forge_matroska_install_targets matroska_objects matroska_static)
    set(_ac3forge_mp4_install_targets mp4_objects mp4_static)
    set(_ac3forge_mpegts_install_targets mpegts_objects mpegts_static)
    set(_ac3forge_capi_install_targets forge_c_objects forge_c_static)
endif()

# Two separate EXPORT sets, not the one combined set an earlier draft of this
# plan sketched: install(EXPORT ... NAMESPACE X) applies X uniformly to
# every target in that export set, and ac3::forge_static/ac3::forge_shared
# need a different namespace from matroska::matroska_static/
# matroska::matroska_shared. Both still land in the one ac3forgeConfig.cmake
# a consumer's find_package(ac3forge) resolves - see ac3forgeConfig.cmake.in,
# which include()s both generated *Targets.cmake files.
# The _objects OBJECT library has to be in the same export set as the
# _static/_shared targets that PUBLIC-link it, even though nothing about it
# needs installing on its own (its compiled code is already embedded in the
# installed .lib/.dll) - install(EXPORT) otherwise refuses to generate,
# since it can't resolve a usage-requirement dependency that isn't itself
# part of any export set.
install(TARGETS ${_ac3forge_forge_install_targets}
    EXPORT ac3forgeTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

# Source headers, from ac3::forge's include/ tree.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/forge/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT library)

# Generated headers - ac3/version.hpp (from ac3/version.hpp.in) and the
# generate_export_header() output - live in the library's own binary dir, not
# its source tree (see src/forge/CMakeLists.txt), so the install(DIRECTORY
# .../include/) call above never sees them. A consumer's
# #include <ac3/version.hpp>/<ac3/export.hpp> needs both installed at the
# same relative paths the in-tree BUILD_INTERFACE include dirs already use.
install(FILES
        "${CMAKE_BINARY_DIR}/src/forge/generated/ac3/version.hpp"
        "${CMAKE_BINARY_DIR}/src/forge/generated/ac3/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3"
    COMPONENT library)

# matroska::matroska is an optional component (AC3FORGE_BUILD_MATROSKA, see the root
# CMakeLists.txt) - a vcpkg port maps this straight to its own "matroska" feature. Its
# targets/headers/export set only exist to install when the component was actually built;
# ac3forgeConfig.cmake.in's include() of matroskaTargets.cmake is itself conditional
# (if(EXISTS)) to match. mp4::mp4 (below) follows this same shape: its own
# AC3FORGE_BUILD_<NAME> option, its own guarded block, its own EXPORT set name.
if(AC3FORGE_BUILD_MATROSKA)
    install(TARGETS ${_ac3forge_matroska_install_targets}
        EXPORT matroskaTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/matroska/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/matroska/generated/matroska/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/matroska"
        COMPONENT library)
endif()

# mp4::mp4 is an optional component (AC3FORGE_BUILD_MP4, see the root CMakeLists.txt), same
# shape as matroska::matroska above including the AC3FORGE_INSTALL_BOTH_LINKAGES-selected
# target list - its targets, headers and export set only exist to install when the component
# was actually built. ac3forgeConfig.cmake.in's include() of mp4Targets.cmake is itself
# conditional (if(EXISTS)) to match. Maps onto its own "mp4" vcpkg feature the same way
# matroska does (packaging/vcpkg-port/ac3forge/vcpkg.json).
if(AC3FORGE_BUILD_MP4)
    install(TARGETS ${_ac3forge_mp4_install_targets}
        EXPORT mp4Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/mp4/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/mp4/generated/mp4/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mp4"
        COMPONENT library)
endif()

# mpegts::mpegts is an optional component (AC3FORGE_BUILD_MPEGTS, see the root
# CMakeLists.txt) - same shape as matroska::matroska immediately above, including its own
# "mpegts" vcpkg feature (packaging/vcpkg-port/ac3forge/vcpkg.json).
if(AC3FORGE_BUILD_MPEGTS)
    install(TARGETS ${_ac3forge_mpegts_install_targets}
        EXPORT mpegtsTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/mpegts/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/mpegts/generated/mpegts/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mpegts"
        COMPONENT library)
endif()

# ac3::forge_c is an optional component (AC3FORGE_BUILD_CAPI, see the root CMakeLists.txt) - same
# shape as matroska::matroska/mp4::mp4/mpegts::mpegts above. Roadmap item F1's whole point is a
# stable C-callable surface for OTHER toolchains, so its header (ac3forge_c/ac3forge.h) installs
# to its own include/ac3forge_c/ subdirectory rather than under include/ac3/ - a C or non-C++
# consumer has no reason to see (or accidentally #include) any C++ header this package ships.
if(AC3FORGE_BUILD_CAPI)
    install(TARGETS ${_ac3forge_capi_install_targets}
        EXPORT capiTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/capi/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/capi/generated/ac3forge_c/export.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3forge_c"
        COMPONENT library)
endif()

# The config file find_package(ac3forge) actually loads. No find_dependency()
# calls needed in ac3forgeConfig.cmake.in: with the platform-audio code
# physically in a separate, non-exported target (ac3::audio), the installed
# package has no third-party or system dependency whatsoever - matches
# vcpkg.json's own note that the codec itself has none.
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ac3forgeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge")

# SameMajorVersion, not exact: pre-1.0, there is no ABI-compatibility promise
# across any two releases (see src/forge/CMakeLists.txt's SOVERSION comment for
# the full reasoning), but SameMajorVersion is the conventional default and
# is what actually governs here - find_package()'s own version matching
# against a requested `find_package(ac3forge X.Y.Z)`, not the .so's SONAME
# (which is set separately, to the full version, precisely because 0.x has
# no narrower compatible range to express).
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
    COMPONENT library)

install(EXPORT ac3forgeTargets
    FILE ac3forgeTargets.cmake
    NAMESPACE ac3::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
    COMPONENT library)

if(AC3FORGE_BUILD_MATROSKA)
    install(EXPORT matroskaTargets
        FILE matroskaTargets.cmake
        NAMESPACE matroska::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_MP4)
    install(EXPORT mp4Targets
        FILE mp4Targets.cmake
        NAMESPACE mp4::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_MPEGTS)
    install(EXPORT mpegtsTargets
        FILE mpegtsTargets.cmake
        NAMESPACE mpegts::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_CAPI)
    install(EXPORT capiTargets
        FILE capiTargets.cmake
        NAMESPACE ac3::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()
