# ---------------------------------------------------------------------------
# ac3cli - the command-line front end over ac3::forge.
# ---------------------------------------------------------------------------
add_executable(ac3cli main.cpp)

target_link_libraries(ac3cli
    PRIVATE
        ac3::forge
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

# ac3::forge and matroska::matroska are link-only: this project is not
# currently positioned as a library other projects consume, so only the
# executables get installed/packaged. Revisit with an EXPORT set if that
# ever changes.
include(GNUInstallDirs)

install(TARGETS ac3cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE DESTINATION ".")
