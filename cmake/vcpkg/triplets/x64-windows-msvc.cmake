# Overlay triplet: x64 Windows, MSVC.
#
# Linkage policy: dynamic CRT, static dependency libraries. ac3forge takes only
# test/tooling packages from vcpkg (Catch2), so linking them statically keeps
# ac3tests.exe self-contained and removes a whole class of "DLL not found"
# failures at test-discovery time. The CRT stays dynamic (/MD) because the
# prebuilt Qt kits the GUI links against are built that way.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy (see also the other triplets and CMakePresets.json):
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE is set once, per concrete configure preset,
# and is deliberately NOT repeated here. One place decides which compiler a
# preset uses; a triplet that also set it would be a second, silently
# disagreeing source of truth.
#
# The practical consequence is that vcpkg builds the ports themselves with its
# own default toolset rather than the chainloaded one. On Windows that is what
# you want anyway - ports that drive their own build systems need the
# unmodified MSVC environment - and clang-cl is MSVC-ABI compatible, so the
# same port binaries link into either Windows preset.
#
# No VCPKG_ENV_PASSTHROUGH here (the reference project needs it for nmake-based
# ports such as OpenSSL). ac3forge's only port is Catch2, a plain CMake build,
# and passthrough variables feed the ABI hash - passing PATH through would
# invalidate the binary cache every time PATH changed.
