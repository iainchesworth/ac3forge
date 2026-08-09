cmake_minimum_required(VERSION 3.28...4.3)

project(ac3forge
    VERSION 0.2.0
    DESCRIPTION "Clean-room AC-3 (ATSC A/52) encoder and decoder with a spatial object layer"
    LANGUAGES CXX)

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

# NON-CLEAN-ROOM, off by default and NOT tracked on any public branch. src/quarantine
# is gitignored (and a CI job fails if it is ever committed) because it embeds a key
# extracted from Dolby's binary. When this option is ON *and* the local overlay is
# present, ac3cli gains an optional EMDF-protection signer for its Atmos output. A
# normal build neither sees nor references it.
option(AC3FORGE_QUARANTINE_SIGNER
    "Build the RE-derived (non-clean-room, local-only) EMDF Atmos protection signer" OFF)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
# The library holds the entire codec and spatial engine and is GUI-free.
add_subdirectory(src/lib)

# Quarantined, RE-derived signer overlay (off by default; local-only, gitignored).
# After src/lib so it can reuse the codec's parsing primitives; before src/cli so
# ac3cli can link it.
if(AC3FORGE_QUARANTINE_SIGNER)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/quarantine/CMakeLists.txt")
        message(FATAL_ERROR
            "AC3FORGE_QUARANTINE_SIGNER=ON but src/quarantine/ is absent. It is a "
            "local-only overlay (never committed); drop it in before enabling this.")
    endif()
    add_subdirectory(src/quarantine)
endif()

# A standalone container writer, kept apart from the codec on purpose: it
# depends on nothing of ac3::forge and knows nothing about AC-3.
add_subdirectory(src/matroska)

if(AC3FORGE_BUILD_CLI)
    add_subdirectory(src/cli)
endif()

if(AC3FORGE_BUILD_GUI)
    add_subdirectory(src/gui)
endif()

if(AC3FORGE_BUILD_TESTS)
    enable_testing()
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

message(STATUS "ac3forge ${PROJECT_VERSION}")
message(STATUS "  Build CLI      : ${AC3FORGE_BUILD_CLI}")
message(STATUS "  Build GUI      : ${AC3FORGE_BUILD_GUI}")
message(STATUS "  Build tests    : ${AC3FORGE_BUILD_TESTS}")
message(STATUS "  Build examples : ${AC3FORGE_BUILD_EXAMPLES}")
