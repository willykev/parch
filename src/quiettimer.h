#pragma once

#include <QObject>
#include <QTimer>

// A timer for QML with the interval, repeat, running, triggered(), start(),
// stop() and restart() of Qt Quick's Timer, which runs on a QTimer instead.
// Qt Quick's Timer is an animation of Qt Quick's: when the last one stops,
// as the caret's do once it comes to rest, Qt starts and stops its animation
// driver, and the threaded render loop the desktop uses then draws every
// window once more with nothing changed. This one starts and stops without
// asking any window for a frame, so that a window left alone draws nothing.
class QuietTimer : public QObject {
    Q_OBJECT
    Q_PROPERTY(int interval READ interval WRITE setInterval NOTIFY intervalChanged)
    Q_PROPERTY(bool repeat READ repeat WRITE setRepeat NOTIFY repeatChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)

public:
    explicit QuietTimer(QObject *parent = nullptr);

    int interval() const { return m_timer.interval(); }
    // A running timer starts its new interval from now, as Qt Quick's does.
    void setInterval(int interval);
    bool repeat() const { return !m_timer.isSingleShot(); }
    void setRepeat(bool repeat);
    bool running() const { return m_timer.isActive(); }
    void setRunning(bool running);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    // Starts it from now, whether it was running or not.
    Q_INVOKABLE void restart();

signals:
    void intervalChanged();
    void repeatChanged();
    void runningChanged();
    void triggered();

private:
    QTimer m_timer;
};
