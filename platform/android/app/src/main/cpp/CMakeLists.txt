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

# The binary directory name is arbitrary but must be given explicitly: with
# a source directory this far outside CMAKE_SOURCE_DIR (the wrapper's own
# directory, per project() above), CMake cannot derive a binary dir by
# mirroring the relative path the way it does for an in-tree subdirectory.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ac3forge_build)

add_library(ac3forge_jni SHARED jni_entry.cpp live_cursor.cpp)

target_compile_features(ac3forge_jni PRIVATE cxx_std_23)

# log: android/log.h, used by jni_entry.cpp and by
# src/audio/src/platform/android/passthrough.cpp (linked in transitively via
# ac3::audio, which already declares its own aaudio/log deps - see
# src/audio/CMakeLists.txt's ANDROID branch).
target_link_libraries(ac3forge_jni PRIVATE ac3::forge ac3::audio log)
