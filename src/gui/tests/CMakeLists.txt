# ---------------------------------------------------------------------------
# ac3gui_qmltests - Qt Quick Test coverage for the interactive surfaces in
# ../qml/. Drives the REAL EncoderController, the same singleton ac3gui
# itself runs - not a mock: every leaf QML component already reads it
# directly rather than through passed-in properties, and this project's own
# code deliberately avoids a second, parallel API that the real one could
# silently disagree with (see encoder_controller.hpp's own header comment).
#
# This embeds a second, independent copy of the Ac3Forge QML module - not a
# shared library with ac3gui - for the same WHOLE_ARCHIVE reason
# ../CMakeLists.txt gives for embedding it directly into the executable
# rather than a backing plugin.
#
# The qt_add_qml_module() call for THIS target is not in this file - it is
# back in ../CMakeLists.txt, right after this add_subdirectory() returns.
# Qt's generated qmlcache build rules compute their OWN output path from the
# QML file's location relative to whichever CMakeLists.txt calls
# qt_add_qml_module, regardless of QT_RESOURCE_ALIAS (confirmed empirically -
# calling it from here, referencing ../qml/*.qml, produced ninja rules for a
# literal "ac3gui_qmltests_..\qml\" output directory and failed at build
# time). Calling it from ../CMakeLists.txt, where qml/ is a direct child,
# avoids the whole problem rather than working around it.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)

qt_add_executable(ac3gui_qmltests
    qml_test_main.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../encoder_controller.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../encoder_controller.hpp")

# The standard Qt Quick Test hookup: QUICK_TEST_MAIN's generated main() reads
# this macro to find the tst_*.qml files at run time. -input on the ctest
# COMMAND line below is a second, redundant way to say the same thing -
# belt and suspenders, since this is the one part of the setup this project
# had no prior example of to copy from.
target_compile_definitions(ac3gui_qmltests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")

target_include_directories(ac3gui_qmltests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_link_libraries(ac3gui_qmltests
    PRIVATE
        ac3::forge
        ac3::audio
        matroska::matroska
        ac3::warnings
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        Qt6::Concurrent)

# QT_QPA_PLATFORM=offscreen on the test itself, not just -platform offscreen
# on the command line: belt and suspenders again, and it means running the
# binary by hand with no arguments (as a developer debugging a failure would)
# still behaves headlessly instead of trying to open a real window.
#
# QT_FORCE_STDERR_LOGGING=1 is not optional, confirmed the hard way: Qt's
# default Windows behaviour is to decide whether a "real" console is attached
# and, if it decides no, silently reroute qWarning/qCritical/QtTest's own
# PASS/FAIL/Totals output to OutputDebugString instead of stderr - the exit
# code still correctly reflects pass/fail, but every diagnostic that would
# say WHY a failure happened is invisible to ctest's captured log, a build
# log, or a developer's terminal. Without this, a red CI run shows nothing
# but "failed" and someone has to already know this flag exists to find out
# what broke.
add_test(NAME ac3gui_qml_tests
    COMMAND ac3gui_qmltests -input "${CMAKE_CURRENT_SOURCE_DIR}/qml" -platform offscreen)
set_tests_properties(ac3gui_qml_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
    LABELS "gui")

# ---------------------------------------------------------------------------
# Windows-only Qt runtime deployment, identical in spirit to ../CMakeLists.txt's
# POST_BUILD windeployqt step and for the same reason: without it the loader
# cannot find Qt6Core/Qt6Quick and QtQuick.Controls imports fail to resolve,
# offscreen or not. Linux/macOS need nothing here - the Qt kit's own RPATH
# already covers it, exactly as it does for ac3gui and for --smoke today.
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(AC3_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(AC3_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt6 windeployqt
        HINTS "${AC3_QT_BIN_DIR}")

    if(AC3_WINDEPLOYQT_EXECUTABLE)
        set(AC3_VCINSTALLDIR "$ENV{VCINSTALLDIR}")
        string(REGEX REPLACE "[\\\\/]+$" "" AC3_VCINSTALLDIR "${AC3_VCINSTALLDIR}")
        file(TO_CMAKE_PATH "${AC3_VCINSTALLDIR}" AC3_VCINSTALLDIR)

        add_custom_command(TARGET ac3gui_qmltests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env "VCINSTALLDIR=${AC3_VCINSTALLDIR}"
                "${AC3_WINDEPLOYQT_EXECUTABLE}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/../qml"
                --compiler-runtime
                "$<TARGET_FILE:ac3gui_qmltests>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AC3_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:ac3gui_qmltests>/platforms/"
            COMMENT "Deploying Qt runtime dependencies for ac3gui_qmltests")
    else()
        message(WARNING
            "windeployqt not found next to Qt6 (looked in ${AC3_QT_BIN_DIR}); "
            "ac3gui_qmltests.exe will not run standalone from the build tree.")
    endif()
endif()
