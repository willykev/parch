#pragma once

#include "apppaths.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <optional>

// One tab: the document it shows and where its view was. Positions are
// UTF-16 offsets in the document's source.
struct ViewState {
    QString documentId;
    // The file the tab shows; empty for a draft.
    QString path;
    int cursor = 0;
    int anchor = 0;
    // The first visible line, as a source offset.
    int top = 0;
    // What the tab was called, so that a tab not yet read is still named.
    QString title = {};
    // How the view shows its document: the source alone, the source beside
    // its preview, or the preview alone.
    enum Mode { Source, Split, Reading };
    int mode = Source;
    // The part of the width the source takes while it is beside the preview.
    double split = 0.5;

    static constexpr double MinSplit = 0.2, MaxSplit = 0.8;
};

// One window: its tabs, the one in front, whether the library is shown
// beside the editor, and its size.
struct WindowState {
    QList<ViewState> tabs;
    int activeTab = 0;
    bool libraryOpen = true;
    // Invalid when the window should take its default size.
    QSize size;
};

struct SessionState {
    QList<WindowState> windows;
};

// The windows and tabs of the last run, kept in AppPaths::sessionFile() as
// JSON and replaced atomically.
namespace Session {
// Writes the state at once, on the calling thread.
bool save(const AppPaths &paths, const SessionState &state);
// The state last saved. A missing file gives no windows. A file that cannot
// be read, or not as a session, gives no windows either and is moved aside as
// <name>.bad (or .bad.2 and on), so that it is never written over. Values out
// of range are mended and tabs that name nothing usable are left out, without
// giving up the rest.
SessionState load(const AppPaths &paths);
QByteArray encode(const SessionState &state);
bool write(const AppPaths &paths, const QByteArray &bytes);
} // namespace Session

// Writes the session off the UI thread, one write at a time. A state given
// while a write runs waits for it, and a newer state replaces one already
// waiting, so that writes never pile up.
class SessionWriter : public QObject {
    Q_OBJECT
public:
    explicit SessionWriter(const AppPaths &paths, QObject *parent = nullptr);
    // Waits for a write in progress, so that none is cut short.
    ~SessionWriter() override;

    // Encodes the state now and writes it on a worker thread.
    void write(const SessionState &state);
    // Waits for a write in progress, drops any waiting state, and writes this
    // one before returning, for the end of the application.
    bool finish(const SessionState &state);
    // How many writes reached the file.
    int written() const { return m_written; }

private:
    void start(const QByteArray &bytes);
    void finished();

    AppPaths m_paths;
    QFutureWatcher<bool> m_watcher;
    std::optional<QByteArray> m_waiting;
    bool m_busy = false;
    int m_written = 0;
};
