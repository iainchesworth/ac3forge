cmake_minimum_required(VERSION 3.28...4.3)

# Derive the version from the nearest reachable v* git tag before project() -
# see the file for the fallback/override rules. This makes a pushed tag the
# single source of truth: nothing in this tree needs bumping by hand anymore.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersionDerivation.cmake")

project(ac3forge
    VERSION ${DERIVED_VERSION_SEMVER}
    DESCRIPTION "Clean-room AC-3 (ATSC A/52) encoder and decoder with a spatial object layer"
    LANGUAGES CXX)

# CMake's project() VERSION field can only hold M.M.P, so a tag's prerelease
# suffix (e.g. the "-beta.1" in v0.2.0-beta.1) is carried separately here.
set(PROJECT_VERSION_PRERELEASE "${DERIVED_VERSION_PRERELEASE}" CACHE STRING "Prerelease label (e.g. beta.2)" FORCE)
set(PROJECT_VERSION_FULL "${DERIVED_VERSION_FULL}" CACHE STRING "Full semver version (M.M.P[-prerelease])" FORCE)

# vcpkg.json's "version" is a fixed placeholder, deliberately never kept in
# sync with PROJECT_VERSION: it is only read by vcpkg's manifest resolver
# before any CMake configure step runs, and this project is never published
# to a vcpkg registry, so the field has no real meaning to synchronise.
# Matches R:\aqualink-automate, whose vcpkg.json version sits equally static.

# ---------------------------------------------------------------------------
# Global language settings: C++23 only, no compiler extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One place for runtime artefacts so the Qt app finds its plugins and the CLI
# and tests sit beside it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Empty unless -DAC3FORGE_SANITIZERS=<comma-list> is set (see the *-asan-ubsan
# presets in CMakePresets.json); a plain configure is unaffected.
set(AC3FORGE_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= value, e.g. address,undefined")
include(Sanitizers)

# ac3::coverage, off unless -DAC3FORGE_ENABLE_COVERAGE=ON (see the
# linux-gcc-coverage preset in CMakePresets.json); a plain configure is unaffected.
include(Coverage)

# ac3::tracy, off unless -DAC3FORGE_ENABLE_TRACY=ON (and vcpkg's "profiling"
# manifest feature resolved - see vcpkg.json); a plain configure is unaffected.
include(Tracy)

# Sources carry spec citations with non-ASCII glyphs (section marks, degrees,
# arrows). Tell MSVC both the source and execution charsets are UTF-8 so it
# neither mis-decodes them nor warns; harmless everywhere else.
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(AC3FORGE_BUILD_CLI "Build the ac3cli command-line application" ON)
option(AC3FORGE_BUILD_GUI "Build the Qt6 Quick GUI application" ON)
option(AC3FORGE_BUILD_TESTS "Build the unit test suite" ON)
# Catch2 is the only build-time dependency. Turn this off to insist it comes
# from a package manager instead of the network (see tests/CMakeLists.txt).
option(AC3FORGE_FETCH_CATCH2 "Download Catch2 when no local copy is found" ON)
option(AC3FORGE_BUILD_EXAMPLES "Build the documented library examples" ON)
# libFuzzer harnesses over ac3::forge's untrusted-input entry points (see
# fuzz/). Off by default: it needs upstream Clang (GCC and MSVC do not ship
# libFuzzer) and instruments ac3forge itself with ASan+UBSan, which no other
# configuration wants paid for. Use fuzz/run.sh rather than this option
# directly - it configures a dedicated build/fuzz directory with the right
# compiler.
option(AC3FORGE_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only; see fuzz/)" OFF)
# Every other library component (matroska::matroska, mp4::mp4, mpegts::mpegts) gets its own
# AC3FORGE_BUILD_<NAME> option, defaulting ON so nothing changes for today's direct/CPack builds.
# This is what lets a vcpkg port expose a component as an opt-in feature - see
# cmake/InstallLibrary.cmake's matching install()/export() gate.
option(AC3FORGE_BUILD_MATROSKA "Build the standalone Matroska container writer (src/matroska)" ON)
option(AC3FORGE_BUILD_MP4 "Build the standalone MP4/ISOBMFF container writer (src/mp4)" ON)
option(AC3FORGE_BUILD_MPEGTS "Build the standalone MPEG-TS container writer (src/mpegts)" ON)

# src/cli, src/gui, tests/ and examples/ all link matroska::matroska, mp4::mp4 and
# mpegts::mpegts unconditionally - none of them are written to cope with any of the three being
# absent. AC3FORGE_BUILD_MATROSKA=OFF, AC3FORGE_BUILD_MP4=OFF or AC3FORGE_BUILD_MPEGTS=OFF only
# make sense with all four also OFF (this is the shape a vcpkg build without the matching
# feature actually uses - mp4/mpegts have no vcpkg feature of their own yet, see
# cmake/InstallLibrary.cmake); catch the unsupported combination here with a clear message
# rather than letting it fail deep inside one of those subdirectories with a "target not found"
# error.
if(NOT AC3FORGE_BUILD_MATROSKA AND (AC3FORGE_BUILD_CLI OR AC3FORGE_BUILD_GUI OR AC3FORGE_BUILD_TESTS OR AC3FORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "AC3FORGE_BUILD_MATROSKA=OFF requires AC3FORGE_BUILD_CLI, "
        "AC3FORGE_BUILD_GUI, AC3FORGE_BUILD_TESTS and AC3FORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link matroska::matroska unconditionally.")
endif()
if(NOT AC3FORGE_BUILD_MP4 AND (AC3FORGE_BUILD_CLI OR AC3FORGE_BUILD_GUI OR AC3FORGE_BUILD_TESTS OR AC3FORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "AC3FORGE_BUILD_MP4=OFF requires AC3FORGE_BUILD_CLI, "
        "AC3FORGE_BUILD_GUI, AC3FORGE_BUILD_TESTS and AC3FORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link mp4::mp4 unconditionally.")
endif()
if(NOT AC3FORGE_BUILD_MPEGTS AND (AC3FORGE_BUILD_CLI OR AC3FORGE_BUILD_GUI OR
                                  AC3FORGE_BUILD_TESTS OR AC3FORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "AC3FORGE_BUILD_MPEGTS=OFF requires AC3FORGE_BUILD_CLI, "
        "AC3FORGE_BUILD_GUI, AC3FORGE_BUILD_TESTS and AC3FORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link mpegts::mpegts unconditionally.")
endif()

# ac3adm::ac3adm (src/ac3adm) is a READER, not a writer like the three container modules above -
# roadmap item B1 phase 1 ("ADM BWF reader feeding the JOC encoder", see ROADMAP.md), parsing
# BW64/RF64 + the Audio Definition Model rather than muxing a container. Unlike
# matroska/mp4/mpegts, it defaults OFF rather than ON and is genuinely additive: nothing in
# src/cli, src/gui, tests/ or examples/ links it unconditionally, so every one of those builds
# identically whether this is on or off. The reason for the different default is its
# dependency footprint - see src/ac3adm/CMakeLists.txt's own header comment - libadm needs
# several Boost header libraries, resolved through a dedicated opt-in vcpkg feature
# (-DVCPKG_MANIFEST_FEATURES=adm, see vcpkg.json) that a normal build never pulls in.
option(AC3FORGE_BUILD_ADM "Build the standalone BW64/RF64 + ADM parser (src/ac3adm) - needs Boost, see vcpkg.json's \"adm\" feature" OFF)

# enable_testing() has to run before ANY add_subdirectory() that might contain
# an add_test() call, not merely before tests/ itself - CTest silently drops
# an add_test() registered from a directory processed earlier in the same
# configure pass (confirmed the hard way: src/gui/tests/'s ac3gui_qml_tests
# was invisible to `ctest -N` until this moved here). Calling it unconditionally
# is harmless when AC3FORGE_BUILD_TESTS is OFF - nothing calls add_test() then.
if(AC3FORGE_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
# The library holds the entire codec and spatial engine and is GUI-free.
add_subdirectory(src/lib)

# Live capture, monitor playback and IEC 61937 passthrough - the only
# platform-specific code in the project. After src/lib (PRIVATE-links
# ac3::forge for the IEC 61937 framer); before src/cli/src/gui/tests, which
# link it alongside ac3::forge for their live-audio commands/features. Not
# linked by examples/ - none of them touch live audio.
#
# Skipped entirely under Emscripten: src/audio/CMakeLists.txt hard-fails
# configure when none of WIN32/LINUX/APPLE/ANDROID is set (it has no browser
# platform directory, and never will - a WASM build gets live audio from the
# Web Audio API in JS instead, not from this library), and nothing a WASM
# configure builds needs it anyway (AC3FORGE_BUILD_CLI/GUI/TESTS are all OFF
# there, and examples/wasm_decode_demo does not link ac3::audio).
if(NOT EMSCRIPTEN)
    add_subdirectory(src/audio)
endif()

# The EMDF Atmos object signer (ac3::signing). Clean-room and dependency-free:
# the HMAC construction and authenticated-region layout are in-tree, and the
# only secret - the key - is supplied by the operator at runtime, never
# embedded, so unlike the old quarantine overlay this is committed and built by
# default. After src/lib so it can reuse the codec's parsing primitives; before
# src/cli so ac3cli can link it.
add_subdirectory(src/signing)

# A standalone container writer, kept apart from the codec on purpose: it
# depends on nothing of ac3::forge and knows nothing about AC-3. Gated by
# AC3FORGE_BUILD_MATROSKA (default ON) so it can be opted out of - a vcpkg
# port maps this straight to its own "matroska" feature.
if(AC3FORGE_BUILD_MATROSKA)
    add_subdirectory(src/matroska)
endif()

# A second standalone container writer, same shape and same reasoning as
# src/matroska above - see src/mp4/CMakeLists.txt's own header. Gated by
# AC3FORGE_BUILD_MP4 (default ON, see the FATAL_ERROR guard above).
if(AC3FORGE_BUILD_MP4)
    add_subdirectory(src/mp4)
endif()

# A third standalone container writer, same shape as src/matroska above but
# for MPEG-2 Transport Stream rather than Matroska. Gated by AC3FORGE_BUILD_MPEGTS
# (default ON, see the option() and its FATAL_ERROR guard above) so it can be
# opted out of the same way matroska is expected to become opt-out-able too.
if(AC3FORGE_BUILD_MPEGTS)
    add_subdirectory(src/mpegts)
endif()

# A standalone BW64/RF64 + Audio Definition Model parser - reader, not writer, but the same
# "depends on nothing of ac3::forge" shape as the three container writers above. Gated by
# AC3FORGE_BUILD_ADM (default OFF, unlike the three above - see the option() itself for why).
if(AC3FORGE_BUILD_ADM)
    add_subdirectory(src/ac3adm)

    # ac3::admbridge - roadmap item B1 phase 2: maps ac3adm's parsed ADM graph onto
    # ac3::oba::AtmosEncoder. Depends on BOTH ac3::forge (added above) and ac3adm::ac3adm (just
    # added), so it is meaningless without AC3FORGE_BUILD_ADM - gated by the same flag rather
    # than a new option of its own; see src/adm_bridge/CMakeLists.txt's own header comment for
    # the full reasoning, including why this is a separate module from either side.
    add_subdirectory(src/adm_bridge)
endif()

# install()/find_package(ac3forge) support for ac3::forge, matroska::matroska,
# mp4::mp4 and mpegts::mpegts - after every add_subdirectory() call above (needs their targets to
# exist), before include(Packaging) (CPack's library component packages exactly what gets
# install()'d here). Deliberately not ac3::audio, and deliberately not ac3adm::ac3adm either -
# see cmake/InstallLibrary.cmake's own header comment for the former; ac3adm is left out of the
# installed package for the same "FetchContent'd third-party code, not this project's own" reason
# it stays out of docs/library/index.md's zero-dependency claim (see src/ac3adm/CMakeLists.txt's
# header comment) plus the added complexity of re-exporting libbw64/libadm's own transitive
# targets through this package - AC3FORGE_BUILD_ADM users consume ac3adm::ac3adm from an in-tree
# add_subdirectory() build, not from a `find_package(ac3forge)` installed one, for now. ac3::admbridge
# (src/adm_bridge) is excluded for the same reason - it transitively depends on ac3adm::ac3adm.
include(InstallLibrary)

if(AC3FORGE_BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(AC3FORGE_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(AC3FORGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# After the tests block: the examples register themselves as tests, which needs
# enable_testing() to have run.
if(AC3FORGE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AC3FORGE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# The browser decode demo (roadmap F3, docs/platforms/wasm.md) - a platform
# app, same shape and reasoning as platform/android/'s Shield app: a small
# demo consuming ac3::forge unmodified, kept out of both src/ (not library
# code) and examples/ (not a single-API-call snippet backing a
# docs/library/*.md page, see CONTRIBUTING.md). Unlike platform/android/,
# which is a wholly separate Gradle/NDK build reached from the other
# direction, this platform has no non-CMake build system to work around, so
# it is a normal add_subdirectory() gated on EMSCRIPTEN (set by
# cmake/toolchains/wasm.emscripten.toolchain.cmake) rather than an
# AC3FORGE_BUILD_* option - there is nothing to opt in or out of on a host
# that isn't targeting WASM in the first place.
if(EMSCRIPTEN)
    add_subdirectory(platform/wasm)
endif()

# After every target's install() rules (src/cli, src/gui) have been declared.
include(Packaging)

message(STATUS "ac3forge ${PROJECT_VERSION_FULL}")
message(STATUS "  Build CLI      : ${AC3FORGE_BUILD_CLI}")
message(STATUS "  Build GUI      : ${AC3FORGE_BUILD_GUI}")
message(STATUS "  Build tests    : ${AC3FORGE_BUILD_TESTS}")
message(STATUS "  Build examples : ${AC3FORGE_BUILD_EXAMPLES}")
# Which src/audio/src/platform/ directory was compiled, and so whether 'record'
# and 'play' work in this build at all - see the platform tree in
# src/audio/CMakeLists.txt. 'posix' is the one that means no.
message(STATUS "  Audio backend  : ${AC3FORGE_AUDIO_BACKEND}")
