#include "caretrest.h"

#include <QEvent>
#include <QGuiApplication>
#include <QStyleHints>

CaretRest::CaretRest(QObject *parent) : QObject(parent)
{
    m_rest.setSingleShot(true);
    m_rest.setInterval(RestMs);
    connect(&m_blink, &QTimer::timeout, this, [this] {
        m_lit = !m_lit;
        refresh();
    });
    connect(&m_rest, &QTimer::timeout, this, [this] {
        m_blink.stop();
        m_lit = true;
        refresh();
    });
    connect(QGuiApplication::styleHints(), &QStyleHints::cursorFlashTimeChanged, this, &CaretRest::wake);
}

void CaretRest::setTarget(QQuickItem *target)
{
    if (target == m_target) return;
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    if (m_target) m_target->removeEventFilter(this);
    m_target = target;
    if (target) {
        target->installEventFilter(this);
        m_connections.append(connect(target, &QQuickItem::activeFocusChanged, this, &CaretRest::focusChanged));
        // Found by name, since TextInput's and TextEdit's classes are not public API.
        const QMetaObject *meta = target->metaObject();
        if (meta->indexOfSignal("cursorPositionChanged()") >= 0)
            m_connections.append(connect(target, SIGNAL(cursorPositionChanged()), this, SLOT(wake())));
        // Being made read-only or editable again starts Qt's blinking anew, once the change is through.
        if (meta->indexOfSignal("readOnlyChanged(bool)") >= 0)
            m_connections.append(
                connect(target, SIGNAL(readOnlyChanged(bool)), this, SLOT(stillQtCursor()), Qt::QueuedConnection));
    }
    QMetaObject::invokeMethod(this, &CaretRest::stillQtCursor, Qt::QueuedConnection);
    wake();
    emit targetChanged();
}

void CaretRest::setSteady(bool steady)
{
    if (steady == m_steady) return;
    m_steady = steady;
    wake();
    emit steadyChanged();
}

void CaretRest::setRestMs(int restMs)
{
    if (restMs == m_rest.interval()) return;
    m_rest.setInterval(restMs);
    emit restMsChanged();
}

bool CaretRest::shown() const
{
    return m_target && m_target->hasActiveFocus() && m_lit;
}

void CaretRest::wake()
{
    m_lit = true;
    const int flash = QGuiApplication::styleHints()->cursorFlashTime();
    if (m_target && m_target->hasActiveFocus() && !m_steady && flash >= 2) {
        m_blink.setInterval(std::max(1, flash / 2));
        m_blink.start();
        m_rest.start();
    } else {
        m_blink.stop();
        m_rest.stop();
    }
    refresh();
}

void CaretRest::refresh()
{
    if (blinking() != m_wasBlinking) {
        m_wasBlinking = blinking();
        emit blinkingChanged();
    }
    if (shown() != m_shown) {
        m_shown = shown();
        emit shownChanged();
    }
}

void CaretRest::focusChanged()
{
    // Qt starts its blinking as the field takes the keys, after this.
    QMetaObject::invokeMethod(this, &CaretRest::stillQtCursor, Qt::QueuedConnection);
    wake();
}

void CaretRest::stillCursorOf(QQuickItem *item)
{
    if (!item || !item->hasActiveFocus()) return;
    item->setProperty("cursorVisible", true);
    item->setProperty("cursorVisible", false);
}

void CaretRest::stillQtCursor()
{
    // The cursor delegate is drawn by what `shown` says instead.
    stillCursorOf(m_target);
}

bool CaretRest::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_target) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::InputMethod:
        case QEvent::MouseButtonPress:
            wake();
            // A TextEdit whose selection does not persist starts its own
            // blinking again as it handles these, after this.
            QMetaObject::invokeMethod(this, &CaretRest::stillQtCursor, Qt::QueuedConnection);
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}
