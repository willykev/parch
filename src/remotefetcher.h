#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

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

// Fetches over the network: http and https only, redirects followed while
// they stay no less safe, at most MaxBytes of answer and TimeoutMs of waiting.
class NetworkFetcher : public QObject, public RemoteFetcher {
    Q_OBJECT

public:
    static constexpr qint64 MaxBytes = qint64(32) << 20;
    static constexpr int TimeoutMs = 30000;

    explicit NetworkFetcher(QObject *parent = nullptr);
    void fetch(const QUrl &url, QObject *context,
               std::function<void(const QByteArray &data, const QString &error)> done) override;

private:
    QNetworkAccessManager m_network;
};
