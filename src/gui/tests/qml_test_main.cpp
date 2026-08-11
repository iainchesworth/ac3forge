#include <QtQuickTest/quicktest.h>

// Standard Qt Quick Test entry point: discovers and runs every tst_*.qml
// file under QUICK_TEST_SOURCE_DIR (set in CMakeLists.txt), exercising the
// real EncoderController the qmldir already embedded into this binary
// resolves - see CMakeLists.txt for why that is a second embedding of the
// module rather than a shared library with ac3gui.
QUICK_TEST_MAIN(ac3gui)
