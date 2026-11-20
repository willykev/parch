#include "touchpadtests.h"

#include "document.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "touchpadscroll.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QtTest>
#include <functional>
#include <memory>

namespace {

qreal contentY(const QQuickItem *view) { return view->property("contentY").toReal(); }
qreal contentX(const QQuickItem *view) { return view->property("contentX").toReal(); }

// A view of 200 by 100 alone in a window whose touchpad scrolls glide.
struct Scene {
    QQmlEngine engine;
    QString error;
    std::unique_ptr<QQuickWindow> window;
    QQuickItem *view = nullptr;
    TouchpadScroll glide;

    explicit Scene(const QByteArray &content = "contentWidth: 200; contentHeight: 20000")
    {
        window = sceneWindow(engine,
                             "Flickable { width: 200; height: 100; boundsBehavior: Flickable.StopAtBounds; " + content + " }",
                             &error);
        if (!window)
            return;
        view = window->contentItem()->childItems().value(0);
        glide.setTarget(window.get());
    }
    bool ok() const { return window && view; }
};

// Two fingers on a touchpad, as Qt reports them on Wayland: a beginning
// that is no device's, at the moment of the first move, the moves, each
// with how far the fingers went, and an end as they lift. The clock is the
// events' own.
struct Fingers {
    QQuickWindow *window = nullptr;
    QPointF at;
    quint64 time = 1000;

    void send(Qt::ScrollPhase phase, QPoint pixels, Qt::MouseEventSource source)
    {
        QWheelEvent event(at, window->mapToGlobal(at), pixels, pixels * 12, Qt::NoButton, Qt::NoModifier, phase, false,
                          source);
        event.setTimestamp(time);
        QCoreApplication::sendEvent(window, &event);
    }
    void down() { send(Qt::ScrollBegin, {}, Qt::MouseEventNotSynthesized); }
    void move(QPoint pixels, int afterMs = 10)
    {
        time += afterMs;
        send(Qt::ScrollUpdate, pixels, Qt::MouseEventSynthesizedBySystem);
    }
    void lift(int afterMs = 10)
    {
        time += afterMs;
        send(Qt::ScrollEnd, {}, Qt::MouseEventSynthesizedBySystem);
    }
    // `moves` moves of `pixels`, the first as the fingers come down and
    // the rest 10 ms apart, lifted 10 ms after the last.
    void swipe(QPoint pixels, int moves = 15, int liftMs = 10)
    {
        down();
        for (int i = 0; i < moves; ++i)
            move(pixels, i == 0 ? 0 : 10);
        lift(liftMs);
    }
    // A click of a mouse wheel.
    void wheel(int angle)
    {
        time += 10;
        QWheelEvent event(at, window->mapToGlobal(at), {}, {0, angle}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                          false);
        event.setTimestamp(time);
        QCoreApplication::sendEvent(window, &event);
    }
};

// A swipe's speed is how far the view moved over the end of it: for one of
// fifteen moves of 20 pixels, the 180 pixels of the last nine moves over
// the 100 ms from the sixth move to the lift.
constexpr qreal SwipeSpeed = 180.0 * 1000 / 100;
// How far such a swipe moves the view: as far as the fingers.
constexpr qreal SwipeLength = 15 * 20;

// How far a glide that starts at `speed` travels before it ends, slower
// than StopVelocity.
qreal travelAt(qreal speed)
{
    return TouchpadScroll::TimeConstant * (std::abs(speed) - TouchpadScroll::StopVelocity);
}

// Waits for the glide to end, noting where the view was at each moment.
QList<QPair<qint64, qreal>> follow(TouchpadScroll &glide, const std::function<qreal()> &position, int timeoutMs = 5000)
{
    QList<QPair<qint64, qreal>> places;
    QElapsedTimer clock;
    clock.start();
    while (glide.gliding() && clock.elapsed() < timeoutMs) {
        places.append({clock.elapsed(), position()});
        QTest::qWait(16);
    }
    places.append({clock.elapsed(), position()});
    return places;
}

// Prose long enough for both views to scroll far.
QString longDocument()
{
    QString text;
    for (int i = 0; i < 400; ++i)
        text += QStringLiteral("Paragraph %1 has enough words in it to wrap onto a second line in a column of the "
                               "editor, and its words are plain.\n\n")
                    .arg(i);
    return text;
}

} // namespace

namespace TouchpadTests {

void viewsFollowTheFingersFromTheirFirstMove()
{
    // The first move comes at the same moment as the beginning, and each
    // move after it moves the view as far.
    Scene scene;
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};
    fingers.down();
    for (int i = 1; i <= 15; ++i) {
        fingers.move({0, -20}, i == 1 ? 0 : 10);
        // Flickable takes the first move with the second.
        if (i > 1)
            QCOMPARE(contentY(scene.view), 20.0 * i);
    }
    QVERIFY(scene.view->property("synchronousDrag").toBool());
    fingers.lift();
    // Once the fingers lift, the view drags as it did before.
    QVERIFY(!scene.view->property("synchronousDrag").toBool());
    scene.glide.stop();

    // One that already followed the fingers keeps doing so.
    scene.view->setProperty("synchronousDrag", true);
    fingers.swipe({0, 20});
    QCOMPARE(contentY(scene.view), 0.0);
    QVERIFY(scene.view->property("synchronousDrag").toBool());
}

void aSwipeGlidesOnAfterTheFingersLift()
{
    Scene scene;
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};

    fingers.down();
    for (int i = 0; i < 15; ++i) {
        fingers.move({0, -20}, i == 0 ? 0 : 10);
        QVERIFY(!scene.glide.gliding());
    }
    QCOMPARE(contentY(scene.view), SwipeLength);
    fingers.lift();
    QVERIFY(scene.glide.gliding());

    // It goes on by itself, slowing down, and comes to rest after
    // travelling the swipe's speed times the time constant, less the
    // little it would still go slower than StopVelocity.
    QElapsedTimer clock;
    clock.start();
    const auto places = follow(scene.glide, [&] { return contentY(scene.view); });
    QVERIFY(!scene.glide.gliding());
    QVERIFY2(qAbs(contentY(scene.view) - (SwipeLength + travelAt(SwipeSpeed))) < 1,
             qPrintable(QString::number(contentY(scene.view))));
    // On a whole pixel, for sharp text.
    QCOMPARE(contentY(scene.view), std::round(contentY(scene.view)));
    QVERIFY2(clock.elapsed() >= 2000, qPrintable(QString::number(clock.elapsed())));
    const auto at = [&](qint64 ms) {
        for (const auto &[time, position] : places) {
            if (time >= ms)
                return position;
        }
        return places.last().second;
    };
    const qreal early = at(300) - at(0);
    const qreal late = at(1300) - at(1000);
    QVERIFY2(late > 0 && late < early / 2, qPrintable(QStringLiteral("%1 then %2").arg(early).arg(late)));
    for (qsizetype i = 1; i < places.size(); ++i)
        QVERIFY(places[i].second >= places[i - 1].second);

    // The other way, the same.
    const qreal before = contentY(scene.view);
    fingers.swipe({0, 20});
    QCOMPARE(contentY(scene.view), before - SwipeLength);
    QVERIFY(scene.glide.gliding());
    follow(scene.glide, [&] { return contentY(scene.view); });
    QVERIFY(qAbs(contentY(scene.view) - (before - SwipeLength - travelAt(SwipeSpeed))) < 1);
}

void sidewaysSwipesGlideSideways()
{
    Scene scene("contentWidth: 20000; contentHeight: 100");
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};
    fingers.swipe({-20, 0});
    QCOMPARE(contentX(scene.view), SwipeLength);
    QVERIFY(scene.glide.gliding());
    follow(scene.glide, [&] { return contentX(scene.view); });
    QVERIFY2(qAbs(contentX(scene.view) - (SwipeLength + travelAt(SwipeSpeed))) < 1,
             qPrintable(QString::number(contentX(scene.view))));
    QCOMPARE(contentY(scene.view), 0.0);
}

void fingersHeldStillLeaveNoGlide()
{
    Scene scene;
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};

    // Lifted 150 ms after they stopped moving: nothing of the swipe's speed
    // is left.
    fingers.swipe({0, -20}, 15, 150);
    QVERIFY(!scene.glide.gliding());
    QTest::qWait(100);
    QCOMPARE(contentY(scene.view), SwipeLength);

    // Lifted 60 ms after: only what moved in the 100 ms before the lift
    // counts, the 80 pixels of the last four moves.
    fingers.swipe({0, -20}, 15, 60);
    QCOMPARE(contentY(scene.view), 2 * SwipeLength);
    QVERIFY(scene.glide.gliding());
    follow(scene.glide, [&] { return contentY(scene.view); });
    QVERIFY2(qAbs(contentY(scene.view) - (2 * SwipeLength + travelAt(80.0 * 1000 / 100))) < 1,
             qPrintable(QString::number(contentY(scene.view))));

    // A swipe too slow to glide.
    const qreal before = contentY(scene.view);
    fingers.down();
    for (int i = 0; i < 6; ++i)
        fingers.move({0, -1}, i == 0 ? 0 : 100);
    fingers.lift();
    QVERIFY(!scene.glide.gliding());
    QCOMPARE(contentY(scene.view), before + 6);
}

void aMouseWheelDoesNotGlide()
{
    Scene scene;
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};

    // The view scrolls, as it always did, and no glide follows.
    for (int i = 0; i < 3; ++i) {
        fingers.wheel(-120);
        QVERIFY(!scene.glide.gliding());
    }
    QTRY_VERIFY(contentY(scene.view) > 0);
    QTest::qWait(50);
    QVERIFY(!scene.glide.gliding());
    QVERIFY(!scene.view->property("synchronousDrag").toBool());

    // A click of the wheel stops a glide.
    fingers.swipe({0, -20});
    QVERIFY(scene.glide.gliding());
    fingers.wheel(-120);
    QVERIFY(!scene.glide.gliding());
}

void aGlideStopsAtTheEndOfTheView()
{
    Scene scene("contentWidth: 200; contentHeight: 600");
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};

    fingers.swipe({0, -20});
    QCOMPARE(contentY(scene.view), SwipeLength);
    QVERIFY(scene.glide.gliding());
    const auto places = follow(scene.glide, [&] { return contentY(scene.view); }, 1000);
    QVERIFY(!scene.glide.gliding());
    QCOMPARE(contentY(scene.view), 500.0);
    for (const auto &[time, position] : places)
        QVERIFY2(position <= 500, qPrintable(QString::number(position)));

    // At the end, a swipe on towards it moves nothing and does not glide.
    fingers.swipe({0, -20});
    QVERIFY(!scene.glide.gliding());
    QCOMPARE(contentY(scene.view), 500.0);
}

void scrollingClicksAndKeysStopAGlide()
{
    const QList<QPair<const char *, std::function<void(Scene &, Fingers &)>>> stoppers = {
        {"fingers on the touchpad again", [](Scene &, Fingers &fingers) { fingers.down(); }},
        {"a click",
         [](Scene &scene, Fingers &fingers) {
             QMouseEvent press(QEvent::MouseButtonPress, fingers.at, scene.window->mapToGlobal(fingers.at), Qt::LeftButton,
                               Qt::LeftButton, Qt::NoModifier);
             QCoreApplication::sendEvent(scene.window.get(), &press);
             QMouseEvent release(QEvent::MouseButtonRelease, fingers.at, scene.window->mapToGlobal(fingers.at),
                                 Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
             QCoreApplication::sendEvent(scene.window.get(), &release);
         }},
        {"a key",
         [](Scene &scene, Fingers &) {
             QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
             QCoreApplication::sendEvent(scene.window.get(), &press);
             QKeyEvent release(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
             QCoreApplication::sendEvent(scene.window.get(), &release);
         }},
    };
    for (const auto &[name, stopper] : stoppers) {
        Scene scene;
        QVERIFY2(scene.ok(), qPrintable(scene.error));
        Fingers fingers{scene.window.get(), {100, 50}};
        fingers.swipe({0, -20});
        QTRY_VERIFY2(contentY(scene.view) > SwipeLength + 50, name);
        stopper(scene, fingers);
        QVERIFY2(!scene.glide.gliding(), name);
        const qreal stopped = contentY(scene.view);
        QTest::qWait(100);
        QVERIFY2(contentY(scene.view) == stopped, name);
    }
}

void aViewMovedElsewhereStopsItsGlide()
{
    Scene scene;
    QVERIFY2(scene.ok(), qPrintable(scene.error));
    Fingers fingers{scene.window.get(), {100, 50}};
    fingers.swipe({0, -20});
    QTRY_VERIFY(contentY(scene.view) > SwipeLength + 50);
    // As a jump to a heading or a caret kept in view moves it.
    scene.view->setProperty("contentY", 40);
    QTRY_VERIFY(!scene.glide.gliding());
    QTest::qWait(100);
    QCOMPARE(contentY(scene.view), 40.0);

    // A view that is hidden is left where it is.
    fingers.swipe({0, -20});
    QTRY_VERIFY(contentY(scene.view) > 40 + SwipeLength + 50);
    scene.view->setVisible(false);
    QTRY_VERIFY(!scene.glide.gliding());
    const qreal hidden = contentY(scene.view);
    QTest::qWait(100);
    QCOMPARE(contentY(scene.view), hidden);
}

void aShortSwipeAlongAGlideAddsToIt()
{
    const auto travel = [](const std::function<void(Fingers &)> &again) {
        Scene scene;
        if (!scene.ok())
            return qreal(-1);
        scene.view->setProperty("contentY", 5000);
        Fingers fingers{scene.window.get(), {100, 50}};
        fingers.swipe({0, -20});
        QTest::qWait(100);
        again(fingers);
        const qreal released = contentY(scene.view);
        follow(scene.glide, [&] { return contentY(scene.view); });
        return contentY(scene.view) - released;
    };

    // The glide still had most of its speed, and adds it to the swipe's.
    const qreal along = travel([](Fingers &fingers) { fingers.swipe({0, -20}); });
    QVERIFY2(along > 1.3 * travelAt(SwipeSpeed), qPrintable(QString::number(along)));
    // A swipe the other way, or a long one, has only its own.
    const qreal against = travel([](Fingers &fingers) { fingers.swipe({0, 20}); });
    QVERIFY2(qAbs(against + travelAt(SwipeSpeed)) < 1, qPrintable(QString::number(against)));
    const qreal slow = travel([](Fingers &fingers) { fingers.swipe({0, -20}, 40); });
    QVERIFY2(qAbs(slow - travelAt(SwipeSpeed)) < 1, qPrintable(QString::number(slow)));
}

void theEditorAndThePreviewGlide()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1800);
    window->setHeight(800);
    shell.settings()->setScrollSync(false);
    shell.document()->buffer()->replace({0, 0}, longDocument());
    shell.context()->tabs()->setActiveMode(TabModel::Split);

    auto *scroll = qobject_cast<TouchpadScroll *>(shell.object(QStringLiteral("touchpadScroll")));
    QVERIFY(scroll);
    QCOMPARE(scroll->target(), window);
    quint64 time = 1000;
    for (const QString name : {QStringLiteral("flickable"), QStringLiteral("previewFlickable")}) {
        QQuickItem *view = shell.item(name);
        QVERIFY2(view, qPrintable(name));
        QTRY_VERIFY2(view->isVisible() && view->property("contentHeight").toReal() > 8000, qPrintable(name));
        Fingers fingers{window, view->mapToScene(QPointF(view->width() / 2, view->height() / 2)), time};
        const qreal before = contentY(view);
        fingers.swipe({0, -20});
        time = fingers.time;
        QVERIFY2(qAbs(contentY(view) - (before + SwipeLength)) < 1,
                 qPrintable(QStringLiteral("%1 at %2").arg(name).arg(contentY(view))));
        QVERIFY2(scroll->gliding(), qPrintable(name));
        const qreal released = contentY(view);
        follow(*scroll, [&] { return contentY(view); });
        QVERIFY2(qAbs(contentY(view) - (released + travelAt(SwipeSpeed))) < 1,
                 qPrintable(QStringLiteral("%1 at %2").arg(name).arg(contentY(view))));
    }
}

} // namespace TouchpadTests
