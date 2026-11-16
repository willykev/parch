#include "benchmark.h"

#include "document.h"
#include "editorbinding.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSGRendererInterface>
#include <QScreen>
#include <QStyleHints>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <mutex>
#include <sys/resource.h>
#include <unistd.h>

namespace {

// Keys typed at each of three places, and far jumps.
constexpr int Keys = 20;
constexpr int Jumps = 10;

double toMs(qint64 ns) { return double(ns) / 1e6; }

QJsonObject stats(std::vector<double> values)
{
    QJsonObject result;
    result["n"] = int(values.size());
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    const auto pick = [&values](double q) {
        return values[std::min(values.size() - 1, size_t(std::ceil(q * double(values.size()))) - 1)];
    };
    result["p50"] = pick(0.50);
    result["p95"] = pick(0.95);
    result["p99"] = pick(0.99);
    result["max"] = values.back();
    return result;
}

// /proc/<pid>/stat from the field after the command name, which may itself
// hold spaces and brackets: the state is the first entry, field 3.
QList<QByteArray> statFields(const QString &pid)
{
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray line = file.readAll();
    const qsizetype close = line.lastIndexOf(')');
    if (close < 0) return {};
    return line.mid(close + 2).trimmed().split(' ');
}

qint64 statField(const QList<QByteArray> &fields, int field) { return fields.value(field - 3).toLongLong(); }

// CPU time of this process, and of the children it has waited for, in clock ticks.
qint64 cpuTicks() { const auto f = statFields(QStringLiteral("self")); return statField(f, 14) + statField(f, 15); }
qint64 childCpuTicks() { const auto f = statFields(QStringLiteral("self")); return statField(f, 16) + statField(f, 17); }

// When the kernel started this process, on the steady clock.
qint64 processStartNs()
{
    const qint64 ticks = statField(statFields(QStringLiteral("self")), 22);
    const qint64 sinceBoot = ticks * 1000000000LL / sysconf(_SC_CLK_TCK);
    timespec boot{}, monotonic{};
    clock_gettime(CLOCK_BOOTTIME, &boot);
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    const qint64 offset = (qint64(boot.tv_sec) - qint64(monotonic.tv_sec)) * 1000000000LL
                          + (qint64(boot.tv_nsec) - qint64(monotonic.tv_nsec));
    return sinceBoot - offset;
}

double pssMiB(const QString &pid)
{
    QFile file(QStringLiteral("/proc/%1/smaps_rollup").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray &line : file.readAll().split('\n'))
        if (line.startsWith("Pss:")) return line.mid(4).trimmed().split(' ').first().toDouble() / 1024.0;
    return -1;
}

// Every process this one started, and theirs, that is still running, with
// what they hold: parch's own helpers count in its memory.
QJsonObject ownedProcesses()
{
    QHash<qint64, QList<qint64>> children;
    for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool number = false;
        const qint64 pid = entry.toLongLong(&number);
        if (!number) continue;
        const auto fields = statFields(entry);
        if (!fields.isEmpty()) children[statField(fields, 4)].append(pid);
    }
    QList<qint64> owned, waiting = children.value(qint64(getpid()));
    while (!waiting.isEmpty()) {
        const qint64 pid = waiting.takeFirst();
        owned.append(pid);
        waiting.append(children.value(pid));
    }
    double pss = 0;
    for (const qint64 pid : std::as_const(owned)) pss += std::max(0.0, pssMiB(QString::number(pid)));
    return {{"count", int(owned.size())}, {"pssMiB", pss}};
}

QJsonObject usage(int who)
{
    rusage used{};
    getrusage(who, &used);
    const auto ms = [](const timeval &t) { return double(t.tv_sec) * 1000.0 + double(t.tv_usec) / 1000.0; };
    return {{"peakRssMiB", double(used.ru_maxrss) / 1024.0},
            {"cpuTimeMs", QJsonObject{{"user", ms(used.ru_utime)}, {"system", ms(used.ru_stime)}}}};
}

QString graphicsApiName(QQuickWindow *window)
{
    QSGRendererInterface *renderer = window->rendererInterface();
    if (!renderer) return QStringLiteral("unknown");
    switch (renderer->graphicsApi()) {
    case QSGRendererInterface::Software: return QStringLiteral("software");
    case QSGRendererInterface::OpenGL: return QStringLiteral("opengl");
    case QSGRendererInterface::Vulkan: return QStringLiteral("vulkan");
    case QSGRendererInterface::Null: return QStringLiteral("null");
    default: return QStringLiteral("other");
    }
}

QObject *findNamed(QQuickItem *root, const QString &name)
{
    if (!root) return nullptr;
    for (QQuickItem *child : root->childItems()) {
        if (child->objectName() == name) return child;
        if (QObject *found = findNamed(child, name)) return found;
    }
    return nullptr;
}

// The scroll: 60 pixels a frame down the flickable, until its end.
const QByteArray scrollerQml = R"(
import QtQuick
FrameAnimation {
    required property Item target
    onTriggered: target.contentY = Math.min(Math.max(0, target.contentHeight - target.height), target.contentY + 60)
}
)";

// The re-wrap: a right margin that grows 4 pixels a frame for 60 frames and
// then shrinks for as many, on and on, narrowing and widening the column.
const QByteArray wrapperQml = R"(
import QtQuick
FrameAnimation {
    required property QtObject target
    required property real base
    property real offset: 0
    property real step: 4
    property int steps: 0
    signal stepped()
    onTriggered: {
        offset += step
        target.rightMargin = base + offset
        if (++steps % 60 === 0)
            step = -step
        stepped()
    }
}
)";

// The control: the editor's place, height and column with plain stripes
// instead of text, every colour from the theme.
const QByteArray controlQml = R"(
import QtQuick
Rectangle {
    id: control
    required property Item pane
    required property real textHeight
    required property real stripeWidth
    property real rightMargin: 0
    x: pane.x
    y: pane.y
    width: pane.width - rightMargin
    height: pane.height
    color: theme.background
    Flickable {
        id: flick
        objectName: "controlFlickable"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: control.textHeight
        boundsBehavior: Flickable.StopAtBounds
        Repeater {
            model: Math.ceil(control.textHeight / 100)
            Rectangle {
                required property int index
                y: index * 100
                x: pane.margin
                width: control.stripeWidth
                height: 50
                color: index % 2 ? theme.panel : theme.controlFill
            }
        }
        // Where the text column would be: it follows the width as the column does.
        Item {
            objectName: "controlColumn"
            x: Math.round((flick.width - width) / 2)
            width: Math.max(0, Math.min(flick.width - 2 * pane.margin, pane.maxLineLength * pane.characterWidth))
            height: control.textHeight
        }
    }
}
)";

} // namespace

struct Benchmark::Frames {
    mutable std::mutex mutex;
    std::vector<qint64> times;
};

Benchmark::Benchmark(const QString &output, const QString &label, const Timing &timing, QObject *parent)
    : QObject(parent), m_output(output), m_label(label), m_timing(timing), m_frames(std::make_shared<Frames>())
{
    m_poll.setTimerType(Qt::PreciseTimer);
    m_poll.setInterval(1);
    connect(&m_poll, &QTimer::timeout, this, &Benchmark::waitForEditor);
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        fail(tr("the measurement did not finish in %1 seconds").arg(m_watchdog.interval() / 1000));
    });
}

Benchmark::Benchmark(const QString &output, const QString &label, QObject *parent)
    : Benchmark(output, label, Timing{}, parent)
{
}

Benchmark::~Benchmark()
{
    stopDrivers();
    delete m_control;
}

qint64 Benchmark::now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString Benchmark::copyIn(const QString &file, const QString &folder, QString *error)
{
    const QFileInfo source(file);
    if (!source.isFile()) {
        *error = tr("there is no file %1 to measure").arg(file);
        return {};
    }
    const QString copy = folder + u'/' + source.fileName();
    if (!QDir().mkpath(folder) || !QFile::copy(file, copy)) {
        *error = tr("cannot copy %1 to measure it").arg(file);
        return {};
    }
    // The copy is written to as the document is saved.
    QFile::setPermissions(copy, QFile::ReadOwner | QFile::WriteOwner);
    return copy;
}

void Benchmark::setLaunchTime(qint64 mainNs) { m_mainNs = mainNs; }

void Benchmark::opening()
{
    m_openedNs = now();
    m_out["pssEmptyMiB"] = pssMiB(QStringLiteral("self"));
}

void Benchmark::start(QQuickWindow *window)
{
    m_window = window;
    if (!m_openedNs) opening();
    m_pane = window ? qobject_cast<QQuickItem *>(findNamed(window->contentItem(), QStringLiteral("editorPane"))) : nullptr;
    if (m_pane) {
        m_editor = m_pane->findChild<QQuickItem *>(QStringLiteral("editor"));
        m_flickable = m_pane->findChild<QQuickItem *>(QStringLiteral("flickable"));
        m_binding = m_pane->findChild<EditorBinding *>(QStringLiteral("binding"));
    }
    if (!m_editor || !m_flickable || !m_binding) {
        fail(tr("the window has no editor to measure"));
        return;
    }
    // Frames are counted as they are swapped, on the render thread. The log
    // is shared so that a frame still being counted outlives this object.
    std::shared_ptr<Frames> frames = m_frames;
    connect(window, &QQuickWindow::frameSwapped, this, [frames] {
        const qint64 t = now();
        std::lock_guard<std::mutex> lock(frames->mutex);
        frames->times.push_back(t);
    }, Qt::DirectConnection);
    // The editor takes a new width only when it lays its text out again.
    connect(m_editor, &QQuickItem::widthChanged, this, [this] { m_widthChanges.push_back(now()); });

    const Timing &t = m_timing;
    const qint64 total = qint64(t.waitMs) * 2 + t.settleMs + 3 * (2 * t.placeMs + Keys * t.keyMs) + t.placeMs
                         + 2 * (t.scrollMs + t.rewrapMs) + Jumps * t.jumpMs + t.quietMs + t.idleMs + 10000;
    m_watchdog.start(int(std::min<qint64>(total + 60000, std::numeric_limits<int>::max())));
    m_window->requestActivate();
    m_poll.start();
}

bool Benchmark::editorReady() const
{
    Document *document = m_binding ? m_binding->document() : nullptr;
    if (!document || !m_editor || !m_binding->editable()) return false;
    const int expected = m_binding->sourceToEditor(document->buffer()->length());
    return expected >= 0 && m_editor->property("length").toInt() == expected;
}

// Waits until the editor holds the whole document and takes keys, and then
// for the first frame after that: the launch ends there.
void Benchmark::waitForEditor()
{
    const qint64 t = now();
    if (!m_window) {
        fail(tr("the window closed"));
        return;
    }
    if (!m_readyNs) {
        if (!editorReady()) {
            if (t - m_openedNs > qint64(m_timing.waitMs) * 1000000) fail(tr("the document never became editable"));
            return;
        }
        m_readyNs = t;
        m_document = m_binding->document();
        m_units = m_document->buffer()->length();
        // A window already showing asks for a frame of its own.
        m_window->update();
    }
    const qint64 frame = firstFrameAfter(m_readyNs);
    if (frame < 0) {
        if (t - m_readyNs > qint64(m_timing.waitMs) * 1000000)
            fail(tr("the window showed no frame; it must be on a visible workspace"));
        return;
    }
    m_poll.stop();
    m_out["label"] = m_label;
    m_out["utf16Units"] = m_units;
    m_out["qt"] = QString::fromLatin1(qVersion());
    m_out["platform"] = QGuiApplication::platformName();
    m_out["refreshHz"] = m_window->screen() ? m_window->screen()->refreshRate() : 0;
    m_out["devicePixelRatio"] = m_window->devicePixelRatio();
    m_out["launchToEditableMs"] = toMs(frame - processStartNs());
    if (m_mainNs) m_out["mainToEditableMs"] = toMs(frame - m_mainNs);
    m_out["loadSetTextMs"] = toMs(m_readyNs - m_openedNs);
    m_out["loadToFirstFrameMs"] = toMs(frame - m_openedNs);
    plan();
    next();
}

void Benchmark::then(int delayMs, std::function<void()> step) { m_steps.emplace_back(delayMs, std::move(step)); }

void Benchmark::next()
{
    if (m_done || m_next >= m_steps.size()) return;
    QTimer::singleShot(m_steps[m_next].first, Qt::PreciseTimer, this, [this] {
        if (m_done) return;
        if (!m_window || !m_pane || !m_editor || !m_flickable || !m_binding || !m_document) {
            fail(tr("the window or its document closed"));
            return;
        }
        const auto step = m_steps[m_next++].second;
        step();
        next();
    });
}

void Benchmark::fail(const QString &reason)
{
    if (m_done) return;
    m_done = true;
    m_poll.stop();
    m_watchdog.stop();
    stopDrivers();
    // Said from the event loop, even when start() fails at once: main()
    // leaves the loop on it, and a loop told to end before it runs forgets.
    QMetaObject::invokeMethod(this, [this, reason] { emit finished(1, reason); }, Qt::QueuedConnection);
}

void Benchmark::stopDrivers()
{
    for (QObject *driver : {m_scroller.data(), m_wrapper.data()})
        if (driver) driver->setProperty("running", false);
    delete m_scroller;
    delete m_wrapper;
}

void Benchmark::stepTaken() { m_stepTimes.push_back(now()); }

qint64 Benchmark::firstFrameAfter(qint64 t) const
{
    std::lock_guard<std::mutex> lock(m_frames->mutex);
    const auto &times = m_frames->times;
    const auto found = std::upper_bound(times.begin(), times.end(), t);
    return found == times.end() ? -1 : *found;
}

std::vector<double> Benchmark::intervals(qint64 from, qint64 to) const
{
    std::lock_guard<std::mutex> lock(m_frames->mutex);
    const auto &times = m_frames->times;
    std::vector<double> result;
    for (size_t i = 1; i < times.size(); ++i)
        if (times[i - 1] >= from && times[i] <= to) result.push_back(toMs(times[i] - times[i - 1]));
    return result;
}

int Benchmark::framesBetween(qint64 from, qint64 to) const
{
    std::lock_guard<std::mutex> lock(m_frames->mutex);
    const auto &times = m_frames->times;
    return int(std::count_if(times.begin(), times.end(), [&](qint64 t) { return t > from && t <= to; }));
}

int Benchmark::widthChangesBetween(qint64 from, qint64 to) const
{
    return int(std::count_if(m_widthChanges.begin(), m_widthChanges.end(), [&](qint64 t) { return t > from && t <= to; }));
}

QObject *Benchmark::make(const QByteArray &qml, const QVariantMap &properties)
{
    QQmlEngine *engine = qmlEngine(m_window);
    if (!engine) return nullptr;
    QQmlComponent component(engine);
    component.setData(qml, QUrl());
    QObject *made = component.createWithInitialProperties(properties, engine->rootContext());
    if (!made) {
        fail(component.errorString().trimmed());
        return nullptr;
    }
    made->setParent(this);
    return made;
}

void Benchmark::reveal(double fraction)
{
    const int position = int(fraction * m_document->buffer()->length());
    QMetaObject::invokeMethod(m_pane, "restoreView", Q_ARG(QVariant, position), Q_ARG(QVariant, position),
                              Q_ARG(QVariant, position));
    QMetaObject::invokeMethod(m_pane, "focusText");
    if (!m_window->isActive()) m_window->requestActivate();
}

void Benchmark::plan()
{
    const Timing &t = m_timing;
    const int settleDelay = m_pane->property("rewrapSettleMs").toInt();

    then(t.settleMs, [this] {
        m_out["pssLoadedMiB"] = pssMiB(QStringLiteral("self"));
        m_out["windowWidth"] = m_window->width();
        m_out["windowHeight"] = m_window->height();
        m_out["editorHeightPx"] = m_editor->height();
        m_out["paneWidthPx"] = m_pane->width();
        m_out["columnWidthPx"] = m_pane->property("columnWidth").toDouble();
        m_out["lineCount"] = m_editor->property("lineCount").toInt();
        m_out["graphicsApi"] = graphicsApiName(m_window);
        const QFont font = m_editor->property("font").value<QFont>();
        m_out["editorFont"] = QJsonObject{{"family", font.family()}, {"pixelSize", font.pixelSize()}};
        QObject *theme = qmlEngine(m_window)->rootContext()->contextProperty(QStringLiteral("theme")).value<QObject *>();
        m_out["themeDark"] = theme && theme->property("dark").toBool();
        m_out["ownedProcessesLoaded"] = ownedProcesses();
    });

    // Typing: 20 keys at each of three places, one key every 60 ms.
    struct Place {
        QString name;
        double fraction;
        std::vector<qint64> sent;
        std::vector<double> dispatch;
    };
    auto places = std::make_shared<std::vector<Place>>(
        std::vector<Place>{{QStringLiteral("start"), 0.0, {}, {}}, {QStringLiteral("middle"), 0.5, {}, {}},
                           {QStringLiteral("end"), 1.0, {}, {}}});
    for (size_t k = 0; k < places->size(); ++k) {
        then(t.placeMs, [this, places, k] { reveal((*places)[k].fraction); });
        for (int i = 0; i < Keys; ++i) {
            then(i == 0 ? t.placeMs : t.keyMs, [this, places, k] {
                QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                const qint64 sent = now();
                QCoreApplication::sendEvent(m_window, &press);
                (*places)[k].dispatch.push_back(toMs(now() - sent));
                (*places)[k].sent.push_back(sent);
                QCoreApplication::sendEvent(m_window, &release);
            });
        }
    }
    then(t.placeMs, [this, places] {
        QJsonObject typing;
        for (const Place &place : *places) {
            std::vector<double> latency;
            for (const qint64 sent : place.sent)
                if (const qint64 frame = firstFrameAfter(sent); frame >= 0) latency.push_back(toMs(frame - sent));
            typing[place.name] = QJsonObject{{"dispatchMs", stats(place.dispatch)}, {"keyToFrameMs", stats(latency)}};
        }
        m_out["typing"] = typing;
        const int typed = m_document->buffer()->length();
        m_out["lengthAfterTyping"] = typed;
        m_out["expectedLength"] = m_units + 3 * Keys;
        if (typed != m_units + 3 * Keys) {
            fail(tr("%1 of %2 keys reached the text; the window must stay in front while it is measured")
                     .arg(typed - m_units)
                     .arg(3 * Keys));
            return;
        }
        m_flickable->setProperty("contentY", 0);
    });

    // Scrolling, then far jumps, as in the whole-document measurement.
    auto scrollStart = std::make_shared<qint64>(0);
    then(t.placeMs, [this, scrollStart] {
        m_scroller = make(scrollerQml, {{QStringLiteral("target"), QVariant::fromValue(m_flickable.data())}});
        if (!m_scroller) return;
        *scrollStart = now();
        m_scroller->setProperty("running", true);
    });
    then(t.scrollMs, [this, scrollStart] {
        stopDrivers();
        m_out["scrollFrameMs"] = stats(intervals(*scrollStart, now()));
        m_out["scrolledToPx"] = m_flickable->property("contentY").toDouble();
    });
    auto jumped = std::make_shared<std::vector<qint64>>();
    for (int i = 0; i < Jumps; ++i) {
        then(t.jumpMs, [this, jumped, i] {
            const double span = m_flickable->property("contentHeight").toDouble() - m_flickable->height();
            jumped->push_back(now());
            m_flickable->setProperty("contentY", std::max(0.0, span * ((i * 37) % 100) / 100.0));
        });
    }
    then(300, [this, jumped] {
        std::vector<double> latency;
        for (const qint64 sent : *jumped)
            if (const qint64 frame = firstFrameAfter(sent); frame >= 0) latency.push_back(toMs(frame - sent));
        m_out["jumpToFrameMs"] = stats(latency);
        // The pane loses only the room beside a full column, so that the
        // column itself narrows from the first step.
        const double spare = m_pane->width() - 2 * m_pane->property("margin").toDouble()
                             - m_pane->property("columnWidth").toDouble();
        m_rewrapBase = std::max(0.0, std::floor(spare));
        m_pane->property("anchors").value<QObject *>()->setProperty("rightMargin", m_rewrapBase);
    });

    // Re-wrap: the column narrows and widens 4 pixels a frame. A document
    // longer than the pane's limit waits until the width is still.
    auto wrapStart = std::make_shared<qint64>(0);
    auto wrapStop = std::make_shared<qint64>(0);
    then(300 + settleDelay, [this, wrapStart] {
        m_stepTimes.clear();
        m_wrapper = make(wrapperQml, {{QStringLiteral("target"), QVariant::fromValue(m_pane->property("anchors").value<QObject *>())},
                                      {QStringLiteral("base"), m_rewrapBase}});
        if (!m_wrapper) return;
        connect(m_wrapper, SIGNAL(stepped()), this, SLOT(stepTaken()));
        *wrapStart = now();
        m_wrapper->setProperty("running", true);
    });
    then(t.rewrapMs, [this, wrapStart, wrapStop] {
        stopDrivers();
        *wrapStop = now();
        m_out["rewrapFrameMs"] = stats(intervals(*wrapStart, *wrapStop));
    });
    then(settleDelay + 1000, [this, wrapStart, wrapStop, settleDelay] {
        const qint64 lastStep = m_stepTimes.empty() ? *wrapStop : m_stepTimes.back();
        QJsonObject settle{{"deferred", m_units > m_pane->property("largeDocumentUnits").toInt()},
                           {"widthSteps", int(m_stepTimes.size())},
                           {"layoutsWhileMoving", widthChangesBetween(*wrapStart, lastStep)},
                           {"layoutsAfterStop", widthChangesBetween(lastStep, now())},
                           {"settleDelayMs", settleDelay}};
        // The one layout after the width stopped, and when its frame showed.
        const auto last = std::find_if(m_widthChanges.rbegin(), m_widthChanges.rend(), [&](qint64 c) { return c > lastStep; });
        if (last != m_widthChanges.rend()) {
            const qint64 frame = firstFrameAfter(*last);
            if (frame >= 0) {
                settle["lastStepToFrameMs"] = toMs(frame - lastStep);
                settle["settleMs"] = toMs(frame - lastStep) - settleDelay;
            }
        }
        m_out["rewrapSettle"] = settle;
        m_out["pssEndMiB"] = pssMiB(QStringLiteral("self"));
        m_out["ownedProcessesEnd"] = ownedProcesses();
        m_pane->property("anchors").value<QObject *>()->setProperty("rightMargin", 0);
        // The idle window has the caret in view, where typing left it.
        reveal(1.0);
    });

    // Idle: the window as it is left, after saving and the session have
    // landed. The caret blinks until the writer has paused for ten seconds,
    // and each time it shows or hides is a frame; after that nothing changes,
    // and no frame is drawn.
    auto idle = std::make_shared<std::array<qint64, 3>>();
    auto blinks = std::make_shared<int>(0);
    auto caretWatch = std::make_shared<QMetaObject::Connection>();
    then(t.quietMs, [this, idle, blinks, caretWatch] {
        if (auto *caret = qobject_cast<QQuickItem *>(findNamed(m_editor, QStringLiteral("caret"))))
            *caretWatch = connect(caret, &QQuickItem::visibleChanged, this, [blinks] { ++*blinks; });
        *idle = {now(), cpuTicks(), childCpuTicks()};
    });
    then(t.idleMs, [this, idle, blinks, caretWatch] {
        const qint64 end = now();
        disconnect(*caretWatch);
        const int frames = framesBetween((*idle)[0], end);
        m_out["idleMs"] = toMs(end - (*idle)[0]);
        m_out["idleFrames"] = frames;
        m_out["idleCaretChanges"] = *blinks;
        m_out["idleFramesBesidesCaret"] = std::max(0, frames - *blinks);
        m_out["cursorFlashTimeMs"] = QGuiApplication::styleHints()->cursorFlashTime();
        m_out["idleCpuTicks"] = cpuTicks() - (*idle)[1];
        m_out["idleChildCpuTicks"] = childCpuTicks() - (*idle)[2];
        m_out["clockTicksPerSecond"] = int(sysconf(_SC_CLK_TCK));
        m_out["ownedProcessesIdle"] = ownedProcesses();
    });

    // The control: the same scroll and re-wrap over plain stripes in the
    // editor's place, measured in the same window.
    auto controlScroll = std::make_shared<qint64>(0);
    then(1000 + settleDelay, [this] {
        const double textHeight = m_flickable->property("contentHeight").toDouble();
        QObject *made = make(controlQml, {{QStringLiteral("pane"), QVariant::fromValue(m_pane.data())},
                                          {QStringLiteral("textHeight"), textHeight},
                                          {QStringLiteral("stripeWidth"), m_pane->property("columnWidth").toDouble()}});
        m_control = qobject_cast<QQuickItem *>(made);
        if (!m_control) {
            delete made;
            if (!m_done) fail(tr("the control scene did not load"));
            return;
        }
        m_control->setParentItem(m_pane->parentItem());
        m_pane->setVisible(false);
    });
    then(500, [this, controlScroll] {
        auto *flick = m_control ? m_control->findChild<QQuickItem *>(QStringLiteral("controlFlickable")) : nullptr;
        if (!flick) {
            fail(tr("the control scene did not load"));
            return;
        }
        m_scroller = make(scrollerQml, {{QStringLiteral("target"), QVariant::fromValue(flick)}});
        if (!m_scroller) return;
        *controlScroll = now();
        m_scroller->setProperty("running", true);
    });
    auto control = std::make_shared<QJsonObject>();
    then(t.scrollMs, [this, controlScroll, control] {
        stopDrivers();
        (*control)["scrollFrameMs"] = stats(intervals(*controlScroll, now()));
    });
    auto controlWrap = std::make_shared<qint64>(0);
    then(300, [this, controlWrap] {
        if (!m_control) return fail(tr("the control scene closed"));
        m_wrapper = make(wrapperQml, {{QStringLiteral("target"), QVariant::fromValue<QObject *>(m_control.data())},
                                      {QStringLiteral("base"), m_rewrapBase}});
        if (!m_wrapper) return;
        *controlWrap = now();
        m_wrapper->setProperty("running", true);
    });
    then(t.rewrapMs, [this, controlWrap, control] {
        stopDrivers();
        (*control)["rewrapFrameMs"] = stats(intervals(*controlWrap, now()));
        m_out["control"] = *control;
        delete m_control;
        m_pane->setVisible(true);
        QMetaObject::invokeMethod(m_pane, "focusText");
    });

    then(0, [this] {
        const QJsonObject self = usage(RUSAGE_SELF);
        m_out["peakRssMiB"] = self["peakRssMiB"];
        m_out["cpuTimeMs"] = self["cpuTimeMs"];
        // Processes parch started and has waited for; it starts none of its own.
        m_out["helperProcesses"] = usage(RUSAGE_CHILDREN);
        m_out["pssEndWithOwnedMiB"] =
            m_out["pssEndMiB"].toDouble() + m_out["ownedProcessesEnd"].toObject()["pssMiB"].toDouble();
        QSaveFile file(m_output);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(m_out).toJson()) < 0 || !file.commit()) {
            fail(tr("cannot write %1: %2").arg(m_output, file.errorString()));
            return;
        }
        m_done = true;
        m_watchdog.stop();
        emit finished(0, {});
    });
}
