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
# default; src/quarantine is gitignored and never on a public branch.
if(AC3FORGE_QUARANTINE_SIGNER)
    target_link_libraries(ac3cli PRIVATE ac3::quarantine)
    target_compile_definitions(ac3cli PRIVATE AC3FORGE_QUARANTINE_SIGNER)
endif()
