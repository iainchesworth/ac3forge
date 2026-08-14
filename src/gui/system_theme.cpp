#include "system_theme.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

SystemTheme::SystemTheme(QObject* parent) : QObject(parent) {
    if (qGuiApp != nullptr) {
        // ApplicationPaletteChange is how a live accent edit in the OS
        // Settings app arrives; a colour-scheme flip can carry a different
        // accent with it, so both re-announce.
        qGuiApp->installEventFilter(this);
        connect(qGuiApp->styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this] { emit accentChanged(); });
    }
}

QColor SystemTheme::accentColor() const {
    if (qGuiApp == nullptr) {
        // Only reachable before a QGuiApplication exists (tooling contexts);
        // ink's own blue keeps the derived ramp sane rather than black.
        return {0x2f, 0x54, 0xd0};
    }
    return qGuiApp->palette().accent().color();
}

bool SystemTheme::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qGuiApp && event->type() == QEvent::ApplicationPaletteChange) {
        emit accentChanged();
    }
    return QObject::eventFilter(watched, event);
}
