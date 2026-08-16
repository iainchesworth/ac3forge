#------------------------------------------------------------------------------
# Emscripten/WASM Toolchain Configuration
#
# Unlike every other toolchain file in this directory, this one is NOT
# chainloaded through vcpkg (compare config-wasm-emscripten's "wasm-emscripten"
# hidden preset in CMakePresets.json against "core": it does not inherit
# "core" at all, so vcpkg.cmake is never the primary toolchain here). vcpkg.json
# itself says why that is fine: ac3::forge's codec code has zero third-party
# dependencies, and the only manifest dependency (catch2) is tests-only, which
# this preset turns off (AC3FORGE_BUILD_TESTS=OFF) - so there is nothing for
# vcpkg to supply a WASM build of, and going through vcpkg's own community
# wasm32-emscripten triplet would only add a slow, fragile catch2-for-wasm
# build for a package nothing here links. This preset's toolchainFile IS this
# file directly.
#
# Requires $EMSDK to be set - `source <emsdk>/emsdk_env.sh` (or
# emsdk_env.bat/.ps1 on Windows) before configuring, same as any other
# Emscripten project.
#------------------------------------------------------------------------------

message(STATUS "Configuring Emscripten/WASM Toolchain")

if(NOT DEFINED ENV{EMSDK})
    message(FATAL_ERROR
        "EMSDK is not set. Install https://github.com/emscripten-core/emsdk and "
        "run `source <emsdk>/emsdk_env.sh` (emsdk_env.bat / emsdk_env.ps1 on "
        "Windows) in the configuring shell before using this preset.")
endif()

set(AC3_EMSCRIPTEN_TOOLCHAIN "$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${AC3_EMSCRIPTEN_TOOLCHAIN}")
    message(FATAL_ERROR "Emscripten.cmake not found at '${AC3_EMSCRIPTEN_TOOLCHAIN}' - "
        "is $EMSDK activated (`emsdk install latest && emsdk activate latest`)?")
endif()

# Does the real work: sets CMAKE_SYSTEM_NAME Emscripten, CMAKE_C(XX)_COMPILER
# to emcc/em++, the sysroot, EMSCRIPTEN==TRUE, etc. Everything below only adds
# to what it sets, never overrides it.
include("${AC3_EMSCRIPTEN_TOOLCHAIN}")
unset(AC3_EMSCRIPTEN_TOOLCHAIN)

message(STATUS "Using Emscripten at: ${CMAKE_CXX_COMPILER}")
