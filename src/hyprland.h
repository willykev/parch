#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>

class QLocalSocket;

// Hyprland's effective options, read over its own sockets and never by
// starting a process. A question is one short connection to the request
// socket, `.socket.sock`: "j/getoption <option>", answered with JSON, after
// which Hyprland closes it. Hyprland reads a question as soon as it takes the
// connection and serves nothing else meanwhile, not even a window's drawing,
// for up to five seconds. So each question is asked whole on a thread of its
// own, written in full the moment it connects, and never waits for the UI
// thread, which can be busy at any time. The event socket, `.socket2.sock`,
// stays open, is only ever read, and says "configreloaded>>" whenever
// Hyprland has read its configuration again, as it does when Omarchy changes
// theme or toggles a setting. Hyprland writes events without waiting for
// their reader, keeping what a slow one has not taken yet, and parch takes
// everything there is whenever its event loop runs. Should the connection
// drop anyway, parch listens again and asks again, so a late reader costs
// parch, never Hyprland. Nothing here runs on a timer while all is well:
// after the first questions, only an event or ask() asks again.
class HyprlandOptions : public QObject {
    Q_OBJECT
public:
    // The folder of the running Hyprland's sockets,
    // $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE, or nothing outside
    // Hyprland.
    static QString sessionFolder();

    // Reads `options` from the Hyprland whose sockets are in `folder`. An
    // empty folder, or one without sockets, is no Hyprland: nothing is asked.
    HyprlandOptions(const QString &folder, const QStringList &options, QObject *parent = nullptr);
    ~HyprlandOptions() override;

    // Asks for every option again, unless that is already being asked, and
    // listens for events again if the connection was lost.
    void ask();

    // The longest a question may take before it is given up.
    static constexpr int AnswerTimeoutMs = 2000;
    // One question, start to end on the calling thread: connects to the
    // request socket at `socketPath`, writes the whole question before
    // anything else, and reads until Hyprland closes the connection. The
    // answer, or nothing when there is none within timeoutMs, or once
    // `stopped` is set.
    static std::optional<QByteArray> request(const QString &socketPath, const QByteArray &question, int timeoutMs,
                                             std::shared_ptr<const std::atomic_bool> stopped);
    // Waits between attempts to listen again after the event connection
    // drops: doubling from the first, and no more attempts after the last.
    static constexpr int FirstRetryMs = 1000;
    static constexpr int LastRetryMs = 60000;

signals:
    // Hyprland's JSON answer for an option.
    void answered(const QString &option, const QJsonObject &reply);

private:
    void question(const QString &option);
    void listen();
    void readEvents();
    void eventsLost();

    QString m_folder;
    QStringList m_options;
    QStringList m_asking;
    // Where the questions are asked, one thread each, and what tells one
    // still waiting for its answer that it is no longer wanted.
    QThreadPool m_pool;
    std::shared_ptr<std::atomic_bool> m_stopped = std::make_shared<std::atomic_bool>(false);
    QLocalSocket *m_events = nullptr;
    QByteArray m_pending;
    // The event connection dropped, so events may have been missed.
    bool m_lost = false;
    QTimer m_retry;
    int m_retryMs = FirstRetryMs;
};
