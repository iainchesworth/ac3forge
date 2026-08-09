# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no platform UI, no third-party
# codec code (see docs/RESEARCH.md on the clean-room rule).
# ---------------------------------------------------------------------------
add_library(ac3forge)
add_library(ac3::forge ALIAS ac3forge)

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
        src/spatial/spatial.cpp)

target_include_directories(ac3forge
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3forge PUBLIC cxx_std_23)

target_link_libraries(ac3forge PRIVATE ac3::warnings)

# ---------------------------------------------------------------------------
# The platform tree.
#
# Live capture, IEC 61937 passthrough, shared-mode monitor playback, and the
# report saying which of the three exists are the only OS-specific code in the
# library. Each is one file of the SAME name under src/platform/<backend>/, and
# CMake compiles exactly one directory of them - so no translation unit ever
# asks which system it is being built for, and adding a platform means adding
# a directory rather than editing conditionals scattered through the sources.
#
# The axis is the audio subsystem rather than the operating system, because
# that is where the answers actually differ: windows/ is WASAPI, alsa/ is ALSA,
# and posix/ is the one that says no to everything. Two of those follow from
# the OS; the third does not, which is the point - a Linux host without
# libasound's development headers falls back to posix/ and still builds a
# complete library, so the dependency is optional rather than required. A
# future pipewire/ or coreaudio/ is a directory and a branch below, and nothing
# else.
#
# AC3FORGE_AUDIO_BACKEND names the selected directory and is published to the
# parent scope: tests/CMakeLists.txt reads it to add that backend's own
# device-free tests, the same way this file adds its sources.
# ---------------------------------------------------------------------------
if(NOT WIN32 AND NOT LINUX AND NOT APPLE)
    message(FATAL_ERROR
        "ac3forge has no platform directory for this system. Add "
        "src/lib/src/platform/<backend>/{audio_backend,capture,passthrough,monitor}.cpp - "
        "src/lib/src/platform/posix/ is the no-backend implementation and is a "
        "working starting point - then select it here.")
endif()

if(WIN32)
    target_sources(ac3forge
        PRIVATE
            src/platform/windows/audio_backend.cpp
            src/platform/windows/capture.cpp
            src/platform/windows/passthrough.cpp
            src/platform/windows/monitor.cpp)

    # How <windows.h> is to be configured wherever it is included: the lean
    # header set, and no min/max macros to collide with <algorithm>. Stated
    # here rather than #defined at the top of each backend, so the two cannot
    # drift apart and a third backend cannot forget.
    target_compile_definitions(ac3forge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

    # WASAPI: COM for the device enumerator, MMCSS for capture-thread priority.
    target_link_libraries(ac3forge PRIVATE ole32 avrt)

    set(AC3FORGE_AUDIO_BACKEND windows)
endif(WIN32)

if(LINUX)
    # -----------------------------------------------------------------------
    # ALSA, if its headers are here.
    #
    # ALSA is the Linux backend rather than PipeWire or PulseAudio because of
    # passthrough specifically: bitstreaming AC-3 means opening a device with
    # the IEC 60958 non-audio bit set, and ALSA is both the layer that
    # expresses that and the layer the other two reach it through. See the
    # essay at the top of src/platform/alsa/passthrough.cpp.
    #
    # Optional, and detected rather than assumed. libasound2-dev (Debian and
    # Ubuntu) or alsa-lib-devel (Fedora) is a small and universally packaged
    # dependency, but a machine that has not installed it must still configure
    # and build - the codec, which is the whole point of the library, does not
    # need a sound card to exist. So AC3FORGE_WITH_ALSA defaults to AUTO: a
    # missing ALSA falls back to posix/ with a message saying what was lost,
    # and only -DAC3FORGE_WITH_ALSA=ON turns that into an error - which is what
    # a packaging build wants, so a machine that has quietly lost the headers
    # fails loudly rather than shipping a binary with no audio in it.
    #
    # find_package(ALSA) is CMake's own module, not a vcpkg port: this is a
    # system library that comes with the distribution, and vcpkg.json stays
    # free of it.
    # -----------------------------------------------------------------------
    set(AC3FORGE_WITH_ALSA "AUTO" CACHE STRING
        "ALSA backend: AUTO uses it when found, ON requires it, OFF never builds it")
    set_property(CACHE AC3FORGE_WITH_ALSA PROPERTY STRINGS AUTO ON OFF)

    if(AC3FORGE_WITH_ALSA STREQUAL "OFF")
        set(ALSA_FOUND FALSE)
    elseif(AC3FORGE_WITH_ALSA STREQUAL "ON")
        find_package(ALSA REQUIRED)
    else()
        find_package(ALSA)
    endif()

    if(ALSA_FOUND)
        target_sources(ac3forge
            PRIVATE
                src/platform/alsa/audio_backend.cpp
                src/platform/alsa/capture.cpp
                src/platform/alsa/monitor.cpp
                src/platform/alsa/passthrough.cpp)
        target_link_libraries(ac3forge PRIVATE ALSA::ALSA)
        set(AC3FORGE_AUDIO_BACKEND alsa)
        message(STATUS
            "ALSA ${ALSA_VERSION_STRING}: live capture, monitor playback and IEC 61937 "
            "passthrough enabled")
    else()
        target_sources(ac3forge
            PRIVATE
                src/platform/posix/audio_backend.cpp
                src/platform/posix/capture.cpp
                src/platform/posix/monitor.cpp
                src/platform/posix/passthrough.cpp)
        set(AC3FORGE_AUDIO_BACKEND posix)
        message(STATUS
            "ALSA headers not found; building without live capture, monitor playback or "
            "IEC 61937 passthrough. Install libasound2-dev (Debian/Ubuntu) or alsa-lib-devel "
            "(Fedora) to enable them, or set -DAC3FORGE_WITH_ALSA=ON to make their absence "
            "an error.")
    endif()
endif(LINUX)

if(APPLE)
    # CoreAudio would go here; until it does, macOS gets the no-backend files
    # and 'ac3cli play' says so instead of failing to link.
    target_sources(ac3forge
        PRIVATE
            src/platform/posix/audio_backend.cpp
            src/platform/posix/capture.cpp
            src/platform/posix/monitor.cpp
            src/platform/posix/passthrough.cpp)

    set(AC3FORGE_AUDIO_BACKEND posix)
endif(APPLE)

set(AC3FORGE_AUDIO_BACKEND "${AC3FORGE_AUDIO_BACKEND}" PARENT_SCOPE)
set(AC3FORGE_AUDIO_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${AC3FORGE_AUDIO_BACKEND}"
    PARENT_SCOPE)
