# ---------------------------------------------------------------------------
# ac3gui - the Qt6 Quick front end over ac3::forge. Presentation and platform
# glue only: every codec decision lives in the library.
#
# Qt is a PREBUILT dependency discovered by cmake/FindQt6.cmake (which widens
# CMAKE_PREFIX_PATH to the standard install roots on Windows, Linux and
# macOS), never a vcpkg port. Machines with no Qt kit build the rest of the
# project with -DAC3FORGE_BUILD_GUI=OFF.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(ac3gui
    main.cpp
    encoder_controller.cpp
    encoder_controller.hpp
    system_theme.cpp
    system_theme.hpp)

# The QML lives in qml/ on disk but belongs at the root of the QML module, so
# every file is aliased to its bare name. Left alone, Qt lays the module out as
# <module>/qml/Main.qml, which makes qml/ a second QML root directory - one
# with no qmldir of its own, so the engine resolving a sibling type from inside
# it knows nothing of the module's registrations. Theme, below, is exactly the
# kind of registration that matters. That is what Qt policy QTP0004 warns
# about; aliasing removes the second root instead of papering over it with a
# generated qmldir, and keeps working whichever way the policy defaults later.
# Absolute, not "qml/Main.qml": qt_add_qml_module resolves QML_FILES against
# whichever CMakeLists.txt calls it, and tests/CMakeLists.txt below reuses
# this same list to embed an identical module into ac3gui_qmltests - a
# relative path would resolve against the wrong directory from there.
set(AC3_QML_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/Theme.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/Card.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/ChannelMeter.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/SoundfieldView.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/SegmentedControl.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/LoudnessGroup.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/VbrPanel.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/GuidedWizard.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/RailBlock.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/AssignmentPanel.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/FirstRunScreen.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/PreferencesDialog.qml")

foreach(qml_file IN LISTS AC3_QML_FILES)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()

# The QML module is backed by the executable itself, so qt_add_qml_module
# embeds the QML and the generated type registrations directly into it - no
# separate plugin, and none of the WHOLE_ARCHIVE gymnastics a standalone
# backing library would need to keep its resource initialisers alive.
# tests/CMakeLists.txt's ac3gui_qmltests target follows the same pattern - a
# second, independent embedding of the identical module, not a shared
# library - for exactly this reason.
# Theme is a QML singleton. This MUST run before qt_add_qml_module below:
# that call reads QT_QML_SINGLETON_TYPE off each QML_FILES entry to decide
# whether to emit a `singleton` line in the generated qmldir. Set it
# afterwards and Theme registers as a plain type instead, so every Theme.*
# binding silently resolves to undefined at runtime.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qml/Theme.qml"
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(ac3gui
    URI Ac3Forge
    VERSION 1.0
    QML_FILES ${AC3_QML_FILES})

# The handoff's typeface, bundled so the design renders as designed on a
# machine that never installed it. Archivo is SIL OFL 1.1 (fonts/OFL.txt
# ships beside the faces - keep them together; the license requires it).
# Static weights rather than the variable font: Qt's font database resolves
# named static weights on every platform this builds for, while variable-
# font axis selection is only dependable from Qt 6.7 up. main.cpp registers
# these with QFontDatabase before the engine loads.
qt_add_resources(ac3gui "ac3gui_fonts"
    PREFIX "/fonts"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Archivo-Regular.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Archivo-Medium.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Archivo-SemiBold.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Archivo-ExtraBold.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/fonts/OFL.txt")

# Qt generates a lot of C++ into this target - moc output, the QML type
# registrations, the rcc resources and qmlcachegen's AOT-compiled QML - and it
# all compiles as if it were ours. It is not, and it does not survive the
# strict set: qmlcachegen emits a bare qsizetype-to-int narrowing for
# SoundfieldView's `property int lfeIndex: ...indexOf("LFE")`, which
# -Wconversion rejects under clang-cl and GCC (MSVC's /W4 has no equivalent,
# which is why this only shows up off the default toolchain). Qt's own headers
# are already outside the warning set because Qt's package config marks their
# include directories SYSTEM; generated sources have no include directory to
# mark, so they are excluded here instead. Nothing in this directory is
# authored into the binary dir, so "generated by Qt" is exactly "under
# CMAKE_CURRENT_BINARY_DIR", and matching on that is enough; our own main.cpp
# and encoder_controller.cpp keep the full set.
#
# MSVC gets a narrower carve-out (see cmake/CompilerWarnings.cmake) rather
# than the blanket "-w" clang-cl/GCC get: cl has almost nothing to say about
# this code under /W4 - the one exception found so far is C4702 (unreachable
# code) in Qt's own <QtQml/qjsprimitivevalue.h>, from the interpreter
# dispatch qmlcachegen falls back to for GuidedWizard.qml's `var`-typed,
# closure-driven step list (a JS array of objects, not a fixed set of typed
# properties - see qmlcache/*/GuidedWizard_qml.cpp.aotstats after a build for
# exactly which of its bindings took that path). If a later Qt does emit
# something else cl dislikes, that surfaces as a build failure to look at
# rather than as silence, the same as it always has.
#
# This deliberately misses one generated file: AUTOMOC adds
# ac3gui_autogen/mocs_compilation.cpp at generate time, which is after this
# directory's CMakeLists.txt - and therefore this loop - has already run, so
# it never appears in SOURCES here and keeps the full set. That is the outcome
# we want - moc's own output is clean under these flags, and the file pulls in
# our headers, so checking it costs nothing and covers something.
#
# (An earlier version of this file had a second loop here, filtering the same
# SOURCES list by the GENERATED source-file property instead of by path and
# setting COMPILE_OPTIONS to a hardcoded "-w" behind a non-MSVC generator
# expression. It selected the same files this one does - every Qt-generated
# source is both GENERATED and under CMAKE_CURRENT_BINARY_DIR - and ran first,
# so set_source_files_properties() below silently overwrote it every time:
# dead code with no effect on any build. Removed rather than kept "for
# clarity", since it was actively misleading about there being two cases.)
#
# This must run after qt_add_qml_module, which is what adds the sources.
# ---------------------------------------------------------------------------
get_target_property(AC3_GUI_SOURCES ac3gui SOURCES)
set(AC3_GUI_GENERATED_SOURCES "")
foreach(source IN LISTS AC3_GUI_SOURCES)
    if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
        list(APPEND AC3_GUI_GENERATED_SOURCES "${source}")
    endif()
endforeach()

if(AC3_GUI_GENERATED_SOURCES)
    set_source_files_properties(${AC3_GUI_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
else()
    message(WARNING
        "No Qt-generated sources found on ac3gui. If Qt has changed where it "
        "writes them, they are now being compiled with ac3::warnings and a "
        "warning in Qt's own output will fail the build.")
endif()

target_link_libraries(ac3gui
    PRIVATE
        ac3::forge
        # EncoderController's live-capture/monitor/passthrough panel is a
        # real, load-bearing consumer of this - not incidental.
        ac3::audio
        # The GUI can save straight to .mkv, so it needs the muxer the CLI's
        # 'mkv' command uses. It links nothing from ac3::forge itself.
        matroska::matroska
        ac3::warnings
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent)

# A GUI app rather than a console one. MACOSX_BUNDLE moves the binary to
# bin/ac3gui.app/Contents/MacOS/ac3gui, which is where --smoke lives on macOS;
# both properties are ignored on the platform they do not apply to.
set_target_properties(ac3gui PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

# ---------------------------------------------------------------------------
# Deploy Qt's runtime DLLs and QML plugins next to the built exe so it runs
# straight from the build tree. Without this the loader cannot find Qt6Core /
# Qt6Quick and the QtQuick.Controls imports fail to resolve at startup.
#
# Windows only, and not an oversight: ELF and Mach-O builds record the Qt kit's
# library directory in the binary's RPATH, and Qt then locates its own plugins
# and QML modules relative to the install prefix it was built with. So the
# build tree already runs - including under QT_QPA_PLATFORM=offscreen, because
# the kit's own offscreen plugin is found in place rather than needing to be
# copied. Shipping the app elsewhere would still want macdeployqt / linuxdeploy;
# that is a packaging step, not a build one.
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(AC3_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(AC3_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt6 windeployqt
        HINTS "${AC3_QT_BIN_DIR}")

    if(AC3_WINDEPLOYQT_EXECUTABLE)
        # --plugindir platforms is implicit, but windeployqt only ships the
        # platform plugin the app will actually use (qwindows). The offscreen
        # plugin is added deliberately so the app can be smoke-tested headless
        # in CI and from a script - without it, QT_QPA_PLATFORM=offscreen
        # aborts before any QML is parsed and a "did it start?" check silently
        # proves nothing.
        #
        # --compiler-runtime locates the MSVC redistributable through
        # VCINSTALLDIR, which only exists in a Developer PowerShell. The
        # toolchain files set it up for the configure process, but this command
        # runs under ninja in whatever shell drives the build, so pass it
        # through explicitly - otherwise windeployqt quietly skips the runtime
        # ("Cannot find Visual Studio installation directory") and the deployed
        # tree is incomplete on a machine without VS.
        #
        # Strip the trailing separator MSVC leaves on the value first: the raw
        # "...\Community\VC\" ends the quoted argument with a backslash, which
        # cmd reads as an escaped quote and swallows the rest of the command
        # line ("cmake -E env: no command given").
        set(AC3_VCINSTALLDIR "$ENV{VCINSTALLDIR}")
        string(REGEX REPLACE "[\\\\/]+$" "" AC3_VCINSTALLDIR "${AC3_VCINSTALLDIR}")
        file(TO_CMAKE_PATH "${AC3_VCINSTALLDIR}" AC3_VCINSTALLDIR)

        # windeployqt prints "Warning: Cannot find any version of the
        # dxcompiler.dll and dxil.dll." on every run. This is not a missing
        # dependency of ours: windeployqt bundles every qmltooling plugin the
        # Qt kit ships (they can be loaded for QML debugging/profiling at
        # runtime), and one of them - qmldbg_quick3dprofiler.dll, the Quick3D
        # profiler backend - pulls in Qt6Quick3DUtils, which optionally links
        # the DirectX Shader Compiler. ac3gui has no QtQuick3D import anywhere
        # (grep the qml/ tree) and never loads that profiler plugin, so the
        # two DLLs windeployqt cannot find are for a code path this app never
        # takes. Safe to ignore.
        add_custom_command(TARGET ac3gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env "VCINSTALLDIR=${AC3_VCINSTALLDIR}"
                "${AC3_WINDEPLOYQT_EXECUTABLE}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --compiler-runtime
                "$<TARGET_FILE:ac3gui>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AC3_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:ac3gui>/platforms/"
            COMMENT "Deploying Qt runtime dependencies for ac3gui")
    else()
        message(WARNING
            "windeployqt not found next to Qt6 (looked in ${AC3_QT_BIN_DIR}); "
            "ac3gui.exe will not run standalone from the build tree.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Install + Qt runtime deployment for packaging. qt_generate_deploy_qml_app_script()
# is Qt's CMake-native replacement for windeployqt/macdeployqt at install
# time: it inspects the built target - including scanning its QML files for
# imports, since this is a QQmlApplicationEngine app - and emits an
# install(SCRIPT) that copies the Qt libraries/plugins/QML modules the app
# actually uses next to it (or into the .app bundle on macOS). This is
# separate from the POST_BUILD windeployqt step above, which targets the
# build tree, not an install/package tree.
#
# The plain qt_generate_deploy_app_script() (no "_qml_") looks like the more
# obvious choice and is NOT what this calls: it only runs
# qt6_deploy_runtime_dependencies() (library dependencies), never
# qt6_deploy_qml_imports() - confirmed empirically, a packaged build with it
# produced a completely empty qml/ directory and ac3gui.exe failed outside
# the build tree with "the QML failed to load" (no Fusion style plugin, no
# QtQuick.Controls modules - nothing qmlimportscanner would have caught).
# qt_generate_deploy_qml_app_script() is the QML-aware sibling that actually
# scans main.cpp's engine.loadFromModule() and the .qml files, matching what
# the POST_BUILD windeployqt --qmldir call above already does for the build
# tree - including bundling every built-in QtQuick.Controls style (Fusion
# among them), because QQuickStyle::setStyle("Fusion") is a runtime C++ call
# no static QML scan can see is the one actually used.
#
# Linux gets none of this: qt_generate_deploy_qml_app_script() still writes a
# "Prefix = .." qt.conf next to the installed binary even with
# NO_UNSUPPORTED_PLATFORM_ERROR, which misdirects the SYSTEM Qt6's QML import
# path lookup and breaks QtQuick.Controls at startup - confirmed empirically
# in CountdownSolver (R:\CountdownSolver\src\app\CMakeLists.txt), which hit
# this exact bug and documents it in the same place. Linux packaging instead
# relies on a declared Qt6 runtime dependency in cmake/Packaging.cmake's DEB
# generator plus the system's own Qt6 install - not implemented yet here
# because AC3FORGE_BUILD_GUI defaults OFF on every Linux preset (see
# CMakePresets.json), so this bug cannot bite in CI until someone turns it on
# for a packaging build. Do not "fix" this by adding the WIN32 OR APPLE
# guard below back in for Linux too.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

# COMPONENT runtime - see src/cli/CMakeLists.txt's identical ac3cli install()
# for why.
install(TARGETS ac3gui
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
    BUNDLE DESTINATION "." COMPONENT runtime)

if(WIN32 OR APPLE)
    qt_generate_deploy_qml_app_script(
        TARGET ac3gui
        OUTPUT_SCRIPT AC3_GUI_DEPLOY_SCRIPT
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${AC3_GUI_DEPLOY_SCRIPT}")
endif()

# Qt Quick Test coverage for the interactive surfaces above - gated the same
# way tests/ is at the top level, and kept beside the GUI code it exercises
# rather than inside the Qt-free tests/ Catch2 tree.
#
# include(), not add_subdirectory(): tests/CMakeLists.txt's qt_add_executable
# and qt_add_qml_module calls for ac3gui_qmltests need to run in THIS
# directory's own scope, not a nested one - add_subdirectory() creates one,
# and a fresh CI build (never an incrementally-rebuilt local tree, which can
# mask this on stale cached state) then fails: Qt's AUTOMOC "collect
# metatypes JSON" custom command silently does not get generated for a
# target whose executable and QML module registration straddle a directory
# boundary, leaving qmltyperegistrar fed an empty file with no visible error
# unless QT_FORCE_STDERR_LOGGING=1 is also set - confirmed by reproducing
# the CI failure locally with the Release preset. include() keeps
# tests/CMakeLists.txt's own CMAKE_CURRENT_SOURCE_DIR/BINARY_DIR equal to
# this file's, so every path inside it is written relative to src/gui, not
# src/gui/tests.
if(AC3FORGE_BUILD_TESTS)
    include(tests/CMakeLists.txt)
endif()
