# ---------------------------------------------------------------------------
# ac3cli - the command-line front end over ac3::forge.
# ---------------------------------------------------------------------------
add_executable(ac3cli main.cpp)

target_link_libraries(ac3cli
    PRIVATE
        ac3::forge
        ac3::audio
        ac3::signing
        matroska::matroska
        mp4::mp4
        ac3::warnings)

# ac3::forge, matroska::matroska and mp4::mp4 are link-only here, but
# installed and packaged in their own right elsewhere - see
# cmake/InstallLibrary.cmake and cmake/Packaging.cmake's `library` CPack
# component, and docs/library/index.md for how a third party consumes them
# via find_package(ac3forge). ac3::audio stays link-only and unpackaged
# always - a CLI/GUI implementation detail, not part of the distributed
# library.
#
# mp4::mp4 is only ever unavailable when AC3FORGE_BUILD_MP4=OFF, which the
# root CMakeLists.txt's own FATAL_ERROR guard already refuses to combine with
# AC3FORGE_BUILD_CLI=ON - so unlike ac3::audio's Needs::kCapture/kPassthrough/
# kMonitor runtime-availability checks (main.cpp), the `mp4` command below
# needs no such check: this target is simply never configured at all in a
# build where it would be missing.
include(GNUInstallDirs)

# COMPONENT runtime: without one, CPack files this under its own
# "Unspecified" component, inconsistent once component-based packaging is
# on for the library (see cmake/InstallLibrary.cmake, cmake/Packaging.cmake).
install(TARGETS ac3cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
    BUNDLE DESTINATION "." COMPONENT runtime)
