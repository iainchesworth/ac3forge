# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no third-party codec code (see
# docs/RESEARCH.md on the clean-room rule) - and, deliberately, no
# platform-specific code at all: this target builds identically on any
# C++23 platform, with no WASAPI/ALSA dependency, so a third party linking it
# purely to encode/decode AC-3/E-AC-3/Atmos streams needs nothing else. Live
# capture, monitor playback and IEC 61937 delivery are ac3::audio
# (src/audio/), a separate target ac3cli/ac3gui link alongside this one -
# see that target's own header comment.
# ---------------------------------------------------------------------------
add_library(ac3forge)
add_library(ac3::forge ALIAS ac3forge)

# --- Version + git stamp ---------------------------------------------------
find_package(Git QUIET)
set(AC3FORGE_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/include/ac3/version.hpp.in")
set(AC3FORGE_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/version.hpp")

# OS/architecture/compiler this binary is built for, for version_details()'s
# --version/About-dialog output. Computed here (CMake already knows all three
# from the toolchain it configured) rather than read back with predefined
# macros at compile time, so version.cpp needs no #ifdef - see ac3::audio's
# platform tree (src/audio/CMakeLists.txt) and scripts/check-platform-macros.ps1.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(AC3FORGE_BUILD_OS "macOS")
else()
    set(AC3FORGE_BUILD_OS "${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(AC3FORGE_BUILD_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(AC3FORGE_BUILD_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i686|X86)$")
    set(AC3FORGE_BUILD_ARCH "x86")
else()
    set(AC3FORGE_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(AC3FORGE_BUILD_COMPILER "GCC ${CMAKE_CXX_COMPILER_VERSION}")
else()
    # AppleClang, Clang and MSVC already spell their own CMAKE_CXX_COMPILER_ID
    # the way version_details() wants it.
    set(AC3FORGE_BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(AC3FORGE_BUILD_TARGET "${AC3FORGE_BUILD_OS} ${AC3FORGE_BUILD_ARCH} (${AC3FORGE_BUILD_COMPILER})")

# Generate once at configure time so the header exists for the first build.
set(AC3FORGE_VERSION "${PROJECT_VERSION}")
set(AC3FORGE_VERSION_FULL "${PROJECT_VERSION_FULL}")
set(SRC "${AC3FORGE_VERSION_TEMPLATE}")
set(DST "${AC3FORGE_VERSION_HEADER}")
set(WORKDIR "${PROJECT_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake")

# Re-stamp on every build so the git information tracks new commits without a
# reconfigure. configure_file only rewrites when the content changes, so this
# does not needlessly recompile dependants.
add_custom_target(ac3forge_version ALL
    BYPRODUCTS "${AC3FORGE_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DAC3FORGE_VERSION=${PROJECT_VERSION}"
        "-DAC3FORGE_VERSION_FULL=${PROJECT_VERSION_FULL}"
        "-DAC3FORGE_BUILD_TARGET=${AC3FORGE_BUILD_TARGET}"
        "-DSRC=${AC3FORGE_VERSION_TEMPLATE}"
        "-DDST=${AC3FORGE_VERSION_HEADER}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DWORKDIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake"
    COMMENT "Stamping version and git information"
    VERBATIM)

# Everything here is portable: bitstream, DSP, containers, measurement. The
# OS-specific code is selected further down, out of this list.
target_sources(ac3forge
    PRIVATE
        src/analysis/levels.cpp
        src/core/bitalloc.cpp
        src/core/eac3_tables.cpp
        src/core/exponents.cpp
        src/core/mantissas.cpp
        src/core/mdct.cpp
        src/decoder/decoder.cpp
        src/decoder/eac3_decoder.cpp
        src/emdf/emdf.cpp
        src/encoder/coupling.cpp
        src/encoder/eac3_frame.cpp
        src/encoder/eac3_tools.cpp
        src/encoder/encoder.cpp
        src/encoder/plan.cpp
        src/encoder/transient.cpp
        src/io/elementary.cpp
        src/io/wav.cpp
        src/meta/drc.cpp
        src/meta/loudness.cpp
        src/meta/mixing.cpp
        src/oba/atmos.cpp
        src/oba/joc.cpp
        src/oba/motion.cpp
        src/oba/oamd.cpp
        src/sinks/iec61937.cpp
        src/spatial/spatial.cpp
        src/version.cpp)

add_dependencies(ac3forge ac3forge_version)

target_include_directories(ac3forge
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3forge PUBLIC cxx_std_23)

target_link_libraries(ac3forge PRIVATE ac3::warnings ac3::coverage)
