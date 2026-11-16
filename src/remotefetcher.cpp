#include "remotefetcher.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

NetworkFetcher::NetworkFetcher(QObject *parent) : QObject(parent)
{
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

void NetworkFetcher::fetch(const QUrl &url, QObject *context,
                           std::function<void(const QByteArray &data, const QString &error)> done)
{
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
        done({}, tr("Only http and https addresses are fetched."));
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(TimeoutMs);
    // A generic name: nothing of the document or its writer goes with the request.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("parch"));
    QNetworkReply *reply = m_network.get(request);
    const QPointer<QObject> alive(context);
    // An answer larger than any picture parch would show is given up.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > MaxBytes || total > MaxBytes) {
            reply->setProperty("parchTooLarge", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [reply, alive, done = std::move(done)] {
        reply->deleteLater();
        if (!alive) return;
        if (reply->property("parchTooLarge").toBool()) done({}, tr("The picture is too large."));
        else if (reply->error() != QNetworkReply::NoError) done({}, reply->errorString());
        else done(reply->readAll(), {});
    });
}
