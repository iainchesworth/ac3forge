# ---------------------------------------------------------------------------
# ac3cli - the command-line front end over ac3::forge.
# ---------------------------------------------------------------------------
add_executable(ac3cli main.cpp)

# ac3cli's one piece of platform-specific code: putting stdin/stdout into
# binary mode for the "-" stdin/stdout file-argument convention (see
# platform/stdio_binary.hpp for why Windows needs a real call and POSIX
# doesn't). Selected here rather than behind an #ifdef in main.cpp, the same
# platform-tree pattern ac3::audio uses for WASAPI/ALSA (src/audio/CMakeLists.txt)
# - scripts/check-platform-macros.ps1 enforces there being no third option.
if(WIN32)
    target_sources(ac3cli PRIVATE platform/windows/stdio_binary.cpp)
else()
    target_sources(ac3cli PRIVATE platform/posix/stdio_binary.cpp)
endif()

target_link_libraries(ac3cli
    PRIVATE
        ac3::forge
        ac3::audio
        ac3::signing
        matroska::matroska
        ac3::warnings)

# ac3::forge and matroska::matroska are link-only here, but installed and
# packaged in their own right elsewhere - see cmake/InstallLibrary.cmake and
# cmake/Packaging.cmake's `library` CPack component, and
# docs/library/index.md for how a third party consumes them via
# find_package(ac3forge). ac3::audio stays link-only and unpackaged always -
# a CLI/GUI implementation detail, not part of the distributed library.
include(GNUInstallDirs)

# COMPONENT runtime: without one, CPack files this under its own
# "Unspecified" component, inconsistent once component-based packaging is
# on for the library (see cmake/InstallLibrary.cmake, cmake/Packaging.cmake).
install(TARGETS ac3cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
    BUNDLE DESTINATION "." COMPONENT runtime)
