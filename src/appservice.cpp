#include "appservice.h"

#include "pathtext.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr int headerBytes = 4;
// An answer is a few words; anything longer is not one.
constexpr quint32 maxAnswerBytes = 1 << 16;
// Qt makes a private socket in a folder beside the name, <name>XXXXXX/s,
// and renames it into place, and the kernel limits a socket's path.
constexpr qsizetype privateSocketRoom = sizeof(sockaddr_un::sun_path) - 1 - 8;

QString errorText(int number)
{
    return QString::fromLocal8Bit(std::strerror(number));
}

QByteArray framed(const QJsonObject &message)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray frame(headerBytes, '\0');
    qToBigEndian(quint32(payload.size()), frame.data());
    return frame + payload;
}

// The socket's folder decides who can replace or answer for the socket, so
// it must be a real folder, not a link, owned by this account and writable
// by no other. With `create`, a missing one is made private to this account.
bool trustedFolder(const QString &path, bool create, QString *error)
{
    const QByteArray name = QFile::encodeName(path);
    if (!PathText::isFullPath(path)) {
        *error = AppService::tr("The runtime folder %1 is not a full path").arg(path);
        return false;
    }
    if (create && ::mkdir(name.constData(), 0700) != 0 && errno != EEXIST) {
        *error = AppService::tr("Cannot create the runtime folder %1: %2").arg(path, errorText(errno));
        return false;
    }
    struct stat info {};
    if (::lstat(name.constData(), &info) != 0) {
        *error = AppService::tr("Cannot use the runtime folder %1: %2").arg(path, errorText(errno));
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        *error = AppService::tr("The runtime folder %1 is not a folder").arg(path);
        return false;
    }
    if (info.st_uid != ::geteuid()) {
        *error = AppService::tr("The runtime folder %1 belongs to another account").arg(path);
        return false;
    }
    if (info.st_mode & (S_IWGRP | S_IWOTH)) {
        *error = AppService::tr("Other accounts can write to the runtime folder %1").arg(path);
        return false;
    }
    return true;
}

// A token is passed on to the compositor as it is, so only the printable
// ASCII that activation tokens are made of is accepted.
bool usableToken(const QString &token)
{
    if (token.isEmpty() || token.size() > AppService::MaxTokenLength) return false;
    for (const QChar c : token)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e) return false;
    return true;
}

struct Request {
    QStringList files;
    QString token;
    // Why it is refused; empty for a request to take.
    QString refusal;
};

Request parseRequest(const QByteArray &payload)
{
    Request request;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        request.refusal = AppService::tr("The request is not one parch understands");
        return request;
    }
    const QJsonObject message = document.object();
    const QJsonValue version = message.value(QStringLiteral("version"));
    if (!version.isDouble()) {
        request.refusal = AppService::tr("The request has no version");
        return request;
    }
    if (version.toDouble() != AppService::ProtocolVersion) {
        request.refusal = AppService::tr("This parch takes requests of version %1, not version %2")
                              .arg(AppService::ProtocolVersion)
                              .arg(version.toDouble());
        return request;
    }
    const QJsonValue files = message.value(QStringLiteral("files"));
    if (!files.isArray() || files.toArray().size() > AppService::MaxFiles) {
        request.refusal = AppService::tr("The request has no usable list of files");
        return request;
    }
    for (const QJsonValue &entry : files.toArray()) {
        if (!entry.isString() || !PathText::isFullPath(entry.toString())) {
            request.refusal = AppService::tr("The request names something that is not a full path");
            return request;
        }
        request.files.append(entry.toString());
    }
    if (message.contains(QStringLiteral("activationToken"))) {
        const QJsonValue token = message.value(QStringLiteral("activationToken"));
        if (!token.isString() || !usableToken(token.toString())) {
            request.refusal = AppService::tr("The request has an unusable activation token");
            return request;
        }
        request.token = token.toString();
    }
    return request;
}

// Waits until the socket is ready or the deadline passes. On the
// application's own thread its events go on meanwhile, so that an instance
// in the same process can answer, as in a test.
bool waitFor(int descriptor, short events, const QDeadlineTimer &deadline)
{
    while (!deadline.hasExpired()) {
        pollfd entry{descriptor, events, 0};
        const int ready = ::poll(&entry, 1, 10);
        if (ready > 0) return true;
        if (ready < 0 && errno != EINTR) return false;
        const QCoreApplication *application = QCoreApplication::instance();
        if (application && application->thread() == QThread::currentThread())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return false;
}
} // namespace

AppService::AppService(const AppPaths &paths, QObject *parent)
    : QObject(parent), m_paths(paths), m_server(new QLocalServer(this))
{
    connect(m_server, &QLocalServer::newConnection, this, &AppService::accept);
}

AppService::~AppService() = default;

QString AppService::socketPath(const AppPaths &paths)
{
    return paths.socketPath();
}

QString AppService::error() const { return m_error; }
QString AppService::activationToken() const { return m_activationToken; }

bool AppService::listen()
{
    m_error.clear();
    if (!m_paths.singleInstance) return trustedFolder(m_paths.runtime, false, &m_error);
    if (m_server->isListening()) return true;
    if (!trustedFolder(m_paths.runtime, true, &m_error)) return false;
    const QString path = socketPath(m_paths);
    // At the kernel limit there is room for the socket itself but not
    // Qt's temporary name. A private directory already restricts access.
    const bool direct = QFile::encodeName(path).size() > privateSocketRoom;
    struct stat folder {};
    if (!m_paths.socketPathFits() || (direct && (::lstat(QFile::encodeName(m_paths.runtime), &folder) || (folder.st_mode & 077)))) {
        m_error = tr("The runtime folder's path is too long for a socket: %1").arg(m_paths.runtime);
        return false;
    }
    // Two launches at once take turns from here until one listens, so that
    // the second finds the first alive instead of both replacing the socket.
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(2 * TimeoutMs)) {
        m_error = tr("Another parch is starting and has not finished");
        return false;
    }
    QLocalSocket probe;
    probe.connectToServer(path);
    const bool alive = probe.waitForConnected(TimeoutMs);
    probe.abort();
    if (alive) return false;
    // Nobody answers: the socket, if any, was left by an instance that ended
    // without removing it.
    QLocalServer::removeServer(path);
    m_server->setSocketOptions(direct ? QLocalServer::NoOptions : QLocalServer::UserAccessOption);
    if (!m_server->listen(path)) {
        m_error = tr("Cannot listen on %1: %2").arg(path, m_server->errorString());
        return false;
    }
    return true;
}

void AppService::close()
{
    for (QLocalSocket *socket : std::exchange(m_clients, {})) {
        socket->abort();
        socket->deleteLater();
    }
    // Removes the socket too, so a launch from now on starts its own instance.
    m_server->close();
}

bool AppService::forward(const AppPaths &paths, const QStringList &files, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (error) error->clear();
    if (!paths.singleInstance) return false;
    // A runtime folder that is missing has no instance in it; one that cannot
    // be trusted may hold someone else's socket, which is never sent to. The
    // launch then tries to listen itself, which says what is wrong.
    QString untrusted;
    if (!trustedFolder(paths.runtime, false, &untrusted)) return false;
    if (files.size() > MaxFiles) return fail(tr("Too many files at once: %1").arg(files.size()));
    for (const QString &file : files)
        if (!PathText::isFullPath(file)) return fail(tr("Not a full path: %1").arg(file));
    QJsonObject request{{QStringLiteral("version"), ProtocolVersion},
                        {QStringLiteral("files"), QJsonArray::fromStringList(files)}};
    const QString token = qEnvironmentVariable("XDG_ACTIVATION_TOKEN");
    if (usableToken(token)) request.insert(QStringLiteral("activationToken"), token);
    const QByteArray frame = framed(request);
    if (frame.size() > headerBytes + MaxRequestBytes) return fail(tr("Too many files at once: %1").arg(files.size()));

    // A plain socket, so that a launch can ask before any application object
    // exists and so before it opens a display.
    const QByteArray name = QFile::encodeName(socketPath(paths));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (name.size() >= qsizetype(sizeof(address.sun_path))) return false;
    std::memcpy(address.sun_path, name.constData(), size_t(name.size()));
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return false;
    const auto closed = qScopeGuard([descriptor] { ::close(descriptor); });
    // Non-blocking from the start, so that an instance too busy to accept
    // cannot hold a launch past its deadline: a full backlog says EAGAIN,
    // and the connection is tried again until the deadline.
    ::fcntl(descriptor, F_SETFL, ::fcntl(descriptor, F_GETFL) | O_NONBLOCK);
    const QDeadlineTimer deadline(TimeoutMs);
    for (;;) {
        if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) break;
        if (errno == EINTR) continue;
        if (errno == EINPROGRESS) {
            int error = 0;
            socklen_t length = sizeof(error);
            if (!waitFor(descriptor, POLLOUT, deadline)
                || ::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
                return fail(tr("The running parch did not answer"));
            break;
        }
        if (errno == EAGAIN && !deadline.hasExpired()) {
            ::poll(nullptr, 0, 10);
            continue;
        }
        if (errno == EAGAIN) return fail(tr("The running parch did not answer"));
        // Nobody listening, or a socket a crash left behind: no instance to ask.
        return false;
    }

    for (qsizetype sent = 0; sent < frame.size();) {
        const ssize_t count = ::send(descriptor, frame.constData() + sent, size_t(frame.size() - sent), MSG_NOSIGNAL);
        if (count > 0) {
            sent += count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(descriptor, POLLOUT, deadline)) {
            continue;
        } else {
            return fail(tr("The running parch did not take the request"));
        }
    }
    // The files are taken only once the instance says so.
    QByteArray answer;
    for (;;) {
        if (answer.size() >= headerBytes) {
            const quint32 length = qFromBigEndian<quint32>(answer.constData());
            if (length > maxAnswerBytes) return fail(tr("The running parch gave an answer parch does not understand"));
            if (answer.size() >= headerBytes + qsizetype(length)) break;
        }
        char buffer[4096];
        const ssize_t count = ::recv(descriptor, buffer, sizeof buffer, 0);
        if (count > 0) {
            answer.append(buffer, count);
        } else if (count == 0) {
            return fail(tr("The running parch closed the connection without taking the files"));
        } else if (errno == EINTR) {
            continue;
        } else if ((errno == EAGAIN || errno == EWOULDBLOCK) && waitFor(descriptor, POLLIN, deadline)) {
            continue;
        } else {
            return fail(tr("The running parch did not answer in time"));
        }
    }
    const QJsonObject reply =
        QJsonDocument::fromJson(answer.mid(headerBytes, qFromBigEndian<quint32>(answer.constData()))).object();
    if (reply.value(QStringLiteral("accepted")).toBool()) return true;
    const QString reason = reply.value(QStringLiteral("reason")).toString();
    return fail(reason.isEmpty() ? tr("The running parch did not take the files") : reason);
}

void AppService::accept()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        // Only connections still being read count; finished ones may wait for deletion.
        if (m_clients.size() >= MaxClients) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_clients.append(socket);
        // The socket reads no more than one whole request.
        socket->setReadBufferSize(headerBytes + MaxRequestBytes);
        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, socket] { drop(socket); });
        timer->start(TimeoutMs);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_clients.removeOne(socket);
            socket->deleteLater();
        });
        read(socket);
    }
}

void AppService::read(QLocalSocket *socket)
{
    if (!m_clients.contains(socket) || socket->bytesAvailable() < headerBytes) return;
    const QByteArray header = socket->peek(headerBytes);
    const quint32 length = qFromBigEndian<quint32>(header.constData());
    if (length > quint32(MaxRequestBytes)) {
        drop(socket);
        return;
    }
    if (socket->bytesAvailable() < headerBytes + qint64(length)) return;
    socket->skip(headerBytes);
    const Request request = parseRequest(socket->read(length));
    // One request per connection; whatever follows it is not read.
    m_clients.removeOne(socket);
    disconnect(socket, &QLocalSocket::readyRead, this, nullptr);
    if (!request.refusal.isEmpty() || !m_refusal.isEmpty()) {
        answer(socket, false, request.refusal.isEmpty() ? m_refusal : request.refusal);
        return;
    }
    m_activationToken = request.token;
    emit openRequested(request.files, request.token);
    answer(socket, true);
}

void AppService::answer(QLocalSocket *socket, bool accepted, const QString &reason)
{
    QJsonObject reply{{QStringLiteral("version"), ProtocolVersion}, {QStringLiteral("accepted"), accepted}};
    if (!reason.isEmpty()) reply.insert(QStringLiteral("reason"), reason);
    socket->write(framed(reply));
    // Closes once the answer is written; the time limit still ends a reader that never takes it.
    socket->disconnectFromServer();
}

void AppService::refuse(const QString &reason)
{
    m_refusal = reason;
}

void AppService::drop(QLocalSocket *socket)
{
    m_clients.removeOne(socket);
    socket->abort();
    socket->deleteLater();
}
