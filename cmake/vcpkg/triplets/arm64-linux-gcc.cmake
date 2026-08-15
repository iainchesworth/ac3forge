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
