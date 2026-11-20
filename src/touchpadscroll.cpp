#include "touchpadscroll.h"

#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {

QPointF positionOf(const QQuickItem *view)
{
    return {view->property("contentX").toReal(), view->property("contentY").toReal()};
}

// The lowest and the highest contentX or contentY of a Flickable whose
// content stays within its bounds.
struct Range {
    qreal lowest = 0;
    qreal highest = 0;
};

Range rangeOf(const QQuickItem *view, Qt::Orientation orientation)
{
    const bool across = orientation == Qt::Horizontal;
    const qreal origin = view->property(across ? "originX" : "originY").toReal();
    const qreal size = view->property(across ? "contentWidth" : "contentHeight").toReal();
    const qreal before = view->property(across ? "leftMargin" : "topMargin").toReal();
    const qreal after = view->property(across ? "rightMargin" : "bottomMargin").toReal();
    const qreal shown = across ? view->width() : view->height();
    const qreal lowest = origin - before;
    return {lowest, std::max(lowest, origin + size + after - shown)};
}

// Every Flickable under a point of the scene, whatever is drawn over it: a
// view that the fingers do not move is left out when they lift.
void collectViews(QQuickItem *item, const QPointF &scenePosition, std::vector<QQuickItem *> &found)
{
    if (!item->isVisible())
        return;
    const bool inside = item->contains(item->mapFromScene(scenePosition));
    if (item->clip() && !inside)
        return;
    if (inside && item->inherits("QQuickFlickable"))
        found.push_back(item);
    for (QQuickItem *child : item->childItems())
        collectViews(child, scenePosition, found);
}

} // namespace

TouchpadScroll::TouchpadScroll(QObject *parent) : QObject(parent)
{
    m_motion.setStartValue(0.0);
    m_motion.setEasingCurve(QEasingCurve::Linear);
    connect(&m_motion, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { step(value.toReal() / 1000); });
    connect(&m_motion, &QVariantAnimation::finished, this, [this] { setGlides({}); });
}

TouchpadScroll::~TouchpadScroll()
{
    untrack();
    if (m_target)
        m_target->removeEventFilter(this);
}

void TouchpadScroll::setTarget(QQuickWindow *target)
{
    if (m_target == target)
        return;
    stop();
    m_tracking = false;
    untrack();
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    if (m_target)
        m_target->installEventFilter(this);
    emit targetChanged();
}

void TouchpadScroll::stop()
{
    m_motion.stop();
    ++m_stops;
    setGlides({});
}

void TouchpadScroll::setGlides(std::vector<Glide> glides)
{
    const bool was = gliding();
    m_glides = std::move(glides);
    if (was != gliding())
        emit glidingChanged();
}

bool TouchpadScroll::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_target)
        return false;
    switch (event->type()) {
    case QEvent::Wheel:
        wheel(static_cast<QWheelEvent *>(event));
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
    case QEvent::KeyPress:
    case QEvent::NativeGesture:
    case QEvent::Hide:
        stop();
        break;
    default:
        break;
    }
    return false;
}

void TouchpadScroll::wheel(QWheelEvent *event)
{
    const auto time = qint64(event->timestamp());
    switch (event->phase()) {
    case Qt::ScrollBegin:
        begin(event->scenePosition(), time);
        // The first move comes at the same moment as the beginning, and
        // Flickable drops an event no later than the one before it.
        if (time > FrameMs)
            event->setTimestamp(quint64(time - FrameMs));
        break;
    case Qt::ScrollUpdate:
        if (!m_tracking)
            break;
        if (time < m_last) {
            // The clock went back: what was noted no longer gives a speed.
            begin(event->scenePosition(), time);
            break;
        }
        // Before Qt delivers this event the views are where the one before
        // it put them; the first finds them where the fingers found them.
        if (m_updates++ > 0)
            record();
        m_last = time;
        break;
    case Qt::ScrollEnd:
        if (!m_tracking)
            break;
        if (m_updates > 0)
            record();
        m_tracking = false;
        release(time);
        break;
    case Qt::ScrollMomentum:
        // The platform carries the scroll on itself.
        stop();
        m_tracking = false;
        untrack();
        break;
    case Qt::NoScrollPhase:
        // A mouse wheel's click, or a device that does not say when it
        // stops: no glide follows, and one under way ends.
        stop();
        break;
    }
}

void TouchpadScroll::begin(const QPointF &scenePosition, qint64 time)
{
    // The fingers stop a glide; a short swipe may add its speed to their own.
    std::vector<std::pair<QPointer<QQuickItem>, QPointF>> speeds;
    for (const Glide &glide : m_glides)
        speeds.emplace_back(glide.view, speedNow(glide));
    stop();

    untrack();
    m_tracking = true;
    m_began = m_last = time;
    m_updates = 0;
    if (!m_target)
        return;
    std::vector<QQuickItem *> views;
    collectViews(m_target->contentItem(), scenePosition, views);
    for (QQuickItem *view : views) {
        Tracked tracked{view, positionOf(view), {}, {}, view->property("synchronousDrag").toBool()};
        // Without it, Flickable holds the content until the fingers have
        // moved some way, and that way is lost.
        if (!tracked.followed)
            view->setProperty("synchronousDrag", true);
        for (const auto &[gliding, speed] : speeds) {
            if (gliding == view)
                tracked.boost = speed;
        }
        m_tracked.push_back(std::move(tracked));
    }
}

void TouchpadScroll::untrack()
{
    for (const Tracked &tracked : std::exchange(m_tracked, {})) {
        if (tracked.view && !tracked.followed)
            tracked.view->setProperty("synchronousDrag", false);
    }
}

void TouchpadScroll::record()
{
    for (Tracked &tracked : m_tracked) {
        if (!tracked.view)
            continue;
        auto &samples = tracked.samples;
        samples.push_back({m_last, positionOf(tracked.view)});
        // Only the last place before the window, and what is in it, count.
        while (samples.size() > 2 && samples[1].time <= m_last - 2 * VelocityWindowMs)
            samples.erase(samples.begin());
    }
}

void TouchpadScroll::release(qint64 time)
{
    std::vector<Glide> glides;
    qreal longest = 0;
    const bool boosts = time - m_began <= BoostMs;
    for (const Tracked &tracked : m_tracked) {
        QQuickItem *view = tracked.view;
        if (!view || !view->isVisible() || tracked.samples.size() < 2)
            continue;

        // The speed of the swipe: how far the view moved from the last
        // place it had at least VelocityWindowMs before the fingers lifted,
        // or from the first, over the time since. Fingers held still that
        // long give none.
        const Sample &last = tracked.samples.back();
        const Sample *from = &tracked.samples.front();
        for (const Sample &sample : tracked.samples) {
            if (sample.time > time - VelocityWindowMs)
                break;
            from = &sample;
        }
        if (from == &last || time <= from->time)
            continue;

        const QPointF now = positionOf(view);
        Glide glide{view, now, {}, now};
        const auto speedAlong = [&](qreal moved, qreal began, qreal current, qreal boost, Qt::Orientation orientation) {
            if (std::abs(current - began) < 0.5)
                return qreal(0);
            qreal speed = moved * 1000 / qreal(time - from->time);
            if (boosts && boost * speed > 0)
                speed += boost;
            speed = std::clamp(speed, -MaxVelocity, MaxVelocity);
            const auto [lowest, highest] = rangeOf(view, orientation);
            // A view beyond its bounds goes back by itself.
            if (std::abs(speed) < StopVelocity || current < lowest || current > highest)
                return qreal(0);
            return speed;
        };
        glide.velocity = {
            speedAlong(last.position.x() - from->position.x(), tracked.began.x(), now.x(), tracked.boost.x(), Qt::Horizontal),
            speedAlong(last.position.y() - from->position.y(), tracked.began.y(), now.y(), tracked.boost.y(), Qt::Vertical)};
        glide.across = glide.velocity.x() != 0;
        glide.down = glide.velocity.y() != 0;
        if (!glide.across && !glide.down)
            continue;
        const qreal fastest = std::max(std::abs(glide.velocity.x()), std::abs(glide.velocity.y()));
        longest = std::max(longest, TimeConstant * std::log(fastest / StopVelocity));
        glides.push_back(glide);
    }
    untrack();
    if (glides.empty())
        return;
    setGlides(std::move(glides));
    const int duration = int(std::ceil(longest * 1000));
    m_motion.setDuration(duration);
    m_motion.setEndValue(qreal(duration));
    m_motion.start();
}

QPointF TouchpadScroll::speedNow(const Glide &glide) const
{
    if (m_motion.state() != QAbstractAnimation::Running)
        return {};
    const qreal left = std::exp(-m_motion.currentTime() / 1000.0 / TimeConstant);
    return {glide.across ? glide.velocity.x() * left : 0, glide.down ? glide.velocity.y() * left : 0};
}

void TouchpadScroll::step(qreal seconds)
{
    const qreal left = std::exp(-seconds / TimeConstant);
    const quint64 stops = m_stops;
    const bool was = gliding();
    for (std::size_t i = 0; i < m_glides.size();) {
        Glide &glide = m_glides[i];
        QQuickItem *view = glide.view;
        const QPointF now = view ? positionOf(view) : QPointF();
        // A view that is gone, hidden or moved by something else is left
        // where it is.
        if (!view || !view->isVisible() || std::abs(now.x() - glide.placed.x()) > 1
            || std::abs(now.y() - glide.placed.y()) > 1) {
            m_glides.erase(m_glides.begin() + qsizetype(i));
            continue;
        }
        const auto place = [&](bool &going, qreal from, qreal velocity, Qt::Orientation orientation, const char *name) {
            if (!going)
                return;
            // At its speed less what it has lost, the view has travelled
            // the speed it lost times TimeConstant.
            qreal to = from + velocity * TimeConstant * (1 - left);
            const auto [lowest, highest] = rangeOf(view, orientation);
            if (to <= lowest || to >= highest) {
                to = std::clamp(to, lowest, highest);
                going = false;
            } else if (std::abs(velocity * left) < StopVelocity) {
                // At rest on a whole pixel, as Flickable leaves its own
                // movements, so that the text is drawn sharp.
                to = std::clamp(std::round(to), lowest, highest);
                going = false;
            }
            view->setProperty(name, to);
        };
        place(glide.across, glide.from.x(), glide.velocity.x(), Qt::Horizontal, "contentX");
        if (m_stops != stops)
            return;
        place(glide.down, glide.from.y(), glide.velocity.y(), Qt::Vertical, "contentY");
        if (m_stops != stops)
            return;
        glide.placed = positionOf(view);
        if (!glide.across && !glide.down)
            m_glides.erase(m_glides.begin() + qsizetype(i));
        else
            ++i;
    }
    if (m_glides.empty()) {
        m_motion.stop();
        if (was)
            emit glidingChanged();
    }
}
