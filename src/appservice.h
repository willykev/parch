#pragma once

#include "apppaths.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;

// Makes parch one application per account and session: the first launch
// listens on <runtime>/parch.sock, and every later launch hands its files to
// it and ends once the running instance has taken them. The socket is the
// account's alone and lives in a runtime folder that must be the account's
// own.
//
// Each message is a 32-bit big-endian length and that many bytes of a JSON
// object. A launch sends {"version": 1, "files": [full paths],
// "activationToken": "..."} (the token only when the launch was given one),
// and the instance answers {"version": 1, "accepted": true} once it has the
// files, or {"version": 1, "accepted": false, "reason": "..."}. A request
// larger than MaxRequestBytes, slower than TimeoutMs, malformed, of another
// version, or naming a path that is not a full path is refused whole. Paths
// are only ever opened as documents; nothing received is run.
class AppService : public QObject {
    Q_OBJECT
public:
    static constexpr int ProtocolVersion = 1;
    static constexpr int MaxRequestBytes = 1 << 20;
    static constexpr int MaxFiles = 1000;
    static constexpr int MaxTokenLength = 512;
    static constexpr int TimeoutMs = 2000;
    // Requests being read at once; any more are closed unread.
    static constexpr int MaxClients = 16;

    explicit AppService(const AppPaths &paths, QObject *parent = nullptr);
    ~AppService() override;

    static QString socketPath(const AppPaths &paths);

    // False when another instance owns the socket, or when the runtime
    // folder cannot be trusted, which error() then explains. A socket left by
    // an instance that crashed is replaced; a live instance's never is.
    bool listen();
    // Stops taking requests and removes the socket, so that a later launch
    // starts an instance of its own; for when this one is ending.
    void close();
    // Refuses every request from now on, with this reason, while the socket
    // stays: for an instance that is ending but has not yet closed it.
    void refuse(const QString &reason);
    // True once a running instance has answered that it has the files. Needs
    // no application object, so that a launch can ask before it opens a
    // display. `error` stays empty when no instance listens at all, and
    // otherwise says why the files were not taken.
    static bool forward(const AppPaths &paths, const QStringList &files, QString *error = nullptr);
    QString error() const;
    // The activation token of the request being handled, as its launch was
    // given one; empty when it had none.
    QString activationToken() const;

signals:
    // The files a later launch handed over, with the activation token that
    // launch was given, or none.
    void openRequested(const QStringList &files, const QString &activationToken);

private:
    void accept();
    void read(QLocalSocket *socket);
    // Answers and closes; a refusal carries its reason.
    void answer(QLocalSocket *socket, bool accepted, const QString &reason = {});
    void drop(QLocalSocket *socket);

    AppPaths m_paths;
    QLocalServer *m_server;
    // The connections still being read; finished ones may wait for deletion.
    QList<QLocalSocket *> m_clients;
    QString m_error;
    QString m_activationToken;
    QString m_refusal;
};
