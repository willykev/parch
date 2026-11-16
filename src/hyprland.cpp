#include "hyprland.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
QString requestSocket(const QString &folder) { return folder + QStringLiteral("/.socket.sock"); }
QString eventSocket(const QString &folder) { return folder + QStringLiteral("/.socket2.sock"); }
// Far more than Hyprland's answer for one option, or one line of its events.
constexpr qsizetype answerLimit = 1 << 16;
// How often a question waiting for its answer looks whether it is still wanted.
constexpr int stopCheckMs = 50;
} // namespace

std::optional<QByteArray> HyprlandOptions::request(const QString &socketPath, const QByteArray &question,
                                                    int timeoutMs, std::shared_ptr<const std::atomic_bool> stopped)
{
    const QDeadlineTimer deadline(timeoutMs);
    const QByteArray path = QFile::encodeName(socketPath);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= qsizetype(sizeof address.sun_path)) return std::nullopt;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket < 0) return std::nullopt;
    const auto closeSocket = qScopeGuard([socket] { ::close(socket); });
    // Every wait is short, so that a question no longer wanted, or out of
    // time, ends within a moment: false once it is to end.
    const auto waitFor = [&](short events) {
        if (stopped->load() || deadline.hasExpired()) return false;
        const int slice = int(std::min<qint64>(stopCheckMs, deadline.remainingTime()));
        if (events == 0) {
            QThread::msleep(ulong(slice));
            return true;
        }
        pollfd ready{socket, events, 0};
        return ::poll(&ready, 1, slice) >= 0 || errno == EINTR;
    };
    // A full queue of connections is tried again, as long as the question may take.
    while (::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0)
        if ((errno != EAGAIN && errno != EINTR) || !waitFor(0)) return std::nullopt;
    // Hyprland waits for these bytes from the moment it takes the
    // connection, and nothing on this thread comes before them.
    for (qsizetype sent = 0; sent < question.size();) {
        const ssize_t wrote = ::send(socket, question.constData() + sent, size_t(question.size() - sent), MSG_NOSIGNAL);
        if (wrote > 0) sent += wrote;
        else if (wrote == 0 || (errno != EAGAIN && errno != EINTR) || !waitFor(POLLOUT)) return std::nullopt;
    }
    // Hyprland answers and closes the connection; anything else is no answer.
    QByteArray reply;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(socket, buffer, sizeof buffer);
        if (got == 0) return reply;
        if (got > 0) {
            reply.append(buffer, got);
            if (reply.size() > answerLimit) return std::nullopt;
        } else if ((errno != EAGAIN && errno != EINTR) || !waitFor(POLLIN)) {
            return std::nullopt;
        }
    }
}

QString HyprlandOptions::sessionFolder()
{
    const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (signature.isEmpty() || runtime.isEmpty() || signature.contains(u'/')) return {};
    return runtime + QStringLiteral("/hypr/") + signature;
}

HyprlandOptions::HyprlandOptions(const QString &folder, const QStringList &options, QObject *parent)
    : QObject(parent), m_options(options), m_retry(this)
{
    // Every option is asked at once, so that one slow answer holds up no other.
    m_pool.setMaxThreadCount(std::max(1, int(options.size())));
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &HyprlandOptions::listen);
    // Without its request socket there is no Hyprland to ask.
    if (folder.isEmpty() || !QFileInfo::exists(requestSocket(folder))) return;
    m_folder = folder;
    listen();
    ask();
}

HyprlandOptions::~HyprlandOptions()
{
    // A question still waiting for its answer gives up within a moment, and none outlives this object.
    m_stopped->store(true);
    m_pool.waitForDone();
    // A socket closing as it goes must not call back into this half-destroyed object.
    const auto sockets = findChildren<QLocalSocket *>(Qt::FindDirectChildrenOnly);
    for (QLocalSocket *socket : sockets) {
        socket->disconnect();
        socket->abort();
    }
}

void HyprlandOptions::ask()
{
    if (m_folder.isEmpty()) return;
    // Events lost for good are listened for again now, from the first wait.
    if (!m_events && !m_retry.isActive()) {
        m_retryMs = FirstRetryMs;
        listen();
    }
    for (const QString &option : std::as_const(m_options))
        if (!m_asking.contains(option)) question(option);
}

void HyprlandOptions::question(const QString &option)
{
    m_asking.append(option);
    using Answer = std::optional<QByteArray>;
    auto *watcher = new QFutureWatcher<Answer>(this);
    // The answer is taken on the UI thread, whenever its event loop runs.
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, option] {
        watcher->deleteLater();
        m_asking.removeOne(option);
        const Answer reply = watcher->result();
        const QJsonDocument json = reply ? QJsonDocument::fromJson(*reply) : QJsonDocument();
        if (json.isObject()) emit answered(option, json.object());
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, &HyprlandOptions::request, requestSocket(m_folder),
                                         "j/getoption " + option.toUtf8(), int(AnswerTimeoutMs),
                                         std::shared_ptr<const std::atomic_bool>(m_stopped)));
}

void HyprlandOptions::listen()
{
    if (m_events || m_folder.isEmpty()) return;
    auto *socket = new QLocalSocket(this);
    m_events = socket;
    connect(socket, &QLocalSocket::readyRead, this, &HyprlandOptions::readEvents);
    // Events missed while the connection was down are made up for by asking again.
    if (m_lost) connect(socket, &QLocalSocket::connected, this, [this] {
        m_lost = false;
        ask();
    });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
        if (socket == m_events) eventsLost();
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, socket] {
        if (socket == m_events) eventsLost();
    });
    // Only ever read: parch has nothing to say to Hyprland's events.
    socket->connectToServer(eventSocket(m_folder), QIODevice::ReadOnly);
}

void HyprlandOptions::readEvents()
{
    if (!m_events) return;
    // Everything there is, is taken at once.
    m_pending += m_events->readAll();
    bool reloaded = false;
    for (qsizetype end = m_pending.indexOf('\n'); end >= 0; end = m_pending.indexOf('\n')) {
        const QByteArray line = m_pending.left(end);
        m_pending.remove(0, end + 1);
        // Hyprland is there and talking: a later loss starts waiting afresh.
        m_retryMs = FirstRetryMs;
        if (line.startsWith("configreloaded>>")) reloaded = true;
    }
    // A line longer than any event parch looks for is no such event.
    if (m_pending.size() > answerLimit) m_pending.clear();
    if (reloaded) ask();
}

void HyprlandOptions::eventsLost()
{
    QLocalSocket *socket = std::exchange(m_events, nullptr);
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    m_pending.clear();
    m_lost = true;
    // Listening again waits longer each time, and stops after the longest
    // wait; asking again starts over.
    if (m_retryMs > LastRetryMs) return;
    m_retry.start(m_retryMs);
    m_retryMs *= 2;
}
