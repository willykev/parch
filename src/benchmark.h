#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

class Document;
class EditorBinding;
class QQuickItem;
class QQuickWindow;

// What `parch open --benchmark OUT FILE` measures, in the real window with
// saving, recovery, history and file watching running. main() gives it a
// temporary folder of its own for every file parch keeps and a copy of FILE
// to open there.
//
// Once the window shows the document and takes keys, it types 20 keys at the
// start, the middle and the end, scrolls 60 pixels a frame for four seconds,
// jumps to ten distant places, narrows and widens the text column 4 pixels a
// frame, leaves the window idle for a minute with the caret in view, and then
// does the same scroll and re-wrap over a plain striped item in the editor's
// place as a control.
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
    };

    Benchmark(const QString &output, const QString &label, const Timing &timing, QObject *parent = nullptr);
    Benchmark(const QString &output, const QString &label, QObject *parent = nullptr);
    ~Benchmark() override;

    // The steady clock in nanoseconds, the clock every time here is taken on.
    static qint64 now();
    // Copies `file` into `folder`, which it makes, under the same name.
    // Returns the copy, or nothing with the reason in `error`.
    static QString copyIn(const QString &file, const QString &folder, QString *error);

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

    void then(int delayMs, std::function<void()> step);
    void next();
    void fail(const QString &reason);
    bool editorReady() const;
    void waitForEditor();
    // The steps after the document shows, in order.
    void plan();

    qint64 firstFrameAfter(qint64 t) const;
    std::vector<double> intervals(qint64 from, qint64 to) const;
    int framesBetween(qint64 from, qint64 to) const;
    int widthChangesBetween(qint64 from, qint64 to) const;
    // A FrameAnimation from QML text, with its initial properties.
    QObject *make(const QByteArray &qml, const QVariantMap &properties);
    void stopDrivers();
    // Puts the cursor at a fraction of the text, shows it, and gives the text the keys.
    void reveal(double fraction);

    QString m_output;
    QString m_label;
    Timing m_timing;
    QJsonObject m_out;
    std::shared_ptr<Frames> m_frames;
    std::vector<std::pair<int, std::function<void()>>> m_steps;
    size_t m_next = 0;
    bool m_done = false;
    QTimer m_poll;
    QTimer m_watchdog;

    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_pane;
    QPointer<QQuickItem> m_editor;
    QPointer<QQuickItem> m_flickable;
    QPointer<EditorBinding> m_binding;
    QPointer<Document> m_document;
    QPointer<QObject> m_scroller;
    QPointer<QObject> m_wrapper;
    QPointer<QQuickItem> m_control;

    qint64 m_mainNs = 0;
    qint64 m_openedNs = 0;
    qint64 m_readyNs = 0;
    int m_units = 0;
    std::vector<qint64> m_widthChanges;
    std::vector<qint64> m_stepTimes;
    double m_rewrapBase = 0;
};
