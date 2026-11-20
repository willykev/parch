#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantAnimation>
#include <vector>

class QWheelEvent;

// Makes scrolling with a touchpad move the window's views as far as the
// fingers do, and glide on after they lift, as the rest of the desktop does.
//
// Qt's Flickable loses the start of every swipe: it drops the first move of
// the fingers, which comes at the same moment as the swipe's beginning, and
// takes the next as a threshold to cross before the content moves. While
// the fingers are on the touchpad, each view under them is made to follow
// them from the first move. It also stops the moment they lift, and on
// Linux nothing else carries a view on: a quick swipe moved the text a short
// way and stopped dead.
//
// Qt marks a touchpad scroll with a beginning and an end; a mouse wheel's
// clicks have neither, and keep scrolling as they did. While the fingers
// move, this notes where each view under them is. When they lift, every
// view that moved goes on at the speed it had over the last
// VelocityWindowMs, losing the same share of its speed in every moment, and
// comes to rest after travelling about its speed times TimeConstant. Fingers
// held still before they lift leave no speed, and the view stays where they
// left it.
//
// A glide stops at the end of the view's content, when the fingers scroll
// again, at a click, a touch or a key, and when something else moves the
// view. A short swipe in the direction of a glide that is under way adds the
// glide's speed to its own, so that a few swipes cross a long document.
class TouchpadScroll : public QObject {
    Q_OBJECT
    // The window whose touchpad scrolls this handles.
    Q_PROPERTY(QQuickWindow *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(bool gliding READ gliding NOTIFY glidingChanged)

public:
    // How long, in seconds, a glide takes to lose all but 1/e of its speed.
    static constexpr qreal TimeConstant = 0.5;
    // How much of the end of a swipe gives its speed.
    static constexpr int VelocityWindowMs = 100;
    // Slower than this, in pixels a second, a view does not glide, and a
    // glide ends.
    static constexpr qreal StopVelocity = 20;
    static constexpr qreal MaxVelocity = 20000;
    // How long a swipe that adds a glide's speed to its own can be.
    static constexpr int BoostMs = 300;
    // How much earlier than the first move a swipe's beginning is made to
    // seem, so that Flickable takes that move: about one of the touchpad's
    // frames.
    static constexpr int FrameMs = 10;

    explicit TouchpadScroll(QObject *parent = nullptr);
    ~TouchpadScroll() override;

    QQuickWindow *target() const { return m_target; }
    void setTarget(QQuickWindow *target);
    bool gliding() const { return !m_glides.empty(); }

    // Ends a glide where the view is now.
    Q_INVOKABLE void stop();

signals:
    void targetChanged();
    void glidingChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Sample {
        qint64 time = 0;
        QPointF position;
    };
    // A view under the fingers while they are on the touchpad.
    struct Tracked {
        QPointer<QQuickItem> view;
        QPointF began;
        std::vector<Sample> samples;
        // The speed of the glide this swipe stopped.
        QPointF boost;
        // Whether the view followed the fingers from their first move by
        // itself, before the swipe made it.
        bool followed = false;
    };
    struct Glide {
        QPointer<QQuickItem> view;
        QPointF from;
        QPointF velocity;
        // Where the glide last put the view.
        QPointF placed;
        bool across = false;
        bool down = false;
    };

    void wheel(QWheelEvent *event);
    void begin(const QPointF &scenePosition, qint64 time);
    void record();
    void release(qint64 time);
    // Gives the views of the swipe back the following they had.
    void untrack();
    void step(qreal seconds);
    QPointF speedNow(const Glide &glide) const;
    void setGlides(std::vector<Glide> glides);

    QPointer<QQuickWindow> m_target;
    QVariantAnimation m_motion;
    std::vector<Tracked> m_tracked;
    std::vector<Glide> m_glides;
    bool m_tracking = false;
    qint64 m_began = 0;
    qint64 m_last = 0;
    int m_updates = 0;
    // Counts the glides stopped, so that a step that moved a view can tell
    // whether that ended its glide.
    quint64 m_stops = 0;
};
