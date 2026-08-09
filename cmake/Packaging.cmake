# ---------------------------------------------------------------------------
# CPack packaging. Included once, from the top-level CMakeLists.txt, after
# every target's install() rules have been declared.
#
# A plain ZIP archive is always offered (needs no external tool). Platform-
# native formats are layered on top when the packaging tool for that format is
# actually available, so `cpack` degrades gracefully instead of failing
# outright. Which targets end up in a package is decided entirely by which
# install() rules ran - ac3cli's runs unconditionally (AC3FORGE_BUILD_CLI
# defaults ON), ac3gui's only when AC3FORGE_BUILD_GUI is ON - so no extra
# gating is needed here for that.
#
# CMakePresets.json's packagePresets deliberately carry no "generators"
# field: `cpack --preset` passes that field to cpack as -G on the command
# line, which OVERRIDES the CPACK_GENERATOR list computed below - confirmed
# empirically, a preset naming NSIS made cpack hard-fail with "Cannot find
# NSIS compiler makensis" even with the find_program() gate below correctly
# leaving NSIS out of CPACK_GENERATOR because makensis was not on PATH.
# Omitting it lets CPack fall back to CPACK_GENERATOR from here instead, so
# the graceful degradation this file computes actually takes effect through
# `cpack --preset` and not only through a bare `cpack` invocation.
# ---------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME "ac3forge")
set(CPACK_PACKAGE_VENDOR "Iain Chesworth")
set(CPACK_PACKAGE_CONTACT "Iain Chesworth")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ac3forge")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_VERBATIM_VARIABLES ON)

# A <package>.sha512 file beside every package, so a release can publish a
# checksum without a separate sha512sum pass over the packages/ directory.
set(CPACK_PACKAGE_CHECKSUM "SHA512")

set(CPACK_GENERATOR "ZIP")

if(WIN32)
    find_program(AC3FORGE_MAKENSIS_EXECUTABLE makensis)
    if(AC3FORGE_MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    endif()
elseif(APPLE)
    # Structurally present for parity with the rest of the preset matrix, but
    # unverified: this project has no macOS host, so this branch has never
    # actually been configured, let alone run (see macos-llvm's description
    # in CMakePresets.json).
    list(APPEND CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    list(APPEND CPACK_GENERATOR "TGZ")

    find_program(AC3FORGE_DPKG_DEB_EXECUTABLE dpkg-deb)
    if(AC3FORGE_DPKG_DEB_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_VENDOR}")
        set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

        # No explicit CPACK_DEBIAN_PACKAGE_DEPENDS for Qt here, unlike
        # CountdownSolver's Packaging.cmake (which this module is otherwise
        # modelled on). That is not an oversight: AC3FORGE_BUILD_GUI defaults
        # OFF on every Linux preset today (cmake/FindQt6.cmake can find a
        # Linux Qt kit fine - see CMakePresets.json's linux-gcc description -
        # but nothing turns AC3FORGE_BUILD_GUI on by default there yet), so a
        # Linux .deb here only ever contains ac3cli, which links no Qt at
        # all. CPACK_DEBIAN_PACKAGE_SHLIBDEPS alone is enough for that.
        #
        # The gotcha to know about BEFORE packaging a Linux ac3gui build:
        # dpkg-shlibdeps will NOT pick up Qt's libraries on its own if that
        # Qt kit came from a private prebuilt archive rather than an apt
        # package - SHLIBDEPS resolves a shared library to a Depends entry
        # by asking dpkg which *installed apt package* owns that .so file,
        # and silently drops anything it can't map that way. CountdownSolver
        # hit this for real (see the comment in
        # R:\CountdownSolver\cmake\Packaging.cmake) and works around it with
        # an explicit CPACK_DEBIAN_PACKAGE_DEPENDS list naming the Qt runtime
        # + qml6-module-* packages and minimum versions by hand. Do the same
        # here once a Linux ac3gui is actually being packaged - and note
        # this only applies if that Qt kit is NOT the distro's own apt
        # package; a system Qt6 install (e.g. via apt) resolves fine through
        # SHLIBDEPS alone, same as it does for every other shared library.
    endif()

    find_program(AC3FORGE_RPMBUILD_EXECUTABLE rpmbuild)
    if(AC3FORGE_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
        set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
        set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
    endif()
endif()

include(CPack)

# Lets `cpack` be triggered from inside an IDE's target list (e.g. Visual
# Studio), not just the command line.
add_custom_target(pack-${PROJECT_NAME}
    COMMAND "${CMAKE_CPACK_COMMAND}" -C $<CONFIGURATION> --config "${CPACK_OUTPUT_CONFIG_FILE}"
    COMMENT "Running CPack. Please wait..."
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
set_target_properties(pack-${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD 1)
