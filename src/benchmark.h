#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

class Backend;
class Document;
class EditorBinding;
class QQuickItem;
class QQuickWindow;
class WindowContext;

// What `parch open --benchmark OUT FILE` measures, in the real window with
// saving, recovery, history and file watching running, and with live
// formatting and spelling on. main() gives it a temporary folder of its own
// for every file parch keeps and a copy of FILE to open there.
//
// The document is shown as source, beside its preview, or as the preview
// alone (Options::view), in one window or in two that show the same
// document (Options::windows). Once the window shows the document and takes
// keys, it types 20 keys at the start, the middle and the end, each place
// until every view of the text has caught up, scrolls 60 pixels a frame for
// four seconds, jumps to ten distant places, narrows and widens the text
// column 4 pixels a frame, optionally edits as no writer does
// (Options::adversarial: a paste of a megabyte and its undo, nesting two
// hundred deep, and keys faster than any view settles), leaves the window
// idle for a minute with the caret in view, from the moment every view has
// caught up and the caret has come to rest, does the same scroll and
// re-wrap over a plain striped item in the editor's place as a control, and
// finally restores a recovery draft of the text as it is then. As the
// preview alone shows no text to type in, that view scrolls the preview and
// neither types nor re-wraps.
// Keys are Qt key events sent to the window; a frame is counted when the
// window swaps it. The results are written to OUT as JSON.
//
// Nothing here exists unless the benchmark was asked for, so it adds nothing
// to an ordinary launch or to typing.
class Benchmark : public QObject {
    Q_OBJECT
public:
    // How long each part lasts. The defaults are the measurement's; the
    // tests run it in a few seconds.
    struct Timing {
        int settleMs = 1500;  // from the first frame to measuring the loaded window
        int placeMs = 400;    // before typing at each place, and after
        int keyMs = 60;       // between keys
        int scrollMs = 4000;
        int jumpMs = 150;
        int rewrapMs = 2500;
        int quietMs = 3000;   // before idling, for saves and the session to land
        int idleMs = 60000;
        int waitMs = 10000;   // the longest wait for the document and its first frame
        int caughtUpMs = 400; // after the last key, at least, before the views count as caught up
        int catchUpMs = 30000; // the longest wait for the views to catch up
        int rapidKeyMs = 5;   // between keys typed faster than any view settles
        int pasteUnits = 1 << 20; // the adversarial paste, in UTF-16 units
    };
    // What is measured.
    struct Options {
        QString view = QStringLiteral("source"); // "source", "split" or "reading"
        int windows = 1;                          // 2: a second window shows the same document
        bool adversarial = false;
    };

    Benchmark(const QString &output, const QString &label, const Timing &timing, QObject *parent = nullptr);
    Benchmark(const QString &output, const QString &label, QObject *parent = nullptr);
    ~Benchmark() override;

    // The steady clock in nanoseconds, the clock every time here is taken on.
    static qint64 now();
    // Copies `file` into `folder`, which it makes, under the same name, with
    // what it reads from its own folder (filesRead()) where it is beside it.
    // Returns the copy, or nothing with the reason in `error`.
    static QString copyIn(const QString &file, const QString &folder, QString *error);
    // The files a document reads besides its text: what it includes, the
    // pictures it and they show, its bibliographies and a style file.
    static QStringList filesRead(const QString &file);

    void setOptions(const Options &options) { m_options = options; }
    // The application measured: its settings, windows, store and recovery.
    void setBackend(Backend *backend);
    // When main() began.
    void setLaunchTime(qint64 mainNs);
    // Just before the window and its document are made.
    void opening();
    // Measures this window, which is opening the document. It begins once
    // the window shows the whole document and takes keys.
    void start(QQuickWindow *window);

signals:
    // The results are written, with code 0; or the run failed, with the
    // reason. Always from the event loop, never from inside start().
    void finished(int code, const QString &error);

private slots:
    // A driver moved the column one step.
    void stepTaken();

private:
    struct Frames;
    struct Views;

    void then(int delayMs, std::function<void()> step);
    // A step that waits until `ready` holds, or `capMs` has passed, and
    // then calls `done` with how long it waited and whether it gave up.
    void thenWaitFor(std::function<bool()> ready, int capMs, std::function<void(qint64 waitedNs, bool gaveUp)> done);
    void next();
    void fail(const QString &reason);
    bool editorReady() const;
    void waitForEditor();
    // The steps after the document shows, in order.
    void plan();
    void planTyping();
    void planScrolling();
    void planAdversarial();
    void planIdle();
    void planControl();
    void planRecovery();

    qint64 firstFrameAfter(qint64 t) const;
    std::vector<double> intervals(qint64 from, qint64 to) const;
    int framesBetween(qint64 from, qint64 to) const;
    int otherFramesBetween(qint64 from, qint64 to) const;
    int widthChangesBetween(qint64 from, qint64 to) const;
    // Frames that missed the display's refresh while the frame intervals
    // were measured: an interval of n refresh intervals missed n - 1.
    int droppedFrames(const std::vector<double> &intervals) const;
    double refreshIntervalMs() const;
    // A FrameAnimation from QML text, with its initial properties.
    QObject *make(const QByteArray &qml, const QVariantMap &properties);
    void stopDrivers();
    // Puts the cursor at a fraction of the text, shows it, and gives the text the keys.
    void reveal(double fraction);
    void typeKey(QChar key, std::vector<double> *dispatch, std::vector<qint64> *sent);
    // Every view of the text in every window has caught up with it: the
    // styling, the spelling, the preview and the word count.
    bool caughtUp() const;
    // The same, once at least Timing::caughtUpMs has passed since `since`.
    bool caughtUpSince(qint64 since) const;
    // No editor's caret in any window measured is blinking: each has come
    // to rest, or is steady.
    bool caretsResting() const;
    void findViews(QQuickWindow *window, Views &views) const;
    void openSecondWindow();
    bool secondReady() const;
    WindowContext *contextOf(QQuickWindow *window) const;
    void showView(QQuickWindow *window);

    QString m_output;
    QString m_label;
    Timing m_timing;
    Options m_options;
    QPointer<Backend> m_backend;
    QJsonObject m_out;
    std::shared_ptr<Frames> m_frames;
    std::shared_ptr<Frames> m_otherFrames;
    std::vector<std::pair<int, std::function<void()>>> m_steps;
    size_t m_next = 0;
    bool m_done = false;
    // A step waits for something before the next one runs.
    bool m_holding = false;
    QTimer m_poll;
    QTimer m_watchdog;

    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_pane;
    QPointer<QQuickItem> m_editor;
    QPointer<QQuickItem> m_flickable;
    QPointer<QQuickItem> m_scrolled;
    QPointer<EditorBinding> m_binding;
    QPointer<Document> m_document;
    QPointer<QObject> m_scroller;
    QPointer<QObject> m_wrapper;
    QPointer<QQuickItem> m_control;
    QPointer<QQuickWindow> m_second;
    std::unique_ptr<Views> m_views;
    std::unique_ptr<Views> m_secondViews;

    qint64 m_mainNs = 0;
    qint64 m_openedNs = 0;
    qint64 m_readyNs = 0;
    int m_units = 0;
    int m_typed = 0;
    std::vector<qint64> m_widthChanges;
    std::vector<qint64> m_stepTimes;
    double m_rewrapBase = 0;
};
