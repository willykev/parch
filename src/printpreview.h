#pragma once

#include "dialectchoice.h"
#include "renderservice.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

class Document;

// The pages of a PDF or a print before it is made: the document laid out
// on paper exactly as the PDF exporter lays it out (PaperDocument), with
// the same options, and its pages drawn as pictures one at a time as a view
// asks for them. Laying out and drawing happen on a thread of the preview's
// own, never the UI's; a change of options lays the pages out again a
// moment after the last one, and pictures of the old pages are dropped.
class PrintPreview : public QObject {
    Q_OBJECT
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    // A page's height over its width.
    Q_PROPERTY(qreal pageAspect READ pageAspect NOTIFY changed)
    // The pages are being laid out.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Changes with every new layout.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit PrintPreview(QObject *parent = nullptr);
    ~PrintPreview() override;

    // Where the helpers are; as Exports::setRenderOptions().
    void setRenderOptions(const RenderService::Options &options);
    void setPlaces(std::function<std::pair<QString, QString>(Document *)> places);

    int pageCount() const { return m_pageCount; }
    qreal pageAspect() const { return m_aspect; }
    bool busy() const { return m_busy; }
    int revision() const { return m_revision; }

    // Lays the document out as a PDF ("pdf") or a print ("print") with
    // these options would be, a moment after the last call.
    Q_INVOKABLE void show(Document *document, const QString &mode, const QVariantMap &options);
    // Lets go of the pages.
    Q_INVOKABLE void stop();
    // Asks for a page, from 0, drawn `width` device pixels wide; pageReady()
    // says when it is.
    Q_INVOKABLE void requestPage(int page, int width);
    // The page as far as it has been drawn; null until it has.
    QImage page(int page) const;

    // How long show() waits for the options to settle.
    static constexpr int DelayMs = 150;

signals:
    void changed();
    void pageReady(int page);

private:
    struct State;
    struct Request {
        QPointer<Document> document;
        QString mode;
        QVariantMap options;
    };
    void layOut();
    void release();

    RenderService::Options m_render;
    std::function<std::pair<QString, QString>(Document *)> m_places;
    QThreadPool m_pool;
    QTimer m_delay;
    Request m_request;
    std::shared_ptr<State> m_state;
    std::shared_ptr<std::atomic<quint64>> m_generation;
    int m_pageCount = 0;
    qreal m_aspect = 1.414;
    bool m_busy = false;
    int m_revision = 0;
    // Pictures of pages, by page, and the width each was asked at.
    QHash<int, QImage> m_pages;
    QHash<int, int> m_asked;
};

// One page of a PrintPreview, as a view shows it: the page's picture at the
// item's size, asked for again as the item's size changes.
class PreviewPage : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(PrintPreview *preview READ preview WRITE setPreview NOTIFY previewChanged)
    Q_PROPERTY(int page READ page WRITE setPage NOTIFY pageChanged)
    // The page's picture has been drawn.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit PreviewPage(QQuickItem *parent = nullptr);

    PrintPreview *preview() const { return m_preview; }
    void setPreview(PrintPreview *preview);
    int page() const { return m_page; }
    void setPage(int page);
    bool ready() const { return m_ready; }

    void paint(QPainter *painter) override;

signals:
    void previewChanged();
    void pageChanged();
    void readyChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void ask();
    void arrived(int page);

    QPointer<PrintPreview> m_preview;
    int m_page = 0;
    bool m_ready = false;
};
