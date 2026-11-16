#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QTemporaryDir;
class QTimer;

// The programs parch runs to draw math and diagrams, which come with it but
// are not part of it: RaTeX's render-svg for TeX math and Merman's
// merman-cli for Mermaid diagrams.
struct HelperSet {
    QString ratex;
    QString merman;

    // This machine's architecture as the helper folders name it: aarch64 or x86_64.
    static QString architecture();
    // Where helpers are looked for, in order: the folder PARCH_HELPERS names
    // (and nowhere else when it is set), the executable's own folder, the
    // package's /usr/lib/parch, then a development build's
    // build/helpers/<arch>, beside or above the executable.
    static QStringList searchFolders();
    // The first of those folders that holds each helper; a helper found in
    // none of them stays empty.
    static HelperSet find();
    // Both helpers in one folder, whether or not they are there.
    static HelperSet in(const QString &folder);

    // Which version each helper is, for reports: "RaTeX 0.1.15", "Merman
    // 0.10.0"; empty for a helper that is not there. The versions come from
    // the `.pins` file beside a helper, which bin/fetch-helpers and the
    // package write ("ratex 0.1.15 …" a line); without one, merman-cli is
    // asked (--version), while render-svg cannot say and is "RaTeX (version
    // unknown)". Runs merman-cli, so not for the GUI thread.
    QString ratexVersion() const;
    QString mermanVersion() const;
};

// How hard a helper may be pushed. A helper that runs past its time or
// writes past its output limit is stopped and its result thrown away. The
// kernel holds it to the rest: its address space, its processor time, and
// the size of any file it writes (maxOutputBytes); it dumps no core.
struct RenderLimits {
    int maxProcesses = 2;
    int timeoutMs = 15000;
    qint64 maxMemoryBytes = qint64(2) << 30;
    int maxCpuSeconds = 30;
    qint64 maxOutputBytes = 32 * 1024 * 1024;
    qint64 maxErrorBytes = 64 * 1024;
    qint64 diskCacheBytes = 128 * 1024 * 1024;
    qint64 memoryCacheBytes = 32 * 1024 * 1024;
};

// One run of a helper: the program, its arguments, what it reads on its
// standard input, and files it finds in its working folder by name.
struct HelperRun {
    QString program;
    QStringList arguments;
    QByteArray input;
    QList<QPair<QString, QByteArray>> files;
    // When set, the input is made from what the run before wrote.
    std::function<QByteArray(const QByteArray &)> inputFromPrevious;
};

struct HelperOutcome {
    enum Status {
        Finished, // it exited by itself, whatever its exit code
        Failed,   // it could not start, or a signal ended it
        TimedOut,
        TooMuchOutput,
        Cancelled,
    };
    Status status = Failed;
    int exitCode = -1;
    QByteArray output;
    QByteArray errors;
    qint64 elapsedMs = 0;
    // Whether the helper ran without a network: in a network namespace of its
    // own, which holds nothing but an unconnected loopback device. The kernel
    // may refuse one (user namespaces turned off); the helper then runs anyway.
    bool networkIsolated = false;
    // Helper process id, while it lived, for tests that look for leftovers.
    qint64 processId = 0;

    bool succeeded() const { return status == Finished && exitCode == 0; }
};

// Runs one helper under supervision, on the thread it lives on. The helper
// gets a new session (so that it and anything it starts are stopped
// together), a private temporary home that is its working folder and is
// removed afterwards, an environment holding only that home, a temporary
// folder inside it and a UTF-8 locale, no core dumps, no network, and the
// limits above. It is killed when its parent thread ends.
class HelperProcess : public QObject {
    Q_OBJECT

public:
    HelperProcess(const HelperRun &run, const RenderLimits &limits, const QString &temporaryRoot,
                  QObject *parent = nullptr);
    ~HelperProcess() override;

    void start();
    // Stops the helper and everything in its session; finished() follows
    // with the Cancelled status.
    void cancel();
    qint64 processId() const;

    // Whether the kernel lets a helper have a network namespace of its own
    // without privileges (unprivileged user namespaces). Asked of the kernel
    // once, by a child that tries; where it cannot, helpers run with the
    // network, and `parch check --json` says so. A test may answer instead.
    static bool canIsolate();
    static void setIsolationProbe(std::function<bool()> probe);

signals:
    void finished(const HelperOutcome &outcome);

private:
    void stop(HelperOutcome::Status status);
    // Appends what the helper wrote; true when it is past the limit.
    bool takeOutput();
    void takeErrors();
    void readOutput();
    void readErrors();
    void conclude(int exitCode, bool crashed);

    HelperRun m_run;
    RenderLimits m_limits;
    QString m_temporaryRoot;
    QTemporaryDir *m_home = nullptr;
    QProcess *m_process = nullptr;
    QTimer *m_deadline = nullptr;
    HelperOutcome m_outcome;
    HelperOutcome::Status m_stopReason = HelperOutcome::Finished;
    bool m_stopped = false;
    bool m_done = false;
    QElapsedTimer m_clock;
};

// Who a helper is: the SHA-256 of its executable, worked out again only when
// the file's size, time or inode changes. Results of one helper are never
// taken for another's. Thread-safe.
class HelperIdentity {
public:
    static QString of(const QString &path);
};
