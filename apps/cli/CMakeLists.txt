# ---------------------------------------------------------------------------
# ac3cli - the command-line front end over ac3::forge.
# ---------------------------------------------------------------------------
add_executable(ac3cli main.cpp support.cpp multi_source.cpp commands/containers.cpp commands/audio_io.cpp commands/live_audio.cpp commands/synth.cpp)

# ac3cli's platform-specific code: putting stdin/stdout into binary mode for
# the "-" stdin/stdout file-argument convention (see platform/stdio_binary.hpp
# for why Windows needs a real call and POSIX doesn't). Selected here rather
# than behind an #ifdef in main.cpp, the same platform-tree pattern ac3::audio
# uses for WASAPI/ALSA (src/audio/CMakeLists.txt) - tools/checks/check_platform_macros.ps1
# enforces there being no third option.
if(WIN32)
    target_sources(ac3cli PRIVATE platform/windows/stdio_binary.cpp)
else()
    target_sources(ac3cli PRIVATE platform/posix/stdio_binary.cpp)
endif()

# main.cpp's 'atmos-adm' command (roadmap B1 phase 3) needs ac3adm::ac3adm/ac3::admbridge - this
# project's one optional, non-default library dependency (AC3FORGE_BUILD_ADM, default OFF - see
# the root CMakeLists.txt's own option() for why: libadm's Boost dependency). Unlike
# matroska::matroska/mp4::mp4/mpegts::mpegts below (default ON, and a FATAL_ERROR guard in the
# root CMakeLists.txt refuses to build this target at all without them), ac3cli builds and works
# perfectly well with this flag off - it just lacks the one command, refused with a clear reason at
# dispatch time (main.cpp's Needs::kAdm/unmet()) rather than missing from the binary's usage
# listing entirely.
#
# main.cpp itself never names ac3adm::ac3adm/ac3::admbridge or includes either of their headers -
# not even behind a preprocessor guard, since tools/checks/check_platform_macros.ps1 (CI-enforced)
# refuses ANY #if/#ifdef/#ifndef anywhere under src/, feature flags included, not just platform
# ones (that script's own header comment). So the same platform-tree shape the WIN32/else split
# above already uses for stdio_binary.cpp applies here too, for a library-linked-or-not axis
# instead of an OS one: adm/atmos_adm.hpp declares the narrow, ac3adm/admbridge-type-free seam
# main.cpp calls unconditionally (ac3cli::load_adm_atmos_source(), ac3cli::adm_capability()), and
# exactly one of the two files below actually implements it - never both, never neither - so
# ac3cli links successfully and behaves correctly either way with zero preprocessor conditionals
# anywhere in this target's sources. See adm/atmos_adm.hpp's own top comment for the full
# reasoning, including why this replaced an earlier #ifdef-based design once
# tools/checks/check_platform_macros.ps1 was actually run against it.
if(AC3FORGE_BUILD_ADM)
    target_sources(ac3cli PRIVATE adm/enabled/atmos_adm.cpp)
    target_link_libraries(ac3cli PRIVATE ac3adm::ac3adm ac3::admbridge)
else()
    target_sources(ac3cli PRIVATE adm/disabled/atmos_adm.cpp)
endif()

target_link_libraries(ac3cli
    PRIVATE
        ac3::forge
        ac3::audio
        ac3::signing
        matroska::matroska
        mp4::mp4
        mpegts::mpegts
        ac3::warnings)

# ac3::forge, matroska::matroska, mp4::mp4 and mpegts::mpegts are link-only here, but
# installed and packaged in their own right elsewhere - see
# cmake/InstallLibrary.cmake and cmake/Packaging.cmake's `library` CPack
# component, and docs/library/index.md for how a third party consumes them
# via find_package(ac3forge). ac3::audio stays link-only and unpackaged
# always - a CLI/GUI implementation detail, not part of the distributed
# library.
#
# mp4::mp4/mpegts::mpegts are only ever unavailable when AC3FORGE_BUILD_MP4=OFF/
# AC3FORGE_BUILD_MPEGTS=OFF, which the root CMakeLists.txt's own FATAL_ERROR guards
# already refuse to combine with AC3FORGE_BUILD_CLI=ON - so unlike ac3::audio's
# Needs::kCapture/kPassthrough/kMonitor runtime-availability checks (main.cpp), the
# `mp4`/`ts` commands below need no such check: this target is simply never configured
# at all in a build where either would be missing.
include(GNUInstallDirs)

# COMPONENT runtime: without one, CPack files this under its own
# "Unspecified" component, inconsistent once component-based packaging is
# on for the library (see cmake/InstallLibrary.cmake, cmake/Packaging.cmake).
install(TARGETS ac3cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
    BUNDLE DESTINATION "." COMPONENT runtime)
