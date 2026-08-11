# ---------------------------------------------------------------------------
# ac3::audio - live capture, IEC 61937 passthrough delivery, and shared-mode
# monitor playback. The only OS-specific code in the project: everything that
# actually touches WASAPI or ALSA lives here, not in ac3::forge.
#
# Split out of ac3::forge (see git history) so the codec library itself stays
# platform-independent - a third party linking ac3::forge purely to encode or
# decode AC-3/E-AC-3/Atmos streams needs no WASAPI or ALSA dependency at all,
# and a third party targeting a platform with neither can still use the codec
# without this library existing for their platform yet. ac3::audio is what
# ac3cli/ac3gui link for their live-capture/monitor/passthrough commands
# (record, live, devices, outputs, play, monitor) - it is not part of the
# distributed ac3forge-dev package (see docs/library/index.md).
#
# PRIVATE-links ac3::forge for one reason only: the passthrough backends
# reuse ac3::sinks::iec61937's burst-size constants and framer to size and
# pack S/PDIF bursts. iec61937 itself has no platform dependency and stays in
# ac3::forge - it is pure codec-adjacent byte framing, also used standalone
# by ac3cli's file-based `spdif` command.
#
# Always STATIC, deliberately, regardless of the ambient BUILD_SHARED_LIBS a
# whole-project configure might set for ac3::forge/matroska::matroska's own
# benefit (see src/lib/CMakeLists.txt): this target carries no export
# annotations at all, so building it SHARED produces a DLL with nothing
# exported and no usable import library - a link failure for every in-tree
# consumer, not a working shared ac3audio. It isn't part of the distributed
# package (see the header comment above), so there is no packaging reason to
# give it the same static+shared treatment ac3::forge/matroska::matroska get.
# ---------------------------------------------------------------------------
add_library(ac3audio STATIC)
add_library(ac3::audio ALIAS ac3audio)

target_include_directories(ac3audio
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3audio PUBLIC cxx_std_23)

target_link_libraries(ac3audio PRIVATE ac3::forge ac3::warnings)

# ---------------------------------------------------------------------------
# The platform tree.
#
# Live capture, IEC 61937 passthrough, shared-mode monitor playback, and the
# report saying which of the three exists are the only OS-specific code in
# the project. Each is one file of the SAME name under src/platform/<backend>/,
# and CMake compiles exactly one directory of them - so no translation unit
# ever asks which system it is being built for, and adding a platform means
# adding a directory rather than editing conditionals scattered through the
# sources.
#
# The axis is the audio subsystem rather than the operating system, because
# that is where the answers actually differ: windows/ is WASAPI, alsa/ is
# ALSA, and posix/ is the one that says no to everything. Two of those follow
# from the OS; the third does not, which is the point - a Linux host without
# libasound's development headers falls back to posix/ and still builds a
# complete library, so the dependency is optional rather than required. A
# future pipewire/ or coreaudio/ is a directory and a branch below, and
# nothing else.
#
# AC3FORGE_AUDIO_BACKEND names the selected directory and is published to the
# parent scope: tests/CMakeLists.txt reads it to add that backend's own
# device-free tests, the same way this file adds its sources.
# ---------------------------------------------------------------------------
if(NOT WIN32 AND NOT LINUX AND NOT APPLE)
    message(FATAL_ERROR
        "ac3audio has no platform directory for this system. Add "
        "src/audio/src/platform/<backend>/{audio_backend,capture,passthrough,monitor}.cpp - "
        "src/audio/src/platform/posix/ is the no-backend implementation and is a "
        "working starting point - then select it here.")
endif()

if(WIN32)
    target_sources(ac3audio
        PRIVATE
            src/platform/windows/audio_backend.cpp
            src/platform/windows/capture.cpp
            src/platform/windows/passthrough.cpp
            src/platform/windows/monitor.cpp)

    # How <windows.h> is to be configured wherever it is included: the lean
    # header set, and no min/max macros to collide with <algorithm>. Stated
    # here rather than #defined at the top of each backend, so the two cannot
    # drift apart and a third backend cannot forget.
    target_compile_definitions(ac3audio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

    # WASAPI: COM for the device enumerator, MMCSS for capture-thread priority.
    target_link_libraries(ac3audio PRIVATE ole32 avrt)

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
    # dependency, but a machine that has not installed it must still
    # configure and build - ac3::forge, the codec itself, does not need a
    # sound card to exist. So AC3FORGE_WITH_ALSA defaults to AUTO: a missing
    # ALSA falls back to posix/ with a message saying what was lost, and
    # only -DAC3FORGE_WITH_ALSA=ON turns that into an error - which is what a
    # packaging build wants, so a machine that has quietly lost the headers
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
        target_sources(ac3audio
            PRIVATE
                src/platform/alsa/audio_backend.cpp
                src/platform/alsa/capture.cpp
                src/platform/alsa/monitor.cpp
                src/platform/alsa/passthrough.cpp)
        target_link_libraries(ac3audio PRIVATE ALSA::ALSA)
        set(AC3FORGE_AUDIO_BACKEND alsa)
        message(STATUS
            "ALSA ${ALSA_VERSION_STRING}: live capture, monitor playback and IEC 61937 "
            "passthrough enabled")
    else()
        target_sources(ac3audio
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
    target_sources(ac3audio
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
