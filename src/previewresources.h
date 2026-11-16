#pragma once

#include "previewbuilder.h"
#include "renderrequest.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

class RemoteFetcher;
class RenderService;

// The pictures of one preview: formulas and diagrams drawn by the render
// service in the preview's colours, and the images its document names,
// read from disk on a worker or fetched from the network. The builder asks
// for a picture and gets it as far as it has come; when a picture arrives,
// fails, or has been drawn again in other colours, changed() names it, and
// the preview builds the sections that show it again.
//
// A picture drawn in colours that are no longer the preview's stays as it
// is, and is shown, until the one in the new colours has arrived.
class PreviewResources : public QObject, public preview::Resources {
    Q_OBJECT

public:
    // No picture is read larger than this on its longer side.
    static constexpr int MaxSide = 4096;
    static constexpr qsizetype MaxInlineBytes = qsizetype(16) << 20;

    explicit PreviewResources(QObject *parent = nullptr);
    ~PreviewResources() override;

    // Without a service, formulas and diagrams stay their source.
    void setRenderService(RenderService *service);
    // Without a fetcher, or when not allowed, images from the network are not shown.
    void setFetcher(RemoteFetcher *fetcher);
    void setRemoteAllowed(bool allowed);
    bool remoteAllowed() const { return m_remoteAllowed; }
    // What formulas and diagrams are drawn with. A change draws them again.
    void setColors(const RenderColors &colors);
    void setScale(qreal scale);
    qreal scale() const { return m_scale; }
    void setLabelFamily(const QString &family);

    preview::Picture math(const QString &tex, bool display, qreal fontSize) override;
    preview::Picture diagram(const QString &source) override;
    preview::Picture image(const md::Destination &destination, const QString &written) override;

    // Forgets every picture but those with these keys.
    void keepOnly(const QSet<QString> &keys);
    void clear();
    // How many pictures are still being drawn, read or fetched.
    int pending() const;

signals:
    void changed(const QString &key);

private:
    struct Entry {
        preview::Picture picture;
        // What it was drawn with; another value asks for it again.
        QString drawnWith;
        QString askedWith;
        quint64 ticket = 0;
        bool waiting = false;
    };
    preview::Picture drawn(const QString &key, const RenderRequest &request);
    void rendered(quint64 ticket, const RenderResult &result);
    void loaded(const QString &key, quint64 generation, const QImage &image, const QString &error);
    QString renderIdentity(qreal fontSize) const;

    RenderService *m_service = nullptr;
    RemoteFetcher *m_fetcher = nullptr;
    bool m_remoteAllowed = true;
    RenderColors m_colors;
    qreal m_scale = 1;
    QString m_labelFamily;
    QHash<QString, Entry> m_entries;
    QHash<quint64, QString> m_tickets;
    // Results of what was asked before clear() are not taken.
    quint64 m_generation = 0;
    QThreadPool m_pool;
};
