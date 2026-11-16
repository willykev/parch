#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

// A caret for one of the window's text fields that blinks for a while and
// then rests, as the editor's does. Qt's TextInput and TextEdit blink their
// cursor on a timer of their own for as long as they have the keys, and each
// blink draws the window again. This stills that timer and gives the field's
// cursor delegate a caret to show instead: it blinks at the desktop's cursor
// flash time, is steady when that is below 2 or when `steady` is set, as for
// reduced motion, and rests lit once nothing has happened for restMs, after
// which an idle window draws nothing. A key, input method text, a click, a
// move of the cursor or the focus lights it at once and starts it again.
class CaretRest : public QObject {
    Q_OBJECT
    // The TextInput or TextEdit, a TextField or TextArea among them.
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(bool steady READ steady WRITE setSteady NOTIFY steadyChanged)
    Q_PROPERTY(int restMs READ restMs WRITE setRestMs NOTIFY restMsChanged)
    // Whether the cursor delegate is to show the caret now.
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    // Whether the caret is blinking, rather than steady or resting.
    Q_PROPERTY(bool blinking READ blinking NOTIFY blinkingChanged)

public:
    // How long nothing must happen before a caret rests: the window's text
    // fields' and the editor's alike, which QML reads as CaretRest.RestMs.
    enum Timing { RestMs = 10000 };
    Q_ENUM(Timing)

    // Stops the blink timer Qt's TextInput or TextEdit runs while the item
    // has the keys, by showing its cursor and hiding it again, which only
    // takes while the item has the keys. The editor's binding does the same
    // for the editor, at the moments it knows of.
    static void stillCursorOf(QQuickItem *item);

    explicit CaretRest(QObject *parent = nullptr);

    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);
    bool steady() const { return m_steady; }
    void setSteady(bool steady);
    int restMs() const { return m_rest.interval(); }
    void setRestMs(int restMs);
    bool shown() const;
    bool blinking() const { return m_blink.isActive(); }

signals:
    void targetChanged();
    void steadyChanged();
    void restMsChanged();
    void shownChanged();
    void blinkingChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // Lights the caret, and blinks it from now while it has the keys and may blink.
    void wake();
    // Stops Qt's own blink, which starts again whenever the field takes the keys.
    void stillQtCursor();

private:
    void focusChanged();
    // Says what changed of `blinking` and `shown`.
    void refresh();

    QPointer<QQuickItem> m_target;
    QList<QMetaObject::Connection> m_connections;
    QTimer m_blink;
    QTimer m_rest;
    bool m_steady = false;
    bool m_lit = true;
    // What was said last.
    bool m_shown = false;
    bool m_wasBlinking = false;
};
