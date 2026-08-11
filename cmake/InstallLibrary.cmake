# ---------------------------------------------------------------------------
# InstallLibrary.cmake
#
# install() rules + package config for distributing ac3::forge and
# matroska::matroska independently, consumable via find_package(ac3forge).
# ac3::audio (src/audio/) is deliberately NOT installed/exported here - it is
# a CLI/GUI implementation detail, not part of the distributed package; see
# docs/library/index.md.
#
# include()'d from the root CMakeLists.txt after add_subdirectory(src/lib)
# and add_subdirectory(src/matroska), before include(Packaging) - CPack's
# own library component (cmake/Packaging.cmake) packages exactly what gets
# install()'d here.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
install(TARGETS forge_objects forge_static forge_shared
    EXPORT ac3forgeTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

install(TARGETS matroska_objects matroska_static matroska_shared
    EXPORT matroskaTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# Source headers, from both libraries' include/ trees.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/lib/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/matroska/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Generated headers - ac3/version.hpp (from ac3/version.hpp.in) and the two
# generate_export_header() outputs - live in each library's own binary dir,
# not its source tree (see src/lib/CMakeLists.txt, src/matroska/CMakeLists.txt),
# so the install(DIRECTORY .../include/) calls above never see them. A
# consumer's #include <ac3/version.hpp>/<ac3/export.hpp>/<matroska/export.hpp>
# needs all three installed at the same relative paths the in-tree
# BUILD_INTERFACE include dirs already use.
install(FILES
        "${CMAKE_BINARY_DIR}/src/lib/generated/ac3/version.hpp"
        "${CMAKE_BINARY_DIR}/src/lib/generated/ac3/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3")
install(FILES "${CMAKE_BINARY_DIR}/src/matroska/generated/matroska/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/matroska")

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
# across any two releases (see src/lib/CMakeLists.txt's SOVERSION comment for
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
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge")

install(EXPORT ac3forgeTargets
    FILE ac3forgeTargets.cmake
    NAMESPACE ac3::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge")

install(EXPORT matroskaTargets
    FILE matroskaTargets.cmake
    NAMESPACE matroska::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge")
