# Overlay triplet: arm64 macOS (Apple Silicon), LLVM (clang).
#
# Only arm64 is provided. An x64 macOS triplet is easy to add later, but a dead
# triplet nobody configures is worse than no triplet at all.
#
# Linkage policy: dynamic runtime, static dependency libraries - see
# x64-windows-msvc.cmake for the reasoning, which is the same here.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: not set here. See x64-linux-gcc.cmake.
#
# macOS carries one extra wrinkle: macos.llvm.toolchain.cmake prefers LLVM's own
# libc++ over the SDK's, while vcpkg will build the ports against the SDK's.
# Both are libc++ so the ABI matches, but this is the platform where the
# assumption is thinnest - and it is untested, since this project has no macOS
# host to configure on.
