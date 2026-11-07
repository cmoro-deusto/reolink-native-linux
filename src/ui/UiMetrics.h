#pragma once

#include <QEvent>
#include <QFontInfo>
#include <QGuiApplication>
#include <QObject>

namespace rl {

// The system UI font's size in logical pixels, for QML to size the interface
// from (see Theme.uiScale). Resolved through QFontInfo because the font is
// usually set in points, which QML's Qt.application.font leaves as pixelSize
// -1. Display scaling (e.g. 145%) is separate: Qt applies it on top of
// logical pixels, so this tracks only the user's font-size choice. Follows
// the desktop's font when it changes while the app runs.
class UiMetrics : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal fontPixelSize READ fontPixelSize NOTIFY fontPixelSizeChanged)

public:
    explicit UiMetrics(QObject *parent = nullptr) : QObject(parent)
    {
        qGuiApp->installEventFilter(this);
    }

    qreal fontPixelSize() const { return QFontInfo(QGuiApplication::font()).pixelSize(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == qGuiApp && event->type() == QEvent::ApplicationFontChange)
            emit fontPixelSizeChanged();
        return QObject::eventFilter(watched, event);
    }

signals:
    void fontPixelSizeChanged();
};

} // namespace rl
