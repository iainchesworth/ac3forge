#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QObject>
#include <QSettings>
#include <QTemporaryDir>

#include <optional>

// Standard Qt Quick Test entry point: discovers and runs every tst_*.qml
// file under QUICK_TEST_SOURCE_DIR (set in CMakeLists.txt), exercising the
// real EncoderController the qmldir already embedded into this binary
// resolves - see CMakeLists.txt for why that is a second embedding of the
// module rather than a shared library with ac3gui.
//
// The setup object exists for one reason: Main.qml's QML Settings must be
// HERMETIC here. With no organization/application identifiers, Qt 6.8's
// Settings failed to initialise and every window saw in-memory defaults -
// accidental but perfect isolation. Qt 6.10's fallback store PERSISTS
// instead, across windows and across runs, so every freshly created test
// window restored the previous window's saved session on top of the one
// live controller they all share - one duplicated source per test, and
// assignments reappearing from runs that finished days earlier. Real
// identifiers plus a QTemporaryDir settings path make the store behave
// normally and evaporate with the process; session restore itself is
// seeded OFF because restoring is a fresh-process feature no test wants
// firing under a shared controller.
class SettingsIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("ac3forge-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("ac3gui_qmltests"));
        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());

        QSettings settings;
        settings.beginGroup(QStringLiteral("workbench"));
        settings.setValue(QStringLiteral("restoreSession"), false);
    }

private:
    std::optional<QTemporaryDir> scratch_;
};

QUICK_TEST_MAIN_WITH_SETUP(ac3gui, SettingsIsolation)

#include "qml_test_main.moc"
