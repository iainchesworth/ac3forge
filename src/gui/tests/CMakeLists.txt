# ---------------------------------------------------------------------------
# ac3gui_qmltests - Qt Quick Test coverage for the interactive surfaces in
# ../qml/. Drives the REAL EncoderController, the same singleton ac3gui
# itself runs - not a mock: every leaf QML component already reads it
# directly rather than through passed-in properties, and this project's own
# code deliberately avoids a second, parallel API that the real one could
# silently disagree with (see encoder_controller.hpp's own header comment).
#
# Included (not add_subdirectory()'d) from ../CMakeLists.txt, so every path
# below is written relative to src/gui, not src/gui/tests - see the comment
# at that include() call for why that distinction is load-bearing here and
# not just a style choice.
#
# This embeds a second, independent copy of the Ac3Forge QML module - not a
# shared library with ac3gui - for the same WHOLE_ARCHIVE reason
# ../CMakeLists.txt gives for embedding it directly into the executable
# rather than a backing plugin. AC3_QML_FILES is the same list ac3gui's own
# qt_add_qml_module call uses, set once in ../CMakeLists.txt, so the two
# embeddings cannot list a different set of files without both being edited.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)

qt_add_executable(ac3gui_qmltests
    tests/qml_test_main.cpp
    encoder_controller.cpp
    encoder_controller.hpp
    system_theme.cpp
    system_theme.hpp)

qt_add_qml_module(ac3gui_qmltests
    URI Ac3Forge
    VERSION 1.0
    # Both targets embed the same URI; without this they'd both compute the
    # same "<binary dir>/Ac3Forge" output directory and collide - ac3gui's
    # own module lives there already.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Ac3Forge"
    QML_FILES ${AC3_QML_FILES})

# Forces this target's own POST_BUILD windeployqt step (below) to run only
# after ac3gui's has finished, not just after ac3gui's LINK step - confirmed
# necessary on a real "build everything" CI run: both windeployqt processes
# copy the same Qt DLLs into the same shared bin/ output directory, and two
# of them racing to write e.g. Qt6Core.dll at once fails with "Cannot create
# a file when that file already exists." A local build of one target at a
# time never triggers this, which is why it went unnoticed until now.
add_dependencies(ac3gui_qmltests ac3gui)

# The standard Qt Quick Test hookup: QUICK_TEST_MAIN's generated main() reads
# this macro to find the tst_*.qml files at run time. -input on the ctest
# COMMAND line below is a second, redundant way to say the same thing -
# belt and suspenders, since this is the one part of the setup this project
# had no prior example of to copy from.
target_compile_definitions(ac3gui_qmltests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")

# Same rationale as ../CMakeLists.txt's identical loop: Qt's generated
# moc/rcc/qmlcachegen output for THIS target compiles under the strict
# warning set unless excluded, and qmlcachegen is known to emit at least one
# warning clang-cl/GCC's -Wconversion rejects (see ../CMakeLists.txt).
get_target_property(AC3_GUI_TEST_SOURCES ac3gui_qmltests SOURCES)
set(AC3_GUI_TEST_GENERATED_SOURCES "")
foreach(source IN LISTS AC3_GUI_TEST_SOURCES)
    if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
        list(APPEND AC3_GUI_TEST_GENERATED_SOURCES "${source}")
    endif()
endforeach()
if(AC3_GUI_TEST_GENERATED_SOURCES)
    set_source_files_properties(${AC3_GUI_TEST_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
endif()

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
    COMMAND ac3gui_qmltests -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/qml" -platform offscreen)
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
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
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
