# ---------------------------------------------------------------------------
# The Shield app's native build: a THIN wrapper, not a second copy of
# ac3forge's build graph. It exists to do exactly two things:
#
#  1. Pull in the real repo root's CMakeLists.txt via add_subdirectory(), so
#     ac3::forge and ac3::audio (including this app's own
#     src/audio/src/platform/android/ backend) are built from the SAME
#     source-of-truth target definitions the desktop CLI/GUI use - a change
#     to the core lib is picked up here automatically, nothing to keep in
#     sync by hand.
#  2. Build this app's own small ac3forge_jni shared library - the one
#     artifact AGP's externalNativeBuild actually loads into the app - which
#     links those two libraries plus this app's own native sources.
#
# See docs/platforms/android.md for the full architecture writeup and
# ../../../../../../CMakeLists.txt (the repo root, six directories up: cpp ->
# main -> src -> app -> android -> platform -> repo root) for what
# add_subdirectory() below actually configures.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28...4.3)
project(ac3forge_shield_jni LANGUAGES CXX)

# Pre-seed the repo root's option()s before pulling it in: option() only sets
# a cache variable if one is not already present, so FORCE-ing these first
# means the root CMakeLists.txt's own `option(AC3FORGE_BUILD_CLI ... ON)`
# etc. see these values already set and leave them alone. This app needs
# none of the desktop-only targets or Catch2/examples - only the two
# libraries add_subdirectory() below builds regardless of these options.
set(AC3FORGE_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_GUI OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)

# ac3forge_jni's own target_link_libraries below is ac3::forge/ac3::audio/ac3::signing only
# (see the comment above the MATROSKA/MP4/MPEGTS block just below) - this app never links
# ac3::forge_c, so there is no reason to compile src/capi at all here. Off explicitly rather
# than relying on the root CMakeLists.txt's default ON: leaving it on would silently compile
# forge_c_objects/forge_c_static/forge_c_shared into every Shield build for nothing.
set(AC3FORGE_BUILD_CAPI OFF CACHE BOOL "" FORCE)

# The Shield app links none of the three standalone container writers either
# (ac3forge_jni's own target_link_libraries below is ac3::forge/ac3::audio/
# ac3::signing only - it streams live objects out AAudio passthrough, it
# never muxes a file) - CLI/GUI/TESTS/EXAMPLES being OFF above is exactly the
# condition the root CMakeLists.txt's own FATAL_ERROR guards require before
# these three may also be OFF ("this is the shape a vcpkg build without the
# matching feature actually uses"). Turning them off here is not just an
# unused-code trim: NDK r26's bundled libc++ (LLVM 17) ships <format> gated
# behind __has_feature(experimental_library) - undefined unless the compiler
# is invoked with -fexperimental-library, which nothing in this project's
# Android build passes - so mp4::mp4's HLS/DASH signaling helpers
# (src/mp4/src/hls.cpp, dash.cpp), which use std::format, fail to compile for
# Android with "no member named 'format' in namespace 'std'" the same
# fundamental gap version.cpp's own std::format-avoiding comment already
# documents for a different reason. Since this app was never going to link
# mp4::mp4 (or matroska::matroska/mpegts::mpegts) anyway, not building them
# is the correct fix on both counts, not a workaround for the <format> gap
# specifically - confirmed by reproducing the exact NDK 26.1.10909125 error
# with a standalone aarch64-linux-android26-clang++ invocation before making
# this change (`-fexperimental-library` alone does fix the standalone repro,
# but adding a project-wide experimental-library flag to work around a
# module this app has no other reason to compile would be the wrong fix).
set(AC3FORGE_BUILD_MATROSKA OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_MP4 OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_MPEGTS OFF CACHE BOOL "" FORCE)

# The binary directory name is arbitrary but must be given explicitly: with
# a source directory this far outside CMAKE_SOURCE_DIR (the wrapper's own
# directory, per project() above), CMake cannot derive a binary dir by
# mirroring the relative path the way it does for an in-tree subdirectory.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ac3forge_build)

add_library(ac3forge_jni SHARED
    jni_entry.cpp
    live_cursor.cpp
    file_replay.cpp
    shield_signing_hook.cpp)

target_compile_features(ac3forge_jni PRIVATE cxx_std_23)

# log: android/log.h, used by jni_entry.cpp and by
# src/audio/src/platform/android/passthrough.cpp (linked in transitively via
# ac3::audio, which already declares its own aaudio/log deps - see
# src/audio/CMakeLists.txt's ANDROID branch).
# android: android/asset_manager_jni.h (AAssetManager_fromJava), used by
# live_cursor.cpp to load the bundled lead-object voice sample and by
# shield_signing_hook.cpp to load the bundled signing key.
# ac3::signing: the committed clean-room EMDF object signer (built by the repo
# root add_subdirectory above, dependency-free). Always linked now - the signer
# has no embedded secret to keep out of the tree; whether it actually signs
# depends on the runtime key asset, not on a build variant. See
# shield_signing_hook.hpp.
target_link_libraries(ac3forge_jni PRIVATE ac3::forge ac3::audio ac3::signing log android)
