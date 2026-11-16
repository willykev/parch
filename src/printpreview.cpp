#include "printpreview.h"

#include "codehighlight.h"
#include "document.h"
#include "documentreading.h"
#include "exporter.h"
#include "exports.h"
#include "mdinclude.h"
#include "pdfexport.h"
#include "sourcebuffer.h"

#include <QFileInfo>
#include <QPainter>
#include <QQuickWindow>
#include <QtConcurrent>

// What one layout keeps: the reading, and the pages set from it, which live
// and die on the preview's thread.
struct PrintPreview::State {
    // The document's parse, and its citations as its style formats them,
    // which point into it.
    struct Read {
        ParsedTextPtr parse;
        DocumentCitations::Formatted formatted;
    };
    std::shared_ptr<const Read> read;
    std::unique_ptr<PdfExporter::Pages> pages;
};

namespace {

QString canonical(const QString &path)
{
    if (path.isEmpty()) return {};
    const QString found = QFileInfo(path).canonicalFilePath();
    return found.isEmpty() ? QFileInfo(path).absoluteFilePath() : found;
}

const PdfExporter *pdfExporter() { return static_cast<const PdfExporter *>(Exporters::find(QStringLiteral("pdf"))); }

} // namespace

PrintPreview::PrintPreview(QObject *parent)
    : QObject(parent), m_generation(std::make_shared<std::atomic<quint64>>(0))
{
    // One thread, kept: what it lays out it also draws and lets go of.
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(-1);
    m_delay.setSingleShot(true);
    m_delay.setInterval(DelayMs);
    connect(&m_delay, &QTimer::timeout, this, &PrintPreview::layOut);
}

PrintPreview::~PrintPreview()
{
    ++*m_generation;
    release();
    m_pool.waitForDone();
}

void PrintPreview::setRenderOptions(const RenderService::Options &options) { m_render = options; }

void PrintPreview::show(Document *document, const QString &mode, const QVariantMap &options)
{
    m_request = {document, mode, options};
    if (!m_busy) {
        m_busy = true;
        emit changed();
    }
    m_delay.start();
}

void PrintPreview::stop()
{
    m_delay.stop();
    ++*m_generation;
    m_request = {};
    release();
    m_pages.clear();
    m_asked.clear();
    m_pageCount = 0;
    m_busy = false;
    ++m_revision;
    emit changed();
}

void PrintPreview::release()
{
    // Its deleter takes it to the preview's thread.
    m_state.reset();
}

void PrintPreview::layOut()
{
    Document *document = m_request.document;
    if (!document) return;
    const quint64 generation = ++*m_generation;
    const auto generations = m_generation;
    QHash<QString, QString> options = Exports::recipeOptions(QStringLiteral("pdf"), m_request.options);
    // The print dialog chooses the paper; until then, the default.
    if (m_request.mode == u"print") {
        options.remove(QStringLiteral("page-size"));
        options.remove(QStringLiteral("landscape"));
    }
    const QString path = document->path();
    const RenderService::Options render = m_render;
    // The worker colours code only once the languages are loaded, here.
    CodeHighlight::prepare();
    // The document's parse of its text as it is now, which every view of it
    // shares; a document that closes first is laid out no more.
    connect(document, &QObject::destroyed, this, [this, generation] {
        if (generation != *m_generation || !m_busy) return;
        m_busy = false;
        emit changed();
    }, Qt::SingleShotConnection);
    document->parse()->whenParsed(this, [=, this](const ParsedTextPtr &parse) {
        if (*generations != generation) return;
        lay(parse, generation, options, path, render);
    });
}

void PrintPreview::lay(const ParsedTextPtr &parse, quint64 generation, const QHash<QString, QString> &options,
                       const QString &path, const RenderService::Options &render)
{
    const auto generations = m_generation;
    // The layout comes back as a plain pointer, so that only the preview
    // owns it, and whatever lets go of it last deletes it on this thread.
    QtConcurrent::run(&m_pool, [=]() -> State * {
        if (*generations != generation) return nullptr;
        auto state = std::make_unique<State>();
        const DocumentReading::Reading &reading = parse->reading;
        state->read = std::make_shared<const State::Read>(
            State::Read{parse, DocumentCitations::format(reading.document, reading.citations)});
        ExportRecipe recipe;
        recipe.source = canonical(path);
        recipe.format = QStringLiteral("pdf");
        recipe.options = options;
        recipe.dialect = reading.dialect;
        recipe.citations = std::shared_ptr<const DocumentCitations::Formatted>(state->read, &state->read->formatted);
        ExportRun run;
        run.render = render;
        run.cancelled = [generations, generation] { return *generations != generation; };
        ExportResult result;
        state->pages = pdfExporter()->setPages(reading.document, recipe, run, &result);
        if (!state->pages || *generations != generation) return nullptr;
        return state.release();
    }).then(this, [this, generation](State *made) {
        if (!made) {
            if (generation == *m_generation) {
                m_busy = false;
                emit changed();
            }
            return;
        }
        QThreadPool *pool = &m_pool;
        std::shared_ptr<State> state(made, [pool](State *gone) { QtConcurrent::run(pool, [gone] { delete gone; }); });
        // Superseded meanwhile: it goes at once.
        if (generation != *m_generation) return;
        release();
        m_state = std::move(state);
        m_pages.clear();
        m_asked.clear();
        m_pageCount = m_state ? m_state->pages->paper->pageCount() : 0;
        if (m_state) {
            const QSizeF size = m_state->pages->paper->pageSize();
            if (size.width() > 0) m_aspect = size.height() / size.width();
        }
        m_busy = false;
        ++m_revision;
        emit changed();
    });
}

void PrintPreview::requestPage(int page, int width)
{
    if (!m_state || page < 0 || page >= m_pageCount || width <= 0) return;
    width = std::min(width, 4096);
    if (m_asked.value(page) == width) {
        if (m_pages.contains(page)) emit pageReady(page);
        return;
    }
    m_asked.insert(page, width);
    const quint64 generation = *m_generation;
    QtConcurrent::run(&m_pool, [state = m_state, page, width]() {
        const QSizeF size = state->pages->paper->pageSize();
        const int height = std::max(1, qRound(width * size.height() / size.width()));
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        // Paper is white, whatever the desktop's theme.
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        const qreal scale = width / size.width();
        painter.scale(scale, scale);
        state->pages->paper->paint(painter, page);
        painter.end();
        return image;
    }).then(this, [this, page, width, generation](const QImage &image) {
        if (generation != *m_generation || m_asked.value(page) != width) return;
        m_pages.insert(page, image);
        // Only the pages near the one asked for are kept.
        while (m_pages.size() > 12) {
            int farthest = page;
            for (auto it = m_pages.cbegin(); it != m_pages.cend(); ++it)
                if (std::abs(it.key() - page) > std::abs(farthest - page)) farthest = it.key();
            m_pages.remove(farthest);
            m_asked.remove(farthest);
        }
        emit pageReady(page);
    });
}

QImage PrintPreview::page(int page) const { return m_pages.value(page); }

PreviewPage::PreviewPage(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    // A page is drawn once it can be seen.
    connect(this, &QQuickItem::visibleChanged, this, &PreviewPage::ask);
}

void PreviewPage::setPreview(PrintPreview *preview)
{
    if (m_preview == preview) return;
    if (m_preview) disconnect(m_preview, nullptr, this, nullptr);
    m_preview = preview;
    if (m_preview) {
        connect(m_preview, &PrintPreview::pageReady, this, &PreviewPage::arrived);
        connect(m_preview, &PrintPreview::changed, this, [this] {
            if (m_ready) {
                m_ready = false;
                emit readyChanged();
            }
            ask();
            update();
        });
    }
    emit previewChanged();
    ask();
}

void PreviewPage::setPage(int page)
{
    if (m_page == page) return;
    m_page = page;
    if (m_ready) {
        m_ready = false;
        emit readyChanged();
    }
    emit pageChanged();
    ask();
    update();
}

void PreviewPage::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) ask();
}

void PreviewPage::ask()
{
    if (!m_preview || width() < 1 || !isVisible()) return;
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1;
    m_preview->requestPage(m_page, qRound(width() * ratio));
}

void PreviewPage::arrived(int page)
{
    if (page != m_page) return;
    if (!m_ready) {
        m_ready = true;
        emit readyChanged();
    }
    update();
}

void PreviewPage::paint(QPainter *painter)
{
    if (!m_preview) return;
    const QImage image = m_preview->page(m_page);
    if (image.isNull()) return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(boundingRect(), image);
}
