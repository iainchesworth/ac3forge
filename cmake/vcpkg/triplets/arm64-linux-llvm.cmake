# Overlay triplet: arm64 Linux, LLVM (clang).
#
# Linkage policy: dynamic runtime, static dependency libraries - see
# x64-windows-msvc.cmake for the reasoning, which is the same here.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: not set here. See arm64-linux-gcc.cmake - the note about
# clang and gcc sharing libstdc++ is what makes that safe, and is also why
# linux.llvm.toolchain.cmake does not switch to libc++.

# See arm64-linux-gcc.cmake's identical setting for why: vcpkg's own
# manifest-install CMake-acquisition is broken for linux-aarch64 upstream,
# found for real on GitHub's ubuntu-24.04-arm runner.
set(VCPKG_FORCE_SYSTEM_BINARIES ON)
