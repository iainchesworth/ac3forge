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
# reuse ac3::iec61937's burst-size constants and framer to size and
# pack S/PDIF bursts. iec61937 itself has no platform dependency and stays in
# ac3::forge - it is pure codec-adjacent byte framing, also used standalone
# by ac3cli's file-based `spdif` command.
#
# Always STATIC, deliberately, regardless of the ambient BUILD_SHARED_LIBS a
# whole-project configure might set for ac3::forge/matroska::matroska's own
# benefit (see src/forge/CMakeLists.txt): this target carries no export
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
# The backend tree.
#
# Live capture, IEC 61937 passthrough, shared-mode monitor playback, and the
# report saying which of the three exists are the only OS-specific code in
# the project. Each is one file of the SAME name under src/backend/<backend>/,
# and CMake compiles exactly one directory of them - so no translation unit
# ever asks which system it is being built for, and adding a platform means
# adding a directory rather than editing conditionals scattered through the
# sources.
#
# The axis is the audio subsystem rather than the operating system, because
# that is where the answers actually differ: windows/ is WASAPI, alsa/ and
# pipewire/ are Linux's two real backends, macos/ is CoreAudio, and posix/ is
# the one that says no to everything (Linux only, now that macOS has its own
# directory - see below). posix/'s existence is not tied to the OS the way
# the other three are, which is the point - a Linux host with neither
# libasound's nor libpipewire's development headers falls back to it and
# still builds a complete library, so both Linux dependencies are optional
# rather than required.
#
# AC3FORGE_AUDIO_BACKEND names the selected directory and is published to the
# parent scope: tests/CMakeLists.txt reads it to add that backend's own
# device-free tests, the same way this file adds its sources.
# ---------------------------------------------------------------------------
if(NOT WIN32 AND NOT LINUX AND NOT APPLE AND NOT ANDROID)
    message(FATAL_ERROR
        "ac3audio has no backend directory for this system. Add "
        "src/audio/src/backend/<backend>/{audio_backend,capture,passthrough,monitor}.cpp - "
        "src/audio/src/backend/posix/ is the no-backend implementation and is a "
        "working starting point - then select it here.")
endif()

if(WIN32)
    target_sources(ac3audio
        PRIVATE
            src/backend/windows/audio_backend.cpp
            src/backend/windows/capture.cpp
            src/backend/windows/passthrough.cpp
            src/backend/windows/monitor.cpp)

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
    # ALSA is the Linux backend rather than PulseAudio because of passthrough
    # specifically: bitstreaming AC-3 means opening a device with the IEC
    # 60958 non-audio bit set, and ALSA is both the layer that expresses that
    # and the layer PulseAudio (and, per the PipeWire block below,
    # ordinarily PipeWire too) reaches it through. See the essay at the top
    # of src/backend/alsa/passthrough.cpp.
    #
    # Optional, and detected rather than assumed. libasound2-dev (Debian and
    # Ubuntu) or alsa-lib-devel (Fedora) is a small and universally packaged
    # dependency, but a machine that has not installed it must still
    # configure and build - ac3::forge, the codec itself, does not need a
    # sound card to exist. So AC3FORGE_WITH_ALSA defaults to AUTO: a missing
    # ALSA falls back to pipewire/ or posix/ with a message saying what was
    # lost, and only -DAC3FORGE_WITH_ALSA=ON turns that into an error - which
    # is what a packaging build wants, so a machine that has quietly lost the
    # headers fails loudly rather than shipping a binary with no audio in it.
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

    # -----------------------------------------------------------------------
    # PipeWire, if its headers are here.
    #
    # A real second Linux backend, not a shim over the one above: capture.cpp
    # and monitor.cpp are native pw_stream PCM, and passthrough.cpp genuinely
    # does negotiate compressed IEC 61937 formats through PipeWire's own API
    # (SPA_MEDIA_SUBTYPE_iec958, spa_format_audio_iec958_build(),
    # PW_STREAM_FLAG_EXCLUSIVE) - see the essay at the top of
    # src/backend/pipewire/passthrough.cpp for what that can and cannot
    # promise in practice, which is the real reason ALSA keeps first
    # precedence below rather than "PipeWire is what most desktops run
    # today" winning by default.
    #
    # Same AUTO/ON/OFF shape as AC3FORGE_WITH_ALSA and resolved the same
    # independent way - probed (or required, or skipped) on its own, with no
    # regard for whether ALSA was also found - so each option's own meaning
    # never depends on the other's outcome; only the SELECTION below (which
    # one of a found ALSA/PipeWire/neither actually gets compiled) considers
    # both at once.
    #
    # pkg-config, not find_package(): PipeWire ships no CMake config package
    # of its own, only a pkg-config .pc file (libpipewire-0.3, from
    # libpipewire-0.3-dev on Debian/Ubuntu or pipewire-devel on Fedora) - the
    # same system-library-not-a-vcpkg-port reasoning as ALSA, via CMake's
    # own FindPkgConfig module instead of a dedicated Find module because
    # upstream provides no such module to find.
    # -----------------------------------------------------------------------
    set(AC3FORGE_WITH_PIPEWIRE "AUTO" CACHE STRING
        "PipeWire backend: AUTO uses it when found, ON requires it, OFF never builds it")
    set_property(CACHE AC3FORGE_WITH_PIPEWIRE PROPERTY STRINGS AUTO ON OFF)

    if(AC3FORGE_WITH_PIPEWIRE STREQUAL "OFF")
        set(PipeWire_FOUND FALSE)
    else()
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            # GLOBAL: tests/CMakeLists.txt links PkgConfig::PipeWire too (its
            # own translation unit needs PipeWire's headers), and tests/ is a
            # sibling of this directory under the top-level CMakeLists.txt,
            # not a descendant - an imported target created without GLOBAL is
            # only visible to this directory and its own subdirectories, so
            # tests/ would not see it otherwise.
            if(AC3FORGE_WITH_PIPEWIRE STREQUAL "ON")
                pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET GLOBAL libpipewire-0.3)
            else()
                pkg_check_modules(PipeWire QUIET IMPORTED_TARGET GLOBAL libpipewire-0.3)
            endif()
        elseif(AC3FORGE_WITH_PIPEWIRE STREQUAL "ON")
            message(FATAL_ERROR
                "AC3FORGE_WITH_PIPEWIRE=ON but pkg-config itself was not found, and it is what "
                "locates libpipewire-0.3. Install pkg-config, or set AC3FORGE_WITH_PIPEWIRE=AUTO "
                "or =OFF.")
        else()
            set(PipeWire_FOUND FALSE)
        endif()
    endif()

    # -----------------------------------------------------------------------
    # Selection: three possible directories now, so one precedence rule picks
    # exactly one. ALSA wins whenever both are present - not because PipeWire
    # is somehow lesser, but because ALSA's passthrough works unconditionally
    # the moment compatible hardware exists, while PipeWire's compressed
    # codec support has to have been separately enabled on the target node by
    # the session manager first (see src/backend/pipewire/passthrough.cpp's
    # header comment) - so preferring PipeWire by default on every machine
    # that merely has both sets of headers installed would silently regress
    # 'ac3cli outputs'/'play' on the common case where nobody has touched
    # that setting, even though the very ALSA device those commands want is
    # sitting right there, underneath the PipeWire daemon that's running.
    # Capture and monitor playback are equally real on both, so passthrough
    # is the only thing this ordering is actually deciding.
    #
    # The escape hatch for a machine where PipeWire is deliberately preferred
    # anyway (its compressed codecs ARE configured, say) is explicit rather
    # than inferred from which cache variable looks "more default":
    # -DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON forces the
    # selection the other way, the same way -DAC3FORGE_WITH_ALSA=OFF alone
    # already forces posix/ today.
    # -----------------------------------------------------------------------
    if(ALSA_FOUND)
        target_sources(ac3audio
            PRIVATE
                src/backend/alsa/audio_backend.cpp
                src/backend/alsa/capture.cpp
                src/backend/alsa/monitor.cpp
                src/backend/alsa/passthrough.cpp)
        target_link_libraries(ac3audio PRIVATE ALSA::ALSA)
        set(AC3FORGE_AUDIO_BACKEND alsa)
        message(STATUS
            "ALSA ${ALSA_VERSION_STRING}: live capture, monitor playback and IEC 61937 "
            "passthrough enabled")
    elseif(PipeWire_FOUND)
        target_sources(ac3audio
            PRIVATE
                src/backend/pipewire/audio_backend.cpp
                src/backend/pipewire/capture.cpp
                src/backend/pipewire/monitor.cpp
                src/backend/pipewire/passthrough.cpp)
        target_link_libraries(ac3audio PRIVATE PkgConfig::PipeWire)
        set(AC3FORGE_AUDIO_BACKEND pipewire)
        message(STATUS
            "PipeWire ${PipeWire_VERSION}: live capture and monitor playback enabled; IEC "
            "61937 passthrough negotiates for real but needs a compressed codec enabled on "
            "the target node by the session manager first - see "
            "src/backend/pipewire/passthrough.cpp")
    else()
        target_sources(ac3audio
            PRIVATE
                src/backend/posix/audio_backend.cpp
                src/backend/posix/capture.cpp
                src/backend/posix/monitor.cpp
                src/backend/posix/passthrough.cpp)
        set(AC3FORGE_AUDIO_BACKEND posix)
        message(STATUS
            "Neither ALSA nor PipeWire headers were found; building without live capture, "
            "monitor playback or IEC 61937 passthrough. Install libasound2-dev/alsa-lib-devel "
            "or libpipewire-0.3-dev/pipewire-devel to enable them, or set "
            "-DAC3FORGE_WITH_ALSA=ON / -DAC3FORGE_WITH_PIPEWIRE=ON to make their absence an "
            "error.")
    endif()
endif(LINUX)

if(APPLE)
    # -----------------------------------------------------------------------
    # CoreAudio: the Audio HAL (AudioObjectID / AudioDeviceIOProc), the same
    # layer WASAPI and ALSA occupy on their own platforms - not AVAudioEngine
    # or Audio Queue Services. See src/backend/macos/capture.cpp's own
    # header for why: passthrough's exclusive-mode physical-format switch
    # has to happen at the HAL, so every capability shares that one layer
    # (and its device-enumeration/hog-mode code, coreaudio_support.hpp)
    # rather than each reaching for a different API.
    #
    # Always available and never optional the way ALSA is on Linux: CoreAudio
    # is part of every macOS SDK, with no missing-headers story to fall back
    # from - see src/backend/macos/audio_backend.cpp's own comment on what
    # "available" still does and does not promise.
    # -----------------------------------------------------------------------
    target_sources(ac3audio
        PRIVATE
            src/backend/macos/audio_backend.cpp
            src/backend/macos/capture.cpp
            src/backend/macos/passthrough.cpp
            src/backend/macos/monitor.cpp)

    # CoreAudio: the Audio HAL itself. CoreFoundation: CFStringRef, used
    # throughout coreaudio_support.hpp for every device's persistent UID and
    # friendly name. Apple frameworks are linked as plain "-framework <name>"
    # strings rather than through find_library/find_package - there is
    # nothing to find, they ship with every macOS SDK.
    target_link_libraries(ac3audio PRIVATE "-framework CoreAudio" "-framework CoreFoundation")

    set(AC3FORGE_AUDIO_BACKEND macos)
endif(APPLE)

if(ANDROID)
    # -----------------------------------------------------------------------
    # NVIDIA Shield / Android TV, for the Shield Atmos demo app
    # (platform/android/app/) - see docs/platforms/android.md.
    #
    # Split unlike the other three backends: monitor.cpp is genuine AAudio
    # (the NDK's native audio API), but passthrough.cpp is a JNI shim to a
    # Kotlin-owned android.media.AudioTrack, because AAudio has no support
    # for compressed/IEC 61937 bitstream passthrough at all - see
    # src/backend/android/audio_backend.cpp's header comment for the full
    # reasoning. capture.cpp is the no-backend stub: this build has no
    # microphone/loopback feature to serve.
    #
    # ANDROID is set by CMake itself (like WIN32/APPLE) when the NDK's
    # toolchain file sets CMAKE_SYSTEM_NAME to "Android" - a peer of the
    # WIN32/LINUX/APPLE checks above, not a special case. Note CMAKE_SYSTEM_
    # NAME is "Android", not "Linux", for an NDK cross-compile, so the LINUX
    # block above is never entered here.
    # -----------------------------------------------------------------------
    target_sources(ac3audio
        PRIVATE
            src/backend/android/audio_backend.cpp
            src/backend/android/capture.cpp
            src/backend/android/passthrough.cpp
            src/backend/android/monitor.cpp)

    # aaudio: monitor.cpp's native playback API. log: android/log.h, used
    # throughout the backend for diagnostics visible in `adb logcat`.
    target_link_libraries(ac3audio PRIVATE aaudio log)

    set(AC3FORGE_AUDIO_BACKEND android)
endif(ANDROID)

set(AC3FORGE_AUDIO_BACKEND "${AC3FORGE_AUDIO_BACKEND}" PARENT_SCOPE)
set(AC3FORGE_AUDIO_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/${AC3FORGE_AUDIO_BACKEND}"
    PARENT_SCOPE)
