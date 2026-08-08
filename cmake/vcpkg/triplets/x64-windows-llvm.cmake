# Overlay triplet: x64 Windows, LLVM (clang-cl).
#
# Same linkage policy as x64-windows-msvc - see that file for the reasoning.
# clang-cl produces MSVC-ABI-compatible objects, so this triplet exists to name
# the configuration rather than to change how the ports are built; it is the
# place any future LLVM-only divergence (a static CRT, a sanitizer runtime)
# would go without disturbing the MSVC build.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: not set here. See x64-windows-msvc.cmake.
