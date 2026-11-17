#include "quiettimer.h"

#include <algorithm>

QuietTimer::QuietTimer(QObject *parent) : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        // A single shot has stopped by the time it is said.
        if (!m_timer.isActive()) emit runningChanged();
        emit triggered();
    });
}

void QuietTimer::setInterval(int interval)
{
    interval = std::max(0, interval);
    if (interval == m_timer.interval()) return;
    // QTimer starts an active timer again with its new interval.
    m_timer.setInterval(interval);
    emit intervalChanged();
}

void QuietTimer::setRepeat(bool repeat)
{
    if (repeat == this->repeat()) return;
    m_timer.setSingleShot(!repeat);
    emit repeatChanged();
}

void QuietTimer::setRunning(bool running)
{
    if (running) start();
    else stop();
}

void QuietTimer::start()
{
    if (m_timer.isActive()) return;
    m_timer.start();
    emit runningChanged();
}

void QuietTimer::stop()
{
    if (!m_timer.isActive()) return;
    m_timer.stop();
    emit runningChanged();
}

void QuietTimer::restart()
{
    const bool was = m_timer.isActive();
    m_timer.start();
    if (!was) emit runningChanged();
}
