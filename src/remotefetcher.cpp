#include "remotefetcher.h"

#include <QHostInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <span>

struct NetworkFetcher::Job {
    QPointer<QObject> context;
    std::function<void(const QByteArray &, const QString &)> done;
    QPointer<QNetworkReply> reply;
    // The time the whole fetch has, redirects and all.
    QTimer deadline;
    int redirects = 0;
    bool finished = false;
};

NetworkFetcher::NetworkFetcher(QObject *parent) : QObject(parent)
{
    m_network.setProxy(QNetworkProxy::NoProxy);
}

NetworkFetcher::~NetworkFetcher() = default;

void NetworkFetcher::setResolver(Resolver resolver) { m_resolver = std::move(resolver); }
void NetworkFetcher::setProxy(const QNetworkProxy &proxy) { m_network.setProxy(proxy); }
void NetworkFetcher::setTimeout(int milliseconds) { m_timeoutMs = milliseconds; }

void NetworkFetcher::fetch(const QUrl &url, QObject *context,
                           std::function<void(const QByteArray &data, const QString &error)> done)
{
    auto job = std::make_shared<Job>();
    job->context = context;
    job->done = std::move(done);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")) || url.host().isEmpty()) {
        finish(job, {}, tr("Only http and https addresses are fetched."));
        return;
    }
    job->deadline.setSingleShot(true);
    connect(&job->deadline, &QTimer::timeout, this, [this, weak = std::weak_ptr<Job>(job)] {
        if (const std::shared_ptr<Job> late = weak.lock()) finish(late, {}, tr("The picture took too long to arrive."));
    });
    job->deadline.start(m_timeoutMs);
    visit(job, url);
}

void NetworkFetcher::visit(const std::shared_ptr<Job> &job, const QUrl &url)
{
    const auto check = [this, job, url](const QList<QHostAddress> &addresses, const QString &error) {
        if (job->finished) return;
        if (addresses.isEmpty()) {
            finish(job, {}, error.isEmpty() ? tr("Its host was not found.") : error);
            return;
        }
        // Every address the name has, since any of them may be the one connected to.
        for (const QHostAddress &address : addresses) {
            if (!isPublic(address)) {
                finish(job, {}, tr("Its address is on this computer or a private network, which parch never fetches from."));
                return;
            }
        }
        get(job, url, addresses);
    };
    const QString host = url.host();
    if (QHostAddress literal; literal.setAddress(host)) {
        check({literal}, {});
        return;
    }
    if (m_resolver) {
        m_resolver(host, check);
        return;
    }
    QHostInfo::lookupHost(host, this, [check](const QHostInfo &info) {
        check(info.error() == QHostInfo::NoError ? info.addresses() : QList<QHostAddress>{}, info.errorString());
    });
}

QNetworkRequest NetworkFetcher::pictureRequest(const QUrl &url, const QHostAddress &address)
{
    QUrl endpoint = url.adjusted(QUrl::RemoveUserInfo);
    endpoint.setHost(address.toString());
    QNetworkRequest request(endpoint);
    request.setRawHeader("Host", url.adjusted(QUrl::RemoveUserInfo).authority(QUrl::FullyEncoded).toLatin1());
    // Qt uses this name for TLS and in its connection-pool key.
    request.setPeerVerifyName(url.host());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    // Nothing is remembered from one picture to the next.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    // A generic name: nothing of the document or its writer goes with the request.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("parch"));
    request.setRawHeader("Accept", "image/png, image/jpeg, image/gif, image/webp, image/svg+xml");
    return request;
}

void NetworkFetcher::get(const std::shared_ptr<Job> &job, const QUrl &url, QList<QHostAddress> addresses)
{
    QNetworkRequest request = pictureRequest(url, addresses.takeFirst());
    request.setTransferTimeout(m_timeoutMs);
    QNetworkReply *reply = m_network.get(request);
    job->reply = reply;
    const auto typeOf = [](QNetworkReply *reply) { return reply->rawHeader("Content-Type"); };
    const auto notAPicture = [](const QByteArray &type) {
        return type.isEmpty() ? tr("It is not a picture: the server did not say what it is.")
                              : tr("It is not a picture: the server sent %1.").arg(QString::fromLatin1(type.left(80)));
    };
    // What is not a picture is given up as soon as the server says so.
    connect(reply, &QNetworkReply::metaDataChanged, reply, [this, job, reply, typeOf, notAPicture] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 200 && status < 300 && !isPictureType(typeOf(reply))) finish(job, {}, notAPicture(typeOf(reply)));
    });
    // An answer larger than any picture parch would show is given up.
    connect(reply, &QNetworkReply::downloadProgress, reply, [this, job](qint64 received, qint64 total) {
        if (received > MaxBytes || total > MaxBytes) finish(job, {}, tr("The picture is too large."));
    });
    connect(reply, &QNetworkReply::finished, this, [this, job, reply, url, addresses, typeOf, notAPicture] {
        if (job->finished) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray location = reply->rawHeader("Location");
        if (status >= 300 && status < 400 && !location.isEmpty()) {
            job->reply = nullptr;
            reply->deleteLater();
            const QUrl next = url.resolved(QUrl::fromEncoded(location));
            if (const QString why = redirectRefusal(url, next); !why.isEmpty()) {
                finish(job, {}, why);
            } else if (++job->redirects > MaxRedirects) {
                finish(job, {}, tr("It was sent on too many times."));
            } else {
                visit(job, next);
            }
            return;
        }
        if (status == 0 && !addresses.isEmpty() && (reply->error() == QNetworkReply::ConnectionRefusedError
            || reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::HostNotFoundError
            || reply->error() == QNetworkReply::NetworkSessionFailedError)) {
            reply->deleteLater();
            get(job, url, addresses);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) finish(job, {}, reply->errorString());
        else if (!isPictureType(typeOf(reply))) finish(job, {}, notAPicture(typeOf(reply)));
        else finish(job, reply->readAll(), {});
    });
}

void NetworkFetcher::finish(const std::shared_ptr<Job> &job, const QByteArray &data, const QString &error)
{
    if (job->finished) return;
    job->finished = true;
    job->deadline.stop();
    if (QNetworkReply *reply = job->reply) {
        job->reply = nullptr;
        // Its finished() comes back here, and finds the job finished.
        reply->abort();
        reply->deleteLater();
    }
    const auto done = std::exchange(job->done, nullptr);
    if (job->context && done) done(data, error);
}

bool NetworkFetcher::isPublic(const QHostAddress &given)
{
    QHostAddress address = given;
    if (address.isNull()) return false;
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR six = address.toIPv6Address();
        const auto zero = [&six](int from, int to) {
            for (int i = from; i < to; ++i)
                if (six[i] != 0) return false;
            return true;
        };
        // IPv4-mapped (::ffff:0:0/96), IPv4-compatible (::/96) and NAT64
        // (64:ff9b::/96) addresses lead to the IPv4 address they end with.
        const bool mapped = zero(0, 10) && six[10] == 0xff && six[11] == 0xff;
        const bool compatible = zero(0, 12);
        const bool translated = six[0] == 0 && six[1] == 0x64 && six[2] == 0xff && six[3] == 0x9b && zero(4, 12);
        if (compatible && zero(12, 15) && six[15] <= 1) return false;
        if (mapped || compatible || translated)
            address = QHostAddress(quint32(six[12]) << 24 | quint32(six[13]) << 16 | quint32(six[14]) << 8 | quint32(six[15]));
    }
    struct Net {
        const char *address;
        int bits;
    };
    static const Net closed4[] = {
        {"0.0.0.0", 8},     {"10.0.0.0", 8},     {"100.64.0.0", 10}, {"127.0.0.0", 8},  {"169.254.0.0", 16}, {"172.16.0.0", 12},
        {"192.0.0.0", 24},  {"192.168.0.0", 16}, {"198.18.0.0", 15}, {"224.0.0.0", 4},  {"240.0.0.0", 4},
    };
    static const Net closed6[] = {
        {"::", 128}, {"::1", 128}, {"100::", 64}, {"fc00::", 7}, {"fe80::", 10}, {"fec0::", 10}, {"ff00::", 8},
    };
    const bool four = address.protocol() == QAbstractSocket::IPv4Protocol;
    for (const Net &net : four ? std::span<const Net>(closed4) : std::span<const Net>(closed6))
        if (address.isInSubnet(QHostAddress(QString::fromLatin1(net.address)), net.bits)) return false;
    return true;
}

QString NetworkFetcher::redirectRefusal(const QUrl &from, const QUrl &to)
{
    const QString scheme = to.scheme().toLower();
    if (!to.isValid() || to.host().isEmpty() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
        return tr("It was sent on to an address that is not a web address.");
    if (from.scheme().toLower() == QLatin1String("https") && scheme == QLatin1String("http"))
        return tr("It was sent on from a secure address to one that is not.");
    return {};
}

bool NetworkFetcher::isPictureType(const QByteArray &contentType)
{
    const QByteArray type = contentType.split(';').value(0).trimmed().toLower();
    static const QList<QByteArray> pictures{"image/png", "image/jpeg", "image/jpg", "image/pjpeg",
                                            "image/gif", "image/webp", "image/svg+xml"};
    return pictures.contains(type);
}
