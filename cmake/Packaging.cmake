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

# Per-generator override, sourced by cpack itself once per generator in a
# multi-generator run - see cmake/CPackProjectConfig.cmake for why DragNDrop
# needs one (CPACK_COMPONENTS_GROUPING below is global CPack state, and
# DragNDrop reads the same value the archive generators use to split).
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CPackProjectConfig.cmake")

if(WIN32)
    find_program(AC3FORGE_MAKENSIS_EXECUTABLE makensis)
    if(AC3FORGE_MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        # Same source ac3gui's own .rc uses (src/gui/CMakeLists.txt) - the
        # installer/uninstaller windows and shortcut both otherwise default
        # to NSIS's own generic icon. NSIS wants a Windows .ico specifically
        # for both variables, which generate_icons.py already produces.
        set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/src/gui/icons/ac3forge.ico")
        set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/src/gui/icons/ac3forge.ico")
    endif()
elseif(APPLE)
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

        # Component-aware packaging, OFF by default for the DEB generator -
        # without this, CPack ignores CPACK_COMPONENTS_ALL/GROUP entirely and
        # bundles every install()'d file (ac3cli AND the full library SDK)
        # into one monolithic .deb, confirmed empirically against a real
        # `dpkg-deb -c` of this project's own pre-split output. Turning it on
        # is what makes runtime/library/libruntime become three independent
        # .deb files instead. CPACK_COMPONENTS_GROUPING's file-level default
        # (below) merges library+libruntime into one "dev" archive for ZIP/
        # TGZ - cmake/CPackProjectConfig.cmake overrides that back to IGNORE
        # for exactly the DEB/RPM passes, so those two stay three separate
        # packages instead of collapsing to the archives' two.
        set(CPACK_DEB_COMPONENT_INSTALL ON)

        # Package-name overrides: without these, CPack derives
        # <name>-<component> for every component once component install is
        # on (e.g. "ac3forge-runtime"), which both renames today's existing
        # ac3cli package and ignores Debian's own libFOO/libFOO-dev naming
        # convention for the library halves.
        set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "ac3forge")
        set(CPACK_DEBIAN_LIBRUNTIME_PACKAGE_NAME "libac3forge0")
        set(CPACK_DEBIAN_LIBRARY_PACKAGE_NAME "libac3forge-dev")

        # The -dev package's headers/static-archives are useless without a
        # matching runtime .so to actually link and load - and since this
        # project makes no ABI-compatibility promise pre-1.0 (see
        # src/forge/CMakeLists.txt's SOVERSION comment), the pin has to be
        # exact, not a >= floor. libac3forge0 itself declares no such
        # dependency the other way: it is a plain .so with no headers or
        # symlink of its own, valid to have installed alone.
        # PROJECT_VERSION, not CPACK_PACKAGE_VERSION: the latter is only
        # computed by include(CPack) itself, further down this file - read
        # here, before that point, it is still unset and silently renders
        # this Depends line as "libac3forge0 (= )" with no version at all
        # (confirmed empirically against a real dpkg-deb -I). See
        # CPACK_SYSTEM_NAME's identical trap, documented below.
        set(CPACK_DEBIAN_LIBRARY_PACKAGE_DEPENDS "libac3forge0 (= ${PROJECT_VERSION})")
    endif()

    find_program(AC3FORGE_RPMBUILD_EXECUTABLE rpmbuild)
    if(AC3FORGE_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
        set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
        set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)

        # Same reasoning and the same three-way split as the DEB block above,
        # RPM's own equivalent switch and per-component variable names.
        # "-devel" rather than "-dev": Fedora/RHEL/openSUSE package-naming
        # convention for a development package, where Debian/Ubuntu use "-dev".
        set(CPACK_RPM_COMPONENT_INSTALL ON)
        set(CPACK_RPM_RUNTIME_PACKAGE_NAME "ac3forge")
        set(CPACK_RPM_LIBRUNTIME_PACKAGE_NAME "libac3forge0")
        set(CPACK_RPM_LIBRARY_PACKAGE_NAME "ac3forge-devel")
        set(CPACK_RPM_LIBRARY_PACKAGE_REQUIRES "libac3forge0 = %{version}-%{release}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Library component(s): a second, separate download alongside the existing
# ac3cli/ac3gui package - headers + .lib/.dll/.a/.so + CMake package config
# for a third party consuming ac3::forge/matroska::matroska via
# find_package(ac3forge) (see cmake/InstallLibrary.cmake). Everything
# install()'d without an explicit COMPONENT falls into CPack's own
# "Unspecified" component, which is why ac3cli/ac3gui and every
# InstallLibrary.cmake rule now carry one explicitly.
#
# Three components, not two: "runtime" (ac3cli/ac3gui, unchanged), "library"
# (headers, static archives, CMake package config, and - on Unix - the
# unversioned .so namelink symlink you link against), and "libruntime" (just
# the versioned .so/.dylib a linked binary loads at runtime - see
# cmake/InstallLibrary.cmake's NAMELINK_COMPONENT comment for why that file
# alone is split out). library+libruntime are DELIBERATELY kept as one
# archive download below (a "-dev" ZIP/TGZ downloader wants both without
# knowing this split exists) but as three separate DEB/RPM packages
# (cmake/CPackProjectConfig.cmake overrides the grouping back to IGNORE for
# just those two generators) - that split is the entire point of shipping
# them as .deb/.rpm at all: apt/dnf can then pull in "the .so a linked binary
# needs" via libac3forge0 without the headers/static archives libac3forge-dev
# carries, the same libFOO/libFOO-dev shape every other Linux C library uses.
#
# CPACK_ARCHIVE_COMPONENT_INSTALL is specifically the Archive generator
# family's (ZIP/TGZ) own component-install switch - it does not affect
# NSIS/DragNDrop, each of which has its own separate
# CPACK_<GENERATOR>_COMPONENT_INSTALL flag, left off here deliberately:
#   - NSIS: a component installer can't also produce a second standalone
#     download the way a second archive naturally can - splitting it would
#     need an entirely different NSIS packaging shape, not a flag flip.
#   - DragNDrop: no macOS host to build or verify this against at all (see
#     the DragNDrop branch above); cmake/CPackProjectConfig.cmake already
#     forces it monolithic regardless of the component/grouping state here.
# DEB/RPM get their own *_COMPONENT_INSTALL switch, set inside their own
# find_program() blocks above, now that the split is real work rather than
# a placeholder.
set(CPACK_COMPONENTS_ALL runtime library libruntime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)

# Default grouping (CPack's own ONE_PER_GROUP): one archive/package per
# CPACK_COMPONENT_<C>_GROUP, one per otherwise-ungrouped component. "runtime"
# stays ungrouped (its own archive, as always); "library"+"libruntime" share
# GROUP "dev" so the archive generators still merge them into the single
# "-dev" download documented in docs/releasing.md - the DEB/RPM split above
# is a per-generator override of this default, not a replacement for it.
set(CPACK_COMPONENT_LIBRARY_GROUP "dev")
set(CPACK_COMPONENT_LIBRUNTIME_GROUP "dev")

# Per-component/per-group filename overrides so the existing ac3cli/ac3gui
# and library archives' names don't change now that they are formally
# "the runtime component"/"the dev group" rather than "everything". Without
# an override, an archive's default name appends the component or group's own
# name (e.g. -runtime/-dev) - the runtime override below exists purely to
# suppress that suffix and keep today's exact filename; the dev-group
# override chooses the name explicitly rather than accepting CPack's default
# "-dev" suffix, matching the ac3forge-dev-* convention docs/releasing.md
# documents. CPACK_ARCHIVE_<NAME>_FILE_NAME keys off the GROUP name once one
# is assigned (library+libruntime share GROUP "dev" above), not the
# individual component name - CPACK_ARCHIVE_LIBRARY_FILE_NAME /
# CPACK_ARCHIVE_LIBRUNTIME_FILE_NAME would silently do nothing now.
#
# CPACK_SYSTEM_NAME and CPACK_PACKAGE_FILE_NAME are NOT usable here despite
# looking already computed above - both are actually filled in by the
# include(CPack) module itself, further down, not by any of the set() calls
# in this file: confirmed by an empty CPACK_SYSTEM_NAME producing a real
# "ac3forge-dev-0.2.0-beta.1-.zip" (trailing hyphen, no platform) and the
# runtime override silently no-op'ing back to CPack's own "-runtime"
# suffixed default, from an actual cpack --preset pack-windows-msvc run, not
# assumed. Setting both explicitly here, before include(CPack), replicates
# CPack's own default computation (win32/win64 on Windows, the bare
# CMAKE_SYSTEM_NAME elsewhere; NAME-VERSION-SYSTEM for the base filename) so
# today's existing filename is unchanged, and include(CPack) leaves an
# already-set variable alone rather than recomputing it.
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CPACK_SYSTEM_NAME "win64")
    else()
        set(CPACK_SYSTEM_NAME "win32")
    endif()
elseif(LINUX)
    # Arch-qualified so an x64 and an arm64 TGZ/ZIP built for the same release
    # don't produce an identical filename - CMAKE_SYSTEM_PROCESSOR (x86_64 /
    # aarch64) is already set correctly by cmake/toolchains/linux.*.toolchain.cmake.
    # DEB/RPM don't need this: their own filenames already carry the arch.
    # APPLE stays plain "Darwin" below - only one macOS arch (arm64) exists as
    # a triplet today, so there is no collision to avoid there yet.
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}")
endif()
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}-${CPACK_SYSTEM_NAME}")

set(CPACK_ARCHIVE_RUNTIME_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
set(CPACK_ARCHIVE_DEV_FILE_NAME "ac3forge-dev-${PROJECT_VERSION_FULL}-${CPACK_SYSTEM_NAME}")

include(CPack)

# Lets `cpack` be triggered from inside an IDE's target list (e.g. Visual
# Studio), not just the command line.
add_custom_target(pack-${PROJECT_NAME}
    COMMAND "${CMAKE_CPACK_COMMAND}" -C $<CONFIGURATION> --config "${CPACK_OUTPUT_CONFIG_FILE}"
    COMMENT "Running CPack. Please wait..."
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
set_target_properties(pack-${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD 1)
