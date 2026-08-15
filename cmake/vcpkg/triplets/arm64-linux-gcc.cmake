# Overlay triplet: arm64 Linux, GCC.
#
# Linkage policy: dynamic runtime, static dependency libraries - see
# x64-windows-msvc.cmake for the reasoning, which is the same here.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: VCPKG_CHAINLOAD_TOOLCHAIN_FILE is set once, per concrete
# configure preset, and is deliberately NOT repeated here - one place decides
# which compiler a preset uses.
#
# So vcpkg builds Catch2 with its own default compiler rather than the pinned
# gcc-15. That is safe for this project because both the GCC and the Clang
# toolchain link the system libstdc++, so the port and the project agree on the
# standard-library ABI. Should ac3forge ever take a port whose ABI is sensitive
# to the exact compiler, that assumption is what has to be revisited.

# Found for real on GitHub's ubuntu-24.04-arm hosted runner, first CI run of
# this triplet: vcpkg's own manifest-install step requires a newer CMake
# (v4.4.0) than the container's apt-installed one (4.2.3) to build its
# internal helper ports (vcpkg-cmake, vcpkg-cmake-config, and Catch2 itself),
# and tries to download and extract a private pinned copy for linux-aarch64 -
# which fails ("expected this path to exist after extracting cmake"), an
# upstream vcpkg/Kitware tool-acquisition gap for this platform, not anything
# in this project's control. VCPKG_FORCE_SYSTEM_BINARIES is vcpkg's own
# documented escape hatch for exactly this: it makes vcpkg build ports with
# the system's CMake/Ninja instead of downloading its own pinned copies, and
# is the standard setting vcpkg's own community triplets carry for ARM and
# other platforms without reliable prebuilt tool binaries. 4.2.3 is more than
# new enough to build these ports; there is nothing version-sensitive about
# them.
set(VCPKG_FORCE_SYSTEM_BINARIES ON)
