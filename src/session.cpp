#include "session.h"

#include "diskfile.h"
#include "pathtext.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtConcurrent>
#include <cmath>
#include <limits>

namespace {
constexpr int formatVersion = 1;
// Bounds that keep a damaged or hostile file from costing more than a session can use.
constexpr qsizetype maxWindows = 64;
constexpr qsizetype maxTabs = 1000;
constexpr qsizetype maxTitle = 200;
constexpr int minSide = 100;
constexpr int maxSide = 100000;

// A whole number from `low` to `high`, as JSON writes one, or `fallback`.
int whole(const QJsonValue &value, int low, int high, int fallback)
{
    if (!value.isDouble()) return fallback;
    const double number = value.toDouble();
    if (std::floor(number) != number || number < low || number > high) return fallback;
    return int(number);
}

std::optional<ViewState> readTab(const QJsonValue &value)
{
    if (!value.isObject()) return std::nullopt;
    const QJsonObject tab = value.toObject();
    ViewState view;
    view.documentId = tab.value(QStringLiteral("id")).toString();
    view.path = tab.value(QStringLiteral("path")).toString();
    // A path names a file to open, so only a full path is used; a draft has none.
    if ((!view.path.isEmpty() && !PathText::isFullPath(view.path)) || view.documentId.contains(QChar(0)))
        return std::nullopt;
    if (view.path.isEmpty() && view.documentId.isEmpty()) return std::nullopt;
    constexpr int most = std::numeric_limits<int>::max();
    view.cursor = whole(tab.value(QStringLiteral("cursor")), 0, most, 0);
    view.anchor = whole(tab.value(QStringLiteral("anchor")), 0, most, 0);
    view.top = whole(tab.value(QStringLiteral("top")), 0, most, 0);
    view.title = tab.value(QStringLiteral("title")).toString().left(maxTitle);
    // A mode this version does not know is the source alone.
    const QString mode = tab.value(QStringLiteral("mode")).toString();
    view.mode = mode == QLatin1String("split")     ? ViewState::Split
                : mode == QLatin1String("reading") ? ViewState::Reading
                                                   : ViewState::Source;
    const QJsonValue split = tab.value(QStringLiteral("split"));
    if (split.isDouble() && split.toDouble() >= ViewState::MinSplit && split.toDouble() <= ViewState::MaxSplit)
        view.split = split.toDouble();
    return view;
}

std::optional<WindowState> readWindow(const QJsonValue &value)
{
    if (!value.isObject()) return std::nullopt;
    const QJsonObject window = value.toObject();
    WindowState state;
    const QJsonArray tabs = window.value(QStringLiteral("tabs")).toArray();
    // The tab in front is counted among the tabs as written, before any is left out.
    const int active = whole(window.value(QStringLiteral("activeTab")), 0, int(tabs.size()) - 1, 0);
    for (qsizetype i = 0; i < tabs.size() && state.tabs.size() < maxTabs; ++i) {
        const std::optional<ViewState> tab = readTab(tabs.at(i));
        if (!tab) continue;
        if (i <= active) state.activeTab = int(state.tabs.size());
        state.tabs.append(*tab);
    }
    if (state.tabs.isEmpty()) return std::nullopt;
    const QJsonValue library = window.value(QStringLiteral("libraryOpen"));
    state.libraryOpen = library.isBool() ? library.toBool() : true;
    const int width = whole(window.value(QStringLiteral("width")), minSide, maxSide, 0);
    const int height = whole(window.value(QStringLiteral("height")), minSide, maxSide, 0);
    if (width > 0 && height > 0) state.size = QSize(width, height);
    return state;
}
} // namespace

QByteArray Session::encode(const SessionState &state)
{
    QJsonArray windows;
    for (const WindowState &window : state.windows) {
        QJsonArray tabs;
        for (const ViewState &view : window.tabs) {
            QJsonObject tab;
            if (!view.documentId.isEmpty()) tab.insert(QStringLiteral("id"), view.documentId);
            if (!view.path.isEmpty()) tab.insert(QStringLiteral("path"), view.path);
            tab.insert(QStringLiteral("cursor"), view.cursor);
            tab.insert(QStringLiteral("anchor"), view.anchor);
            tab.insert(QStringLiteral("top"), view.top);
            if (!view.title.isEmpty()) tab.insert(QStringLiteral("title"), view.title);
            if (view.mode == ViewState::Split) tab.insert(QStringLiteral("mode"), QStringLiteral("split"));
            else if (view.mode == ViewState::Reading) tab.insert(QStringLiteral("mode"), QStringLiteral("reading"));
            if (view.split != 0.5) tab.insert(QStringLiteral("split"), view.split);
            tabs.append(tab);
        }
        QJsonObject object;
        object.insert(QStringLiteral("tabs"), tabs);
        object.insert(QStringLiteral("activeTab"), window.activeTab);
        object.insert(QStringLiteral("libraryOpen"), window.libraryOpen);
        if (window.size.isValid()) {
            object.insert(QStringLiteral("width"), window.size.width());
            object.insert(QStringLiteral("height"), window.size.height());
        }
        windows.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), formatVersion);
    root.insert(QStringLiteral("windows"), windows);
    return QJsonDocument(root).toJson();
}

bool Session::write(const AppPaths &paths, const QByteArray &bytes)
{
    const QString file = paths.sessionFile();
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) return false;
    QSaveFile output(file);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}

bool Session::save(const AppPaths &paths, const SessionState &state)
{
    return write(paths, encode(state));
}

SessionState Session::load(const AppPaths &paths)
{
    const QString file = paths.sessionFile();
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) {
        // A file that is there but cannot be read is moved aside like a
        // damaged one, so that the next write does not replace it.
        if (QFile::exists(file) && !DiskFile::moveAside(file))
            qWarning("parch: the unreadable session %s cannot be moved aside", qPrintable(file));
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &error);
    input.close();
    const QJsonObject root = document.object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    const bool readable = error.error == QJsonParseError::NoError && document.isObject()
        && root.value(QStringLiteral("windows")).isArray() && (version.isUndefined() || version.toInt(-1) == formatVersion);
    if (!readable) {
        if (!DiskFile::moveAside(file))
            qWarning("parch: the damaged session %s cannot be moved aside", qPrintable(file));
        return {};
    }
    SessionState state;
    const QJsonArray windows = root.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &value : windows) {
        if (state.windows.size() >= maxWindows) break;
        if (const std::optional<WindowState> window = readWindow(value)) state.windows.append(*window);
    }
    return state;
}

SessionWriter::SessionWriter(const AppPaths &paths, QObject *parent) : QObject(parent), m_paths(paths)
{
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &SessionWriter::finished);
}

SessionWriter::~SessionWriter()
{
    m_watcher.waitForFinished();
}

void SessionWriter::write(const SessionState &state)
{
    const QByteArray bytes = Session::encode(state);
    // Busy until the finished write has been seen here, not only until the
    // worker is done, so that exactly one write is ever in flight.
    if (m_busy) {
        m_waiting = bytes;
        return;
    }
    start(bytes);
}

bool SessionWriter::finish(const SessionState &state)
{
    m_waiting.reset();
    // An older write must not land after this one.
    m_watcher.waitForFinished();
    const bool ok = Session::write(m_paths, Session::encode(state));
    if (ok) ++m_written;
    return ok;
}

void SessionWriter::start(const QByteArray &bytes)
{
    m_busy = true;
    m_watcher.setFuture(QtConcurrent::run(&Session::write, m_paths, bytes));
}

void SessionWriter::finished()
{
    m_busy = false;
    if (m_watcher.result()) ++m_written;
    else qWarning("parch: the session could not be saved to %s", qPrintable(m_paths.sessionFile()));
    if (m_waiting) start(*std::exchange(m_waiting, std::nullopt));
}
