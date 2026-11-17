#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>

// Where a preview gets the images a document names by http or https URL.
// The application has one fetcher that asks the network; a window without
// one, as in the tests, fetches nothing.
class RemoteFetcher {
public:
    virtual ~RemoteFetcher() = default;
    // Fetches the URL and calls `done` on the context's thread with its
    // bytes, or with why there are none; never once the context is gone.
    virtual void fetch(const QUrl &url, QObject *context,
                       std::function<void(const QByteArray &data, const QString &error)> done) = 0;
};

// Fetches over the network, from public addresses alone. Each host is
// resolved first and every address it has must be public (isPublic()):
// nothing on this computer, on its network or on the provider's, so that a
// document cannot make parch send requests to a router or a local service.
// Redirects are followed by parch itself, at most MaxRedirects of them,
// each to an http or https address, never from https to http, and each
// resolved and checked again. Only a picture's content type is taken (PNG,
// JPEG, GIF, WebP or SVG), at most MaxBytes of it, and the whole fetch,
// redirects and all, gives up after TimeoutMs.
//
// Each connection uses a checked numeric address. HTTP and TLS retain the
// original host name, and relative redirects resolve against the original URL.
class NetworkFetcher : public QObject, public RemoteFetcher {
    Q_OBJECT

public:
    static constexpr qint64 MaxBytes = qint64(32) << 20;
    static constexpr int TimeoutMs = 30000;
    static constexpr int MaxRedirects = 3;

    // Finds a host's addresses, and calls `answer` with them or with why
    // there are none.
    using Resolver = std::function<void(const QString &host,
                                        std::function<void(const QList<QHostAddress> &addresses, const QString &error)> answer)>;

    explicit NetworkFetcher(QObject *parent = nullptr);
    ~NetworkFetcher() override;
    void fetch(const QUrl &url, QObject *context,
               std::function<void(const QByteArray &data, const QString &error)> done) override;

    // For tests: how names are resolved (the system's, through Qt's host
    // lookup, unless given), a proxy every request goes through, and a
    // shorter time for the whole fetch.
    void setResolver(Resolver resolver);
    void setProxy(const QNetworkProxy &proxy);
    void setTimeout(int milliseconds);

    // The HTTP request for a checked address, with the origin's host and TLS identity.
    static QNetworkRequest pictureRequest(const QUrl &url, const QHostAddress &address);
    // Whether parch fetches from an address: false for loopback, private
    // (RFC 1918, fc00::/7), link-local, shared (100.64.0.0/10), multicast,
    // broadcast, reserved and unspecified addresses, and for IPv6 addresses
    // that carry such an IPv4 address.
    static bool isPublic(const QHostAddress &address);
    // Why a redirect from one address to another is not followed, or
    // nothing when it is.
    static QString redirectRefusal(const QUrl &from, const QUrl &to);
    // Whether a Content-Type names a picture parch takes from the web.
    static bool isPictureType(const QByteArray &contentType);

private:
    struct Job;
    // Resolves the address's host and fetches it once every address is public.
    void visit(const std::shared_ptr<Job> &job, const QUrl &url);
    void get(const std::shared_ptr<Job> &job, const QUrl &url, QList<QHostAddress> addresses);
    void finish(const std::shared_ptr<Job> &job, const QByteArray &data, const QString &error);

    QNetworkAccessManager m_network;
    Resolver m_resolver;
    int m_timeoutMs = TimeoutMs;
};
