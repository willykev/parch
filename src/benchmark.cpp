#include "benchmark.h"

#include "backend.h"
#include "dialectchoice.h"
#include "document.h"
#include "documentreading.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "markdownstyling.h"
#include "mdinclude.h"
#include "mdtree.h"
#include "preview.h"
#include "recovery.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "spellchecker.h"
#include "spellinglayer.h"
#include "tabmodel.h"
#include "windowcontext.h"
#include "wordcounter.h"

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
#include <QUuid>
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
// Keys typed faster than any view settles, and how deep the nesting goes.
constexpr int RapidKeys = 200;
constexpr int NestingDepth = 200;

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

// CPU time of every thread of this process, in milliseconds, finer than ticks.
double cpuMs()
{
    rusage used{};
    getrusage(RUSAGE_SELF, &used);
    const auto ms = [](const timeval &t) { return double(t.tv_sec) * 1000.0 + double(t.tv_usec) / 1000.0; };
    return ms(used.ru_utime) + ms(used.ru_stime);
}

// How many times the threads of this process were switched off a CPU,
// those that ended included.
qint64 contextSwitches()
{
    rusage used{};
    getrusage(RUSAGE_SELF, &used);
    return qint64(used.ru_nvcsw) + qint64(used.ru_nivcsw);
}

// The CPU time of each thread of this process so far, in nanoseconds, by
// thread id, from /proc/self/task/<tid>/schedstat (its first field).
QHash<QString, qint64> threadCpuNs()
{
    QHash<QString, qint64> times;
    const QString tasks = QStringLiteral("/proc/self/task");
    for (const QString &task : QDir(tasks).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(tasks + u'/' + task + QStringLiteral("/schedstat"));
        if (file.open(QIODevice::ReadOnly)) times.insert(task, file.readAll().trimmed().split(' ').value(0).toLongLong());
    }
    return times;
}

// The CPU time the UI thread and the other threads used since `before`, in
// milliseconds; threads started since count whole.
std::pair<double, double> threadCpuSince(const QHash<QString, qint64> &before)
{
    const QString ui = QString::number(getpid());
    const QHash<QString, qint64> after = threadCpuNs();
    qint64 main = 0, others = 0;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        (it.key() == ui ? main : others) += std::max<qint64>(0, it.value() - before.value(it.key(), 0));
    return {toMs(main), toMs(others)};
}

// How many times each thread of a process was put on a CPU, by
// "pid/tid", from /proc/<pid>/task/<tid>/schedstat (its third field): every
// wakeup is one.
void timeslices(const QString &pid, QHash<QString, qint64> &slices)
{
    const QString tasks = QStringLiteral("/proc/%1/task").arg(pid);
    for (const QString &task : QDir(tasks).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(tasks + u'/' + task + QStringLiteral("/schedstat"));
        if (!file.open(QIODevice::ReadOnly)) continue;
        slices.insert(pid + u'/' + task, file.readAll().trimmed().split(' ').value(2).toLongLong());
    }
}

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

// Every process this one started, and theirs, that is still running.
QList<qint64> ownedPids()
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
    return owned;
}

// The processes parch started, with what they hold: its own helpers count
// in its memory.
QJsonObject ownedProcesses()
{
    const QList<qint64> owned = ownedPids();
    double pss = 0;
    for (const qint64 pid : owned) pss += std::max(0.0, pssMiB(QString::number(pid)));
    return {{"count", int(owned.size())}, {"pssMiB", pss}};
}

// The CPU ticks the processes parch started and that still run have used, by pid.
QHash<qint64, qint64> ownedTicks()
{
    QHash<qint64, qint64> ticks;
    for (const qint64 pid : ownedPids()) {
        const auto fields = statFields(QString::number(pid));
        ticks.insert(pid, statField(fields, 14) + statField(fields, 15));
    }
    return ticks;
}

// The times every thread of this process and of those it started was put
// on a CPU, by "pid/tid".
QHash<QString, qint64> wakeups()
{
    QHash<QString, qint64> slices;
    timeslices(QString::number(getpid()), slices);
    for (const qint64 pid : ownedPids()) timeslices(QString::number(pid), slices);
    return slices;
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

// An item, or an object an item holds, such as a QML type that draws
// nothing, by its name.
QObject *findObjectNamed(QQuickItem *root, const QString &name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (QObject *child : root->children())
        if (child->objectName() == name) return child;
    for (QQuickItem *child : root->childItems())
        if (QObject *found = findObjectNamed(child, name)) return found;
    return nullptr;
}

// Text as a writer pastes it: paragraphs of prose with the usual Markdown
// in them, to `units` UTF-16 units.
QString pastedText(int units)
{
    static const QString paragraph = QStringLiteral(
        "The quiet writer keeps **each ordinary word** and reads again, with *emphasis*, a "
        "[link](https://example.org/notes), `a code span` and a note[^pasted]. The quiet writer keeps each "
        "ordinary word and reads again, and the paragraph runs on as a pasted chapter would.\n\n");
    QString text = QStringLiteral("\n\n## A pasted chapter\n\n[^pasted]: A note of the pasted chapter.\n\n");
    while (text.size() < units) text += paragraph;
    return text.left(std::max(qsizetype(units), qsizetype(0)));
}

// Constructs inside one another NestingDepth deep: quotes, lists,
// emphasis and brackets.
QString nestedText()
{
    QString text = QStringLiteral("\n\n");
    text += QStringLiteral("> ").repeated(NestingDepth) + QStringLiteral("A quote two hundred deep.\n\n");
    for (int i = 0; i < NestingDepth; ++i) text += QString(2 * i, u' ') + QStringLiteral("- a list item\n");
    text += u'\n';
    for (int i = 0; i < NestingDepth; ++i) text += i % 2 ? u'_' : u'*';
    text += QStringLiteral("nested emphasis");
    for (int i = NestingDepth - 1; i >= 0; --i) text += i % 2 ? u'_' : u'*';
    text += QStringLiteral("\n\n") + QString(NestingDepth, u'[') + QStringLiteral("brackets")
            + QString(NestingDepth, u']') + QStringLiteral("\n\n");
    return text;
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

// What follows the text in one window, off the typing path.
struct Benchmark::Views {
    QPointer<MarkdownStyling> styling;
    QPointer<SpellingLayer> spelling;
    QPointer<Preview> preview;
    QPointer<WordCounter> counter;
    QPointer<QQuickItem> previewFlickable;
    QPointer<QQuickItem> editor;
    QPointer<EditorBinding> binding;
};

Benchmark::Benchmark(const QString &output, const QString &label, const Timing &timing, QObject *parent)
    : QObject(parent), m_output(output), m_label(label), m_timing(timing), m_frames(std::make_shared<Frames>()),
      m_otherFrames(std::make_shared<Frames>()), m_views(std::make_unique<Views>()),
      m_secondViews(std::make_unique<Views>())
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
    // What it reads beside it comes along, where it is beside it.
    const QDir from(source.absolutePath());
    for (const QString &read : filesRead(source.absoluteFilePath())) {
        const QString relative = from.relativeFilePath(read);
        if (relative.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(relative) || !QFileInfo(read).isFile()) continue;
        const QString target = folder + u'/' + relative;
        if (QFileInfo::exists(target)) continue;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(read, target)) {
            *error = tr("cannot copy %1 to measure %2").arg(read, file);
            return {};
        }
    }
    return copy;
}

QStringList Benchmark::filesRead(const QString &file)
{
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) return {};
    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = QFileInfo(file).canonicalFilePath();
    source.resolver = &resolver;
    const DocumentReading::Reading reading = DocumentReading::read(QString::fromUtf8(in.readAll()), {}, source);
    QStringList files;
    for (size_t i = 1; i < reading.document.files.size(); ++i) files.append(reading.document.files[i].path);
    files.append(reading.dialect.bibliography);
    if (Dialect::isStyleFile(reading.dialect.citationStyle)) files.append(reading.dialect.citationStyle);
    // The pictures of every file, found from that file's own folder.
    const std::function<void(const md::Node &)> walk = [&](const md::Node &node) {
        QString written;
        if (node.type == md::NodeType::Image) written = node.dest;
        else if (node.type == md::NodeType::HtmlElement && node.label == QLatin1StringView("img"))
            for (const md::Attribute &attribute : node.attributes)
                if (attribute.name == QLatin1StringView("src")) written = attribute.value;
        if (!written.isEmpty() && node.file >= 0 && size_t(node.file) < reading.document.files.size()) {
            const md::Destination target =
                md::classifyDestination(written, reading.document.files[size_t(node.file)].directory);
            if (target.kind == md::Destination::Kind::LocalFile && target.absolute) files.append(target.path);
        }
        for (const md::Node &child : node.children) walk(child);
    };
    walk(reading.document.root);
    for (QString &read : files) read = QDir::cleanPath(read);
    files.removeDuplicates();
    return files;
}

void Benchmark::setBackend(Backend *backend) { m_backend = backend; }

void Benchmark::setLaunchTime(qint64 mainNs) { m_mainNs = mainNs; }

void Benchmark::opening()
{
    m_openedNs = now();
    m_out["pssEmptyMiB"] = pssMiB(QStringLiteral("self"));
}

WindowContext *Benchmark::contextOf(QQuickWindow *window) const
{
    return window ? qobject_cast<WindowContext *>(window->property("context").value<QObject *>()) : nullptr;
}

// Shows the document in front as the options ask: as source, beside its
// preview, or as the preview alone.
void Benchmark::showView(QQuickWindow *window)
{
    WindowContext *context = contextOf(window);
    if (!context) return;
    const TabModel::ViewMode mode = m_options.view == QLatin1String("split")     ? TabModel::Split
                                    : m_options.view == QLatin1String("reading") ? TabModel::Reading
                                                                                  : TabModel::Source;
    context->tabs()->setActiveMode(mode);
}

void Benchmark::findViews(QQuickWindow *window, Views &views) const
{
    QQuickItem *root = window ? window->contentItem() : nullptr;
    views.styling = qobject_cast<MarkdownStyling *>(findObjectNamed(root, QStringLiteral("styling")));
    views.spelling = qobject_cast<SpellingLayer *>(findObjectNamed(root, QStringLiteral("spelling")));
    views.preview = qobject_cast<Preview *>(findObjectNamed(root, QStringLiteral("preview")));
    views.counter = qobject_cast<WordCounter *>(findObjectNamed(root, QStringLiteral("wordCounter")));
    views.previewFlickable = qobject_cast<QQuickItem *>(findObjectNamed(root, QStringLiteral("previewFlickable")));
    views.editor = qobject_cast<QQuickItem *>(findObjectNamed(root, QStringLiteral("editor")));
    views.binding = qobject_cast<EditorBinding *>(findObjectNamed(root, QStringLiteral("binding")));
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
    // Live formatting and spelling are on, as an ordinary session has them
    // (spelling where a dictionary is installed).
    if (m_backend) {
        m_backend->settings()->setLiveFormatting(true);
        if (m_backend->settings()->spellcheck() == std::optional<bool>(false)) m_backend->setSpellchecking(true);
    }
    showView(window);
    findViews(window, *m_views);
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
    const qint64 adversarial = m_options.adversarial ? 4 * qint64(t.catchUpMs) + RapidKeys * qint64(t.rapidKeyMs) + 6 * t.placeMs : 0;
    const qint64 total = qint64(t.waitMs) * 4 + t.settleMs + 3 * (2 * t.placeMs + Keys * t.keyMs + t.catchUpMs)
                         + t.placeMs + 2 * (t.scrollMs + t.rewrapMs) + Jumps * t.jumpMs + t.catchUpMs + t.quietMs + t.idleMs
                         + adversarial + 10000;
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
    m_out["view"] = m_options.view;
    m_out["windows"] = m_options.windows;
    m_out["adversarial"] = m_options.adversarial;
    plan();
    next();
}

void Benchmark::then(int delayMs, std::function<void()> step) { m_steps.emplace_back(delayMs, std::move(step)); }

void Benchmark::thenWaitFor(std::function<bool()> ready, int capMs,
                            std::function<void(qint64 waitedNs, bool gaveUp)> done)
{
    then(0, [this, ready = std::move(ready), capMs, done = std::move(done)] {
        m_holding = true;
        const qint64 began = now();
        auto *poll = new QTimer(this);
        poll->setTimerType(Qt::PreciseTimer);
        poll->setInterval(5);
        connect(poll, &QTimer::timeout, this, [this, poll, ready, capMs, done, began] {
            if (m_done) {
                poll->deleteLater();
                return;
            }
            const bool ok = ready();
            const qint64 waited = now() - began;
            if (!ok && waited < qint64(capMs) * 1000000) return;
            poll->stop();
            poll->deleteLater();
            done(waited, !ok);
            m_holding = false;
            next();
        });
        poll->start();
    });
}

void Benchmark::next()
{
    if (m_done || m_holding || m_next >= m_steps.size()) return;
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

int Benchmark::otherFramesBetween(qint64 from, qint64 to) const
{
    std::lock_guard<std::mutex> lock(m_otherFrames->mutex);
    const auto &times = m_otherFrames->times;
    return int(std::count_if(times.begin(), times.end(), [&](qint64 t) { return t > from && t <= to; }));
}

int Benchmark::widthChangesBetween(qint64 from, qint64 to) const
{
    return int(std::count_if(m_widthChanges.begin(), m_widthChanges.end(), [&](qint64 c) { return c > from && c <= to; }));
}

double Benchmark::refreshIntervalMs() const
{
    const double hz = m_window && m_window->screen() ? m_window->screen()->refreshRate() : 0;
    return hz > 0 ? 1000.0 / hz : 1000.0 / 60.0;
}

int Benchmark::droppedFrames(const std::vector<double> &intervals) const
{
    const double refresh = refreshIntervalMs();
    int missed = 0;
    for (const double interval : intervals) missed += std::max(0, int(std::lround(interval / refresh)) - 1);
    return missed;
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

void Benchmark::typeKey(QChar key, std::vector<double> *dispatch, std::vector<qint64> *sent)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QString(key));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_X, Qt::NoModifier, QString(key));
    const qint64 at = now();
    QCoreApplication::sendEvent(m_window, &press);
    if (dispatch) dispatch->push_back(toMs(now() - at));
    if (sent) sent->push_back(at);
    QCoreApplication::sendEvent(m_window, &release);
}

bool Benchmark::caughtUp() const
{
    const auto caught = [](const Views &views) {
        if (views.styling && views.styling->busy()) return false;
        if (views.spelling && views.spelling->checking()) return false;
        if (views.preview && views.preview->active() && (!views.preview->current() || views.preview->working()))
            return false;
        if (views.counter && views.counter->counting()) return false;
        return true;
    };
    return caught(*m_views) && (!m_second || caught(*m_secondViews));
}

bool Benchmark::caretsResting() const
{
    for (QQuickWindow *window : {m_window.data(), m_second.data()}) {
        QObject *pane = window ? findNamed(window->contentItem(), QStringLiteral("editorPane")) : nullptr;
        QObject *blink = pane ? pane->findChild<QObject *>(QStringLiteral("caretBlink")) : nullptr;
        if (blink && blink->property("running").toBool()) return false;
    }
    return true;
}

bool Benchmark::caughtUpSince(qint64 since) const
{
    return now() - since >= qint64(m_timing.caughtUpMs) * 1000000 && caughtUp();
}

// A second window with the same document in front, shown as the first
// shows it: a view of its own, with its own layout and preview.
void Benchmark::openSecondWindow()
{
    if (!m_backend) return fail(tr("two windows need the application"));
    m_backend->newWindow();
    WindowContext *context = m_backend->windows().last();
    if (!context || context == contextOf(m_window)) return fail(tr("no second window opened"));
    // In place of the new window's blank draft.
    context->tabs()->openInPlaceOf(0, m_document);
    for (QWindow *window : QGuiApplication::topLevelWindows()) {
        auto *quick = qobject_cast<QQuickWindow *>(window);
        if (quick && contextOf(quick) == context) m_second = quick;
    }
    if (!m_second) return fail(tr("the second window has no view"));
    showView(m_second);
    findViews(m_second, *m_secondViews);
    std::shared_ptr<Frames> frames = m_otherFrames;
    connect(m_second, &QQuickWindow::frameSwapped, this, [frames] {
        const qint64 t = now();
        std::lock_guard<std::mutex> lock(frames->mutex);
        frames->times.push_back(t);
    }, Qt::DirectConnection);
}

bool Benchmark::secondReady() const
{
    const Views &views = *m_secondViews;
    if (!m_second || !views.binding || views.binding->document() != m_document) return false;
    if (m_options.view == QLatin1String("reading"))
        return views.preview && views.preview->current() && !views.preview->working();
    const int expected = views.binding->sourceToEditor(m_document->buffer()->length());
    return views.editor && expected >= 0 && views.editor->property("length").toInt() == expected;
}

void Benchmark::plan()
{
    const Timing &t = m_timing;
    if (m_options.windows > 1) {
        auto opened = std::make_shared<qint64>(0);
        then(0, [this, opened] {
            *opened = now();
            openSecondWindow();
        });
        thenWaitFor([this] { return secondReady(); }, t.waitMs, [this, opened](qint64 waited, bool gaveUp) {
            if (gaveUp) return fail(tr("the second window never showed the document"));
            m_out["secondWindow"] = QJsonObject{{"loadMs", toMs(waited)}};
            Q_UNUSED(opened);
        });
    }

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
        // What is shown, and which views of the text work beside typing.
        const auto shown = [this](const char *name) {
            auto *item = qobject_cast<QQuickItem *>(findNamed(m_window->contentItem(), QLatin1String(name)));
            return item && item->isVisible() && item->width() > 0;
        };
        m_out["panes"] = QJsonObject{{"editor", shown("editorPane")}, {"preview", shown("previewPane")}};
        m_out["liveFormatting"] = m_views->styling && m_views->styling->enabled();
        m_out["spellingActive"] = m_backend && m_backend->spelling() && m_backend->spelling()->active();
        m_out["previewActive"] = m_views->preview && m_views->preview->active();
    });

    const bool reading = m_options.view == QLatin1String("reading");
    if (!reading) planTyping();
    planScrolling();
    if (m_options.adversarial && !reading) planAdversarial();
    planIdle();
    if (!reading) planControl();
    planRecovery();

    then(0, [this] {
        const QJsonObject self = usage(RUSAGE_SELF);
        m_out["peakRssMiB"] = self["peakRssMiB"];
        m_out["cpuTimeMs"] = self["cpuTimeMs"];
        // Processes parch started and has waited for: the helpers that draw
        // formulas and diagrams.
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

// Typing: 20 keys at each of three places, one key every 60 ms, and then a
// pause until every view of the text has caught up. The CPU time of the
// whole process from the first key until then, its workers' too, is the
// cost of those keys.
void Benchmark::planTyping()
{
    const Timing &t = m_timing;
    struct Place {
        QString name;
        double fraction;
        std::vector<qint64> sent;
        std::vector<double> dispatch;
        double cpuBefore = 0;
        double cpuMs = 0;
        QHash<QString, qint64> threadsBefore;
        std::pair<double, double> threadMs;
        quint64 parsesBefore = 0;
        quint64 parses = 0;
        int previewUpdatesBefore = 0;
        int previewStaleBefore = 0;
        int previewUpdates = 0;
        int previewStale = 0;
        double caughtUpMs = 0;
        bool gaveUp = false;
    };
    auto places = std::make_shared<std::vector<Place>>(
        std::vector<Place>{{QStringLiteral("start"), 0.0}, {QStringLiteral("middle"), 0.5}, {QStringLiteral("end"), 1.0}});
    for (size_t k = 0; k < places->size(); ++k) {
        then(t.placeMs, [this, places, k] { reveal((*places)[k].fraction); });
        for (int i = 0; i < Keys; ++i) {
            then(i == 0 ? t.placeMs : t.keyMs, [this, places, k, i] {
                Place &place = (*places)[k];
                if (i == 0) {
                    place.cpuBefore = cpuMs();
                    place.threadsBefore = threadCpuNs();
                    place.parsesBefore = md::parseCount();
                    if (m_views->preview) {
                        place.previewUpdatesBefore = m_views->preview->updates();
                        place.previewStaleBefore = m_views->preview->staleResults();
                    }
                }
                typeKey(u'x', &place.dispatch, &place.sent);
                ++m_typed;
            });
        }
        thenWaitFor([this, places, k] { return caughtUpSince((*places)[k].sent.back()); }, t.catchUpMs,
                    [this, places, k](qint64, bool gaveUp) {
                        Place &place = (*places)[k];
                        place.cpuMs = cpuMs() - place.cpuBefore;
                        place.threadMs = threadCpuSince(place.threadsBefore);
                        place.parses = md::parseCount() - place.parsesBefore;
                        if (m_views->preview) {
                            place.previewUpdates = m_views->preview->updates() - place.previewUpdatesBefore;
                            place.previewStale = m_views->preview->staleResults() - place.previewStaleBefore;
                        }
                        place.caughtUpMs = toMs(now() - place.sent.back());
                        place.gaveUp = gaveUp;
                    });
    }
    then(t.placeMs, [this, places] {
        QJsonObject typing;
        double cpu = 0, uiCpu = 0, otherCpu = 0;
        quint64 parses = 0;
        int late = 0, keys = 0;
        const double refresh = refreshIntervalMs();
        for (const Place &place : *places) {
            std::vector<double> latency;
            for (const qint64 sent : place.sent)
                if (const qint64 frame = firstFrameAfter(sent); frame >= 0) latency.push_back(toMs(frame - sent));
            const int placeLate = int(std::count_if(latency.begin(), latency.end(), [refresh](double ms) { return ms > refresh; }));
            const double count = double(place.sent.size());
            QJsonObject measured{{"dispatchMs", stats(place.dispatch)},
                                 {"keyToFrameMs", stats(latency)},
                                 {"cpuMsPerKey", place.cpuMs / count},
                                 // The UI thread's share, and that of every other thread: workers and rendering.
                                 {"uiThreadCpuMsPerKey", place.threadMs.first / count},
                                 {"otherThreadsCpuMsPerKey", place.threadMs.second / count},
                                 {"parses", double(place.parses)},
                                 {"caughtUpMs", place.caughtUpMs},
                                 {"caughtUp", !place.gaveUp},
                                 {"lateFrames", placeLate}};
            if (m_views->preview && m_views->preview->active()) {
                measured["previewUpdates"] = place.previewUpdates;
                measured["previewStale"] = place.previewStale;
            }
            typing[place.name] = measured;
            cpu += place.cpuMs;
            uiCpu += place.threadMs.first;
            otherCpu += place.threadMs.second;
            parses += place.parses;
            late += placeLate;
            keys += int(place.sent.size());
        }
        m_out["typing"] = typing;
        m_out["typingCpuMsPerKey"] = keys ? cpu / keys : 0.0;
        m_out["typingUiThreadCpuMsPerKey"] = keys ? uiCpu / keys : 0.0;
        m_out["typingOtherThreadsCpuMsPerKey"] = keys ? otherCpu / keys : 0.0;
        m_out["typingParsesPerKey"] = keys ? double(parses) / keys : 0.0;
        QJsonObject dropped = m_out["droppedFrames"].toObject();
        dropped["refreshIntervalMs"] = refresh;
        dropped["typing"] = late;
        m_out["droppedFrames"] = dropped;
        const int typed = m_document->buffer()->length();
        m_out["lengthAfterTyping"] = typed;
        m_out["expectedLength"] = m_units + m_typed;
        if (typed != m_units + m_typed) {
            fail(tr("%1 of %2 keys reached the text; the window must stay in front while it is measured")
                     .arg(typed - m_units)
                     .arg(m_typed));
            return;
        }
    });
}

// Scrolling, then far jumps, as in the whole-document measurement: the
// text, or the preview when it is shown alone. Beside its preview the text
// leads, and the preview follows it.
void Benchmark::planScrolling()
{
    const Timing &t = m_timing;
    const bool reading = m_options.view == QLatin1String("reading");
    then(t.placeMs, [this, reading] {
        m_scrolled = reading ? m_views->previewFlickable.data() : m_flickable.data();
        if (!m_scrolled) return fail(tr("the window has no %1 to scroll").arg(reading ? tr("preview") : tr("text")));
        m_out["scrolled"] = reading ? QStringLiteral("preview") : QStringLiteral("text");
        m_scrolled->setProperty("contentY", 0);
    });
    auto scrollStart = std::make_shared<qint64>(0);
    then(t.placeMs, [this, scrollStart] {
        m_scroller = make(scrollerQml, {{QStringLiteral("target"), QVariant::fromValue(m_scrolled.data())}});
        if (!m_scroller) return;
        *scrollStart = now();
        m_scroller->setProperty("running", true);
    });
    then(t.scrollMs, [this, scrollStart] {
        stopDrivers();
        const std::vector<double> frames = intervals(*scrollStart, now());
        m_out["scrollFrameMs"] = stats(frames);
        m_out["scrolledToPx"] = m_scrolled ? m_scrolled->property("contentY").toDouble() : 0.0;
        QJsonObject dropped = m_out["droppedFrames"].toObject();
        dropped["refreshIntervalMs"] = refreshIntervalMs();
        dropped["scroll"] = droppedFrames(frames);
        m_out["droppedFrames"] = dropped;
    });
    auto jumped = std::make_shared<std::vector<qint64>>();
    for (int i = 0; i < Jumps; ++i) {
        then(t.jumpMs, [this, jumped, i] {
            if (!m_scrolled) return;
            const double span = m_scrolled->property("contentHeight").toDouble() - m_scrolled->height();
            jumped->push_back(now());
            m_scrolled->setProperty("contentY", std::max(0.0, span * ((i * 37) % 100) / 100.0));
        });
    }
    then(300, [this, jumped] {
        std::vector<double> latency;
        for (const qint64 sent : *jumped)
            if (const qint64 frame = firstFrameAfter(sent); frame >= 0) latency.push_back(toMs(frame - sent));
        m_out["jumpToFrameMs"] = stats(latency);
    });
    if (reading) return;

    // Re-wrap: the column narrows and widens 4 pixels a frame. A document
    // longer than the pane's limit waits until the width is still. The pane
    // loses only the room beside a full column, so that the column itself
    // narrows from the first step.
    const int settleDelay = m_pane->property("rewrapSettleMs").toInt();
    then(0, [this] {
        const double spare = m_pane->width() - 2 * m_pane->property("margin").toDouble()
                             - m_pane->property("columnWidth").toDouble();
        m_rewrapBase = std::max(0.0, std::floor(spare));
        m_pane->property("anchors").value<QObject *>()->setProperty("rightMargin", m_rewrapBase);
    });
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
        m_pane->property("anchors").value<QObject *>()->setProperty("rightMargin", 0);
    });
}

// Edits no writer makes, each until every view has caught up: a megabyte
// put in at once, as a paste does, and its undo; constructs nested two
// hundred deep; and keys faster than any view settles.
void Benchmark::planAdversarial()
{
    const Timing &t = m_timing;
    struct Edit {
        qint64 sent = 0;
        double dispatchMs = 0;
        double cpuBefore = 0;
        quint64 parsesBefore = 0;
    };
    auto adversarial = std::make_shared<QJsonObject>();
    auto edit = std::make_shared<Edit>();
    const auto begin = [this, edit](const std::function<void()> &act) {
        edit->cpuBefore = cpuMs();
        edit->parsesBefore = md::parseCount();
        edit->sent = now();
        act();
        edit->dispatchMs = toMs(now() - edit->sent);
    };
    const auto measured = [this, edit](qint64, bool gaveUp) {
        const qint64 frame = firstFrameAfter(edit->sent);
        return QJsonObject{{"dispatchMs", edit->dispatchMs},
                           {"toFrameMs", frame >= 0 ? toMs(frame - edit->sent) : -1.0},
                           {"caughtUpMs", toMs(now() - edit->sent)},
                           {"caughtUp", !gaveUp},
                           {"cpuMs", cpuMs() - edit->cpuBefore},
                           {"parses", double(md::parseCount() - edit->parsesBefore)}};
    };
    auto before = std::make_shared<int>(0);

    // The paste, at the end of the text, as one edit.
    then(t.placeMs, [this] { reveal(1.0); });
    then(t.placeMs, [this, begin, before] {
        *before = m_document->buffer()->length();
        const QString text = pastedText(m_timing.pasteUnits);
        begin([this, &text] {
            const int at = m_editor->property("cursorPosition").toInt();
            m_binding->replaceEditorRange(at, at, text);
        });
    });
    thenWaitFor([this, edit] { return caughtUpSince(edit->sent); }, t.catchUpMs,
                [this, adversarial, measured, before](qint64 waited, bool gaveUp) {
                    QJsonObject paste = measured(waited, gaveUp);
                    paste["units"] = m_document->buffer()->length() - *before;
                    (*adversarial)["paste"] = paste;
                });
    // Its undo, by the key.
    then(t.placeMs, [this, begin] {
        begin([this] {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Z, Qt::ControlModifier);
            QCoreApplication::sendEvent(m_window, &press);
            QCoreApplication::sendEvent(m_window, &release);
        });
    });
    thenWaitFor([this, edit] { return caughtUpSince(edit->sent); }, t.catchUpMs,
                [this, adversarial, measured, before](qint64 waited, bool gaveUp) {
                    QJsonObject undo = measured(waited, gaveUp);
                    undo["undone"] = m_document->buffer()->length() == *before;
                    (*adversarial)["undo"] = undo;
                });
    // Nesting two hundred deep, put in at the end.
    then(t.placeMs, [this] { reveal(1.0); });
    then(t.placeMs, [this, begin] {
        const QString text = nestedText();
        begin([this, &text] {
            const int at = m_editor->property("cursorPosition").toInt();
            m_binding->replaceEditorRange(at, at, text);
        });
    });
    thenWaitFor([this, edit] { return caughtUpSince(edit->sent); }, t.catchUpMs,
                [adversarial, measured](qint64 waited, bool gaveUp) {
                    QJsonObject nesting = measured(waited, gaveUp);
                    nesting["depth"] = NestingDepth;
                    (*adversarial)["nesting"] = nesting;
                });
    // Keys faster than any view settles, in the middle of the text.
    auto rapid = std::make_shared<std::pair<std::vector<double>, std::vector<qint64>>>();
    then(t.placeMs, [this] { reveal(0.5); });
    for (int i = 0; i < RapidKeys; ++i) {
        then(i == 0 ? t.placeMs : t.rapidKeyMs, [this, rapid, edit, i] {
            if (i == 0) {
                edit->cpuBefore = cpuMs();
                edit->parsesBefore = md::parseCount();
            }
            typeKey(u'x', &rapid->first, &rapid->second);
            edit->sent = rapid->second.back();
        });
    }
    thenWaitFor([this, edit] { return caughtUpSince(edit->sent); }, t.catchUpMs,
                [this, adversarial, rapid, edit](qint64, bool gaveUp) {
                    std::vector<double> latency;
                    for (const qint64 sent : rapid->second)
                        if (const qint64 frame = firstFrameAfter(sent); frame >= 0) latency.push_back(toMs(frame - sent));
                    const double span = toMs(rapid->second.back() - rapid->second.front());
                    (*adversarial)["rapid"] = QJsonObject{{"keys", int(rapid->second.size())},
                                                          {"keysPerSecond", span > 0 ? 1000.0 * (rapid->second.size() - 1) / span : 0.0},
                                                          {"dispatchMs", stats(rapid->first)},
                                                          {"keyToFrameMs", stats(latency)},
                                                          {"caughtUpMs", toMs(now() - edit->sent)},
                                                          {"caughtUp", !gaveUp},
                                                          {"cpuMs", cpuMs() - edit->cpuBefore},
                                                          {"parses", double(md::parseCount() - edit->parsesBefore)}};
                    m_out["adversarialEdits"] = *adversarial;
                });
}

// Idle: the window as it is left once it has settled, after saving and the
// session have landed. The caret blinks until the writer has paused for ten
// seconds, and putting it in view counts as moving it, so the minute begins
// once every view has caught up with the text and the caret has come to
// rest, and then the saves and the session have landed: from then on
// nothing changes, and no frame is drawn. Every wakeup of every thread
// counts, and so does the CPU time of the helpers parch started.
void Benchmark::planIdle()
{
    const Timing &t = m_timing;
    then(0, [this] {
        m_out["pssEndMiB"] = pssMiB(QStringLiteral("self"));
        m_out["ownedProcessesEnd"] = ownedProcesses();
        // The idle window has the caret in view at the end of the text.
        if (m_options.view != QLatin1String("reading")) reveal(1.0);
    });
    thenWaitFor([this] { return caughtUp() && caretsResting(); }, t.catchUpMs, [this](qint64 waited, bool gaveUp) {
        m_out["idleSettleMs"] = toMs(waited);
        m_out["idleSettled"] = !gaveUp;
    });
    struct Idle {
        qint64 at = 0;
        qint64 ticks = 0;
        qint64 childTicks = 0;
        QHash<QString, qint64> slices;
        qint64 switches = 0;
        QHash<qint64, qint64> owned;
    };
    auto idle = std::make_shared<Idle>();
    auto blinks = std::make_shared<int>(0);
    auto caretWatch = std::make_shared<QMetaObject::Connection>();
    then(t.quietMs, [this, idle, blinks, caretWatch] {
        if (auto *caret = qobject_cast<QQuickItem *>(findNamed(m_editor, QStringLiteral("caret"))))
            *caretWatch = connect(caret, &QQuickItem::visibleChanged, this, [blinks] { ++*blinks; });
        *idle = {now(), cpuTicks(), childCpuTicks(), wakeups(), contextSwitches(), ownedTicks()};
    });
    then(t.idleMs, [this, idle, blinks, caretWatch] {
        const qint64 end = now();
        disconnect(*caretWatch);
        const int frames = framesBetween(idle->at, end);
        m_out["idleMs"] = toMs(end - idle->at);
        m_out["idleFrames"] = frames;
        if (m_second) m_out["idleFramesSecondWindow"] = otherFramesBetween(idle->at, end);
        m_out["idleCaretChanges"] = *blinks;
        m_out["idleFramesBesidesCaret"] = std::max(0, frames - *blinks);
        m_out["cursorFlashTimeMs"] = QGuiApplication::styleHints()->cursorFlashTime();
        m_out["idleCpuTicks"] = cpuTicks() - idle->ticks;
        m_out["idleChildCpuTicks"] = childCpuTicks() - idle->childTicks;
        // Helpers still running: what they used over the minute, and those
        // started meanwhile all they used.
        qint64 helperTicks = 0;
        const QHash<qint64, qint64> owned = ownedTicks();
        for (auto it = owned.cbegin(); it != owned.cend(); ++it)
            helperTicks += std::max<qint64>(0, it.value() - idle->owned.value(it.key(), 0));
        m_out["idleOwnedCpuTicks"] = helperTicks;
        m_out["idleCpuTicksWithHelpers"] =
            m_out["idleCpuTicks"].toInteger() + m_out["idleChildCpuTicks"].toInteger() + helperTicks;
        // Every thread's wakeups over the minute, those of threads started
        // meanwhile included. A thread that ended meanwhile, as a worker
        // its pool let go of, took its count along: the context switches,
        // which the kernel keeps for ended threads too, count it.
        const QHash<QString, qint64> slices = wakeups();
        qint64 woken = 0;
        int ended = 0;
        for (auto it = slices.cbegin(); it != slices.cend(); ++it)
            woken += std::max<qint64>(0, it.value() - idle->slices.value(it.key(), 0));
        for (auto it = idle->slices.cbegin(); it != idle->slices.cend(); ++it)
            if (!slices.contains(it.key())) ++ended;
        m_out["idleWakeups"] = woken;
        m_out["idleThreadsEnded"] = ended;
        m_out["idleContextSwitches"] = contextSwitches() - idle->switches;
        m_out["clockTicksPerSecond"] = int(sysconf(_SC_CLK_TCK));
        m_out["ownedProcessesIdle"] = ownedProcesses();
    });
}

// The control: the same scroll and re-wrap over plain stripes in the
// editor's place, measured in the same window.
void Benchmark::planControl()
{
    const Timing &t = m_timing;
    const int settleDelay = m_pane->property("rewrapSettleMs").toInt();
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
        const std::vector<double> frames = intervals(*controlScroll, now());
        (*control)["scrollFrameMs"] = stats(frames);
        (*control)["scrollDroppedFrames"] = droppedFrames(frames);
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
}

// A recovery draft of the text as it is now, as a restart after a crash
// finds it: read from recovery, made a document, and shown in a tab of
// the window until it is editable and drawn. The measured document comes
// to the front again afterwards.
void Benchmark::planRecovery()
{
    if (!m_backend) return;
    struct Restore {
        qint64 began = 0;
        qint64 read = 0;
        qint64 editable = 0;
        int units = 0;
        QPointer<Document> document;
    };
    auto restore = std::make_shared<Restore>();
    then(500, [this, restore] {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString text = m_document->buffer()->text();
        restore->units = int(text.size());
        QString error;
        if (!m_backend->recovery()->write(id, QString(), SourceSnapshot{1, text}, DiskVersion{}, &error))
            return fail(tr("cannot write a recovery draft: %1").arg(error));
        restore->began = now();
        for (const Recovery::Entry &entry : m_backend->recovery()->pending())
            if (entry.documentId == id) restore->document = m_backend->store()->restore(entry);
        restore->read = now();
        if (!restore->document) return fail(tr("the recovery draft was not restored"));
        contextOf(m_window)->tabs()->openDocument(restore->document);
    });
    thenWaitFor(
        [this, restore] {
            if (!restore->document || m_binding->document() != restore->document || !m_binding->editable()) return false;
            const int expected = m_binding->sourceToEditor(restore->document->buffer()->length());
            return expected >= 0 && m_editor->property("length").toInt() == expected;
        },
        m_timing.waitMs,
        [this, restore](qint64, bool gaveUp) {
            if (gaveUp) return fail(tr("the recovery draft never became editable"));
            restore->editable = now();
            m_window->update();
        });
    then(200, [this, restore] {
        const qint64 frame = firstFrameAfter(restore->editable);
        m_out["recoveryRestore"] = QJsonObject{{"units", restore->units},
                                               {"readMs", toMs(restore->read - restore->began)},
                                               {"toEditableMs", toMs(restore->editable - restore->began)},
                                               {"toFrameMs", frame >= 0 ? toMs(frame - restore->began) : -1.0}};
        TabModel *tabs = contextOf(m_window)->tabs();
        tabs->setActiveIndex(tabs->indexOf(m_document));
    });
}
