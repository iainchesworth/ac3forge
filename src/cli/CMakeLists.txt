# ---------------------------------------------------------------------------
# ac3cli - the command-line front end over ac3::forge.
# ---------------------------------------------------------------------------
add_executable(ac3cli main.cpp)

target_link_libraries(ac3cli
    PRIVATE
        ac3::forge
        ac3::audio
        matroska::matroska
        ac3::warnings)

# Optional, non-clean-room: the local-only RE-derived EMDF Atmos signer. Off by
# default; src/quarantine is gitignored and never on a public branch. main.cpp
# calls quarantine_hook.hpp unconditionally; exactly one of the two
# implementations below is ever compiled, so main.cpp needs no #ifdef - see
# quarantine_hook.hpp.
if(AC3FORGE_QUARANTINE_SIGNER)
    target_sources(ac3cli PRIVATE quarantine_hook_enabled.cpp)
    target_link_libraries(ac3cli PRIVATE ac3::quarantine)
else()
    target_sources(ac3cli PRIVATE quarantine_hook_stub.cpp)
endif()

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
