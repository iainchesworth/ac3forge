# ---------------------------------------------------------------------------
# ac3gui - the Qt6 Quick front end over ac3::forge. Presentation and platform
# glue only: every codec decision lives in the library.
#
# Qt is a PREBUILT dependency discovered by cmake/FindQt6.cmake (which widens
# CMAKE_PREFIX_PATH to the standard install roots), never a vcpkg port.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(ac3gui
    main.cpp
    encoder_controller.cpp
    encoder_controller.hpp)

# The QML module is backed by the executable itself, so qt_add_qml_module
# embeds the QML and the generated type registrations directly into it - no
# separate plugin, and none of the WHOLE_ARCHIVE gymnastics a standalone
# backing library would need to keep its resource initialisers alive.
qt_add_qml_module(ac3gui
    URI Ac3Forge
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Card.qml)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

target_link_libraries(ac3gui
    PRIVATE
        ac3::forge
        ac3::warnings
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent)

set_target_properties(ac3gui PROPERTIES WIN32_EXECUTABLE ON)

# ---------------------------------------------------------------------------
# Deploy Qt's runtime DLLs and QML plugins next to the built exe so it runs
# straight from the build tree. Without this the loader cannot find Qt6Core /
# Qt6Quick and the QtQuick.Controls imports fail to resolve at startup.
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(AC3_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(AC3_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt6 windeployqt
        HINTS "${AC3_QT_BIN_DIR}")

    if(AC3_WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ac3gui POST_BUILD
            COMMAND "${AC3_WINDEPLOYQT_EXECUTABLE}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --compiler-runtime
                "$<TARGET_FILE:ac3gui>"
            COMMENT "Deploying Qt runtime dependencies for ac3gui")
    else()
        message(WARNING
            "windeployqt not found next to Qt6 (looked in ${AC3_QT_BIN_DIR}); "
            "ac3gui.exe will not run standalone from the build tree.")
    endif()
endif()
