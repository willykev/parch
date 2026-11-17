#include "screenshot.h"

#include "backend.h"
#include "document.h"
#include "editorbinding.h"
#include "exporter.h"
#include "exports.h"
#include "preview.h"
#include "previewresources.h"
#include "printpreview.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "spellchecker.h"
#include "tabmodel.h"
#include "windowcontext.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QQuickItem>
#include <QQuickWindow>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

QQuickItem *findNamed(QQuickItem *root, const QString &name)
{
    if (!root) return nullptr;
    for (QQuickItem *child : root->childItems()) {
        if (child->objectName() == name) return child;
        if (QQuickItem *found = findNamed(child, name)) return found;
    }
    return nullptr;
}

void everyNamed(QQuickItem *root, const QString &name, QList<QQuickItem *> &found)
{
    if (!root) return;
    for (QQuickItem *child : root->childItems()) {
        if (child->objectName() == name) found.append(child);
        everyNamed(child, name, found);
    }
}

// Where line `line`, counted from 1, starts in the text; the start of the
// last line for a line past the end.
int lineStart(const QString &text, int line)
{
    int at = 0;
    for (int i = 1; i < line; ++i) {
        const qsizetype next = text.indexOf(u'\n', at);
        if (next < 0 || next + 1 >= text.size()) break;
        at = int(next + 1);
    }
    return at;
}

// The command each panel is opened with, and what shows once it is open.
struct Panel {
    const char *command;
    const char *shown;
};

Panel panelFor(const QString &name)
{
    if (name == u"export") return {"export", "exportDialog"};
    if (name == u"print") return {"print", "exportDialog"};
    if (name == u"document-settings") return {"documentSettings", "documentSettingsDialog"};
    if (name == u"history") return {"history", "historyDialog"};
    if (name == u"palette") return {"palette", "commandPalette"};
    if (name == u"preferences") return {"preferences", "preferencesDialog"};
    if (name == u"shortcuts") return {"shortcuts", "shortcutsDialog"};
    return {"outline", nullptr};
}

} // namespace

Screenshot::Screenshot(const Picture &picture, Backend *backend, int waitMs, QObject *parent)
    : QObject(parent), m_picture(picture), m_backend(backend), m_waitMs(waitMs)
{
    m_poll.setInterval(PollMs);
    connect(&m_poll, &QTimer::timeout, this, &Screenshot::poll);
}

void Screenshot::start(QQuickWindow *window)
{
    m_window = window;
    m_clock.start();
    m_poll.start();
    leave();
}

void Screenshot::leave()
{
    // Off screen the pointer starts inside the window, over the header's
    // first button, whose tooltip would then show.
    if (!m_window) return;
    QEvent left(QEvent::Leave);
    QCoreApplication::sendEvent(m_window, &left);
}

QQuickItem *Screenshot::item(const QString &name) const
{
    return m_window ? findNamed(m_window->contentItem(), name) : nullptr;
}

QObject *Screenshot::object(const QString &name) const
{
    return m_window ? m_window->findChild<QObject *>(name) : nullptr;
}

QStringList Screenshot::pending() const
{
    if (!m_window) return {tr("the window")};
    QStringList waiting;
    auto *document = qobject_cast<Document *>(m_window->property("document").value<QObject *>());
    QQuickItem *editorPane = item(QStringLiteral("editorPane"));
    if (document && editorPane) {
        auto *binding = editorPane->findChild<EditorBinding *>(QStringLiteral("binding"));
        auto *editor = editorPane->findChild<QQuickItem *>(QStringLiteral("editor"));
        const int expected = binding && binding->document() == document
                                 ? binding->sourceToEditor(document->buffer()->length()) : -1;
        if (!editor || expected < 0 || editor->property("length").toInt() != expected) {
            waiting.append(tr("the text"));
        } else if (m_window->property("editorShown").toBool()) {
            QObject *styling = editorPane->findChild<QObject *>(QStringLiteral("styling"));
            if (styling && styling->property("busy").toBool()) waiting.append(tr("live formatting"));
            SpellChecker *spelling = m_backend ? m_backend->spelling() : nullptr;
            QObject *layer = editorPane->findChild<QObject *>(QStringLiteral("spelling"));
            if ((spelling && spelling->wanted() && !spelling->settled()) || (layer && layer->property("checking").toBool()))
                waiting.append(tr("spelling"));
            // The text has the keys and shows its caret, unless a panel has them.
            if (m_step >= Step::Placing && m_picture.show.isEmpty() && !editor->hasActiveFocus())
                waiting.append(tr("the caret"));
        }
    }
    QQuickItem *previewPane = item(QStringLiteral("previewPane"));
    if (previewPane && previewPane->isVisible()) {
        auto *preview = previewPane->findChild<Preview *>(QStringLiteral("preview"));
        if (preview && (!preview->current() || preview->working() || preview->parsing()
                        || (preview->resources() && preview->resources()->pending() > 0)))
            waiting.append(tr("the preview"));
    }
    QObject *exportDialog = object(QStringLiteral("exportDialog"));
    if (exportDialog && exportDialog->property("visible").toBool() && m_backend) {
        Exports *exports = m_backend->exports();
        if (exports->reading()) waiting.append(tr("what the export leaves out"));
        if (exportDialog->property("paged").toBool()) {
            PrintPreview *pages = exports->preview();
            bool drawn = pages && !pages->busy() && pages->pageCount() > 0;
            QList<QQuickItem *> shown;
            everyNamed(item(QStringLiteral("previewPages")), QStringLiteral("previewPage"), shown);
            for (QQuickItem *page : std::as_const(shown))
                drawn = drawn && (!page->isVisible() || page->property("ready").toBool());
            if (!drawn) waiting.append(tr("the pages"));
        }
    }
    QObject *history = object(QStringLiteral("historyDialog"));
    if (history && history->property("visible").toBool() && history->property("loading").toBool())
        waiting.append(tr("the history"));
    return waiting;
}

void Screenshot::poll()
{
    if (m_done) return;
    if (!m_window) {
        finish(4, tr("the window closed before it was drawn"));
        return;
    }
    const QStringList waiting = pending();
    if (m_clock.elapsed() > m_waitMs) {
        const int seconds = std::max(1, m_waitMs / 1000);
        const QString within = seconds == 1 ? tr("1 second") : tr("%1 seconds").arg(seconds);
        finish(4, waiting.isEmpty() ? tr("the window was not drawn within %1: it kept changing").arg(within)
                                    : tr("the window was not drawn within %1: still drawing %2")
                                          .arg(within, waiting.join(QStringLiteral(", "))));
        return;
    }
    if (!waiting.isEmpty()) {
        m_quiet = 0;
        m_last = QImage();
        return;
    }
    // Each step waits for a moment in which nothing is left to draw, so
    // that what the step before it asked for has come.
    if (++m_quiet < 2) return;
    m_quiet = 0;
    switch (m_step) {
    case Step::Loading:
        arrange();
        m_step = Step::Arranging;
        return;
    case Step::Arranging:
        place();
        m_step = Step::Placing;
        return;
    case Step::Placing:
        if (!m_picture.show.isEmpty() && !open()) {
            finish(4, tr("--show %1 could not be opened in this window").arg(m_picture.show));
            return;
        }
        m_step = Step::Opening;
        return;
    case Step::Opening:
    case Step::Settling:
        break;
    }
    m_step = Step::Settling;
    // Where the text was put stays put, whatever came in late.
    place();
    frameExport();
    hideClippedShapes();
    const QImage now = m_window->grabWindow();
    if (now.isNull()) {
        finish(4, tr("the window could not be drawn"));
        return;
    }
    if (m_last.isNull() || now != m_last) {
        m_last = now;
        return;
    }
    QBuffer encoded;
    encoded.open(QIODevice::WriteOnly);
    QImageWriter writer(&encoded, {});
    const QByteArray suffix = QFileInfo(m_picture.file).suffix().toLower().toLatin1();
    writer.setFormat(QImageWriter::supportedImageFormats().contains(suffix) ? suffix : QByteArray("png"));
    if (!writer.write(m_last)) {
        finish(3, tr("cannot write the picture to %1 (%2)").arg(m_picture.file, writer.errorString()));
        return;
    }
    QString error;
    if (!Exporters::writeFile(m_picture.file, encoded.data(), &error)) {
        finish(3, tr("cannot write the picture (%1)").arg(error));
        return;
    }
    finish(0, m_picture.file);
}

void Screenshot::arrange()
{
    auto *context = qobject_cast<WindowContext *>(m_window->property("context").value<QObject *>());
    if (!m_picture.view.isEmpty()) {
        const int mode = m_picture.view == u"split"     ? TabModel::Split
                         : m_picture.view == u"reading" ? TabModel::Reading
                                                         : TabModel::Source;
        QMetaObject::invokeMethod(m_window, "setViewMode", Q_ARG(QVariant, mode));
    }
    if (m_picture.library && context) {
        context->setLibraryOpen(*m_picture.library);
        // The library folder that holds the document is open on it, the
        // nearest of them where several do.
        auto *document = qobject_cast<Document *>(m_window->property("document").value<QObject *>());
        const QString path = document ? document->path() : QString();
        QString holder;
        if (*m_picture.library && !path.isEmpty() && m_backend)
            for (const QString &folder : m_backend->settings()->libraryFolders())
                if (path.startsWith(QDir(folder).absolutePath() + u'/') && folder.size() > holder.size()) holder = folder;
        if (!holder.isEmpty()) context->showFolder(holder, false);
    }
    if (m_picture.focus) m_window->setProperty("focusMode", true);
    m_window->requestActivate();
    QMetaObject::invokeMethod(m_window, "focusText");
    leave();
}

void Screenshot::place()
{
    const bool selecting = m_picture.selectFrom >= 0;
    if (m_picture.line <= 0 && m_picture.scroll < 0 && !selecting) return;
    auto *document = qobject_cast<Document *>(m_window->property("document").value<QObject *>());
    QQuickItem *editorPane = item(QStringLiteral("editorPane"));
    QQuickItem *previewPane = item(QStringLiteral("previewPane"));
    auto *binding = editorPane ? editorPane->findChild<EditorBinding *>(QStringLiteral("binding")) : nullptr;
    if (!document || !binding) return;
    const bool editorShown = m_window->property("editorShown").toBool();
    const bool previewShown = m_window->property("previewShown").toBool();
    const QString text = document->buffer()->text();
    const int length = int(text.size());
    int top = -1;
    if (m_picture.line > 0) {
        top = lineStart(text, m_picture.line);
    } else if (m_picture.scroll >= 0) {
        // That far down what the text, or else the preview, can scroll.
        QQuickItem *flickable = editorShown ? editorPane->findChild<QQuickItem *>(QStringLiteral("flickable"))
                                            : item(QStringLiteral("previewFlickable"));
        if (flickable) {
            const qreal most = std::max<qreal>(0, flickable->property("contentHeight").toReal() - flickable->height());
            flickable->setProperty("contentY", std::round(m_picture.scroll * most));
        }
        if (editorShown) top = binding->topSourcePosition();
    }
    const int anchor = selecting ? std::min(m_picture.selectFrom, length) : top >= 0 ? top : binding->anchorSource();
    const int cursor = selecting ? std::min(m_picture.selectTo, length) : top >= 0 ? top : binding->cursorSource();
    if (top >= 0)
        QMetaObject::invokeMethod(editorPane, "restoreView", Q_ARG(QVariant, cursor), Q_ARG(QVariant, anchor),
                                  Q_ARG(QVariant, top));
    else if (selecting)
        QMetaObject::invokeMethod(editorPane, "selectSource", Q_ARG(QVariant, anchor), Q_ARG(QVariant, cursor));
    if (top >= 0 && previewShown && previewPane)
        QMetaObject::invokeMethod(previewPane, "showSource", Q_ARG(QVariant, top));
}

bool Screenshot::open()
{
    const Panel panel = panelFor(m_picture.show);
    if (!panel.shown) {
        // The outline, unless the library already shows it: asked for again,
        // it would hide.
        const bool libraryShown = (item(QStringLiteral("library")) && item(QStringLiteral("library"))->isVisible())
                                  || (object(QStringLiteral("libraryDrawer"))
                                      && object(QStringLiteral("libraryDrawer"))->property("visible").toBool());
        if (!(m_backend && m_backend->settings()->outlineShown() && libraryShown))
            QMetaObject::invokeMethod(m_window, "run", Q_ARG(QVariant, QString::fromLatin1(panel.command)));
        return m_backend && m_backend->settings()->outlineShown();
    }
    QMetaObject::invokeMethod(m_window, "run", Q_ARG(QVariant, QString::fromLatin1(panel.command)));
    QObject *shown = object(QString::fromLatin1(panel.shown));
    // Export opens on the format last used; the picture is of a PDF's.
    if (shown && m_picture.show == u"export") shown->setProperty("format", QStringLiteral("pdf"));
    return shown && shown->property("visible").toBool();
}

void Screenshot::frameExport()
{
    QObject *dialog = object(QStringLiteral("exportDialog"));
    if (!dialog || !dialog->property("visible").toBool()) return;
    QQuickItem *body = item(QStringLiteral("exportBody"));
    QQuickItem *limitations = item(QStringLiteral("exportLimitations"));
    // The place the pages keep after the options when they are not beside them.
    QQuickItem *pages = item(QStringLiteral("exportPagesBelow"));
    auto *content = body ? body->property("contentItem").value<QQuickItem *>() : nullptr;
    if (!content || !limitations) return;
    // Beside the pages, the format and its options show from the top.
    if (dialog->property("sideBySide").toBool()) return;
    // After them, in a narrow window, what the format leaves out at the
    // top, and the pages below it as far as they fit.
    const qreal from = limitations->mapToItem(content, QPointF(0, 0)).y();
    qreal to = from + limitations->height();
    if (pages && pages->isVisible()) to = pages->mapToItem(content, QPointF(0, pages->height())).y();
    const qreal most = std::max<qreal>(0, body->property("contentHeight").toReal() - body->height());
    const qreal y = std::clamp(std::min(from, to - body->height()), qreal(0), most);
    body->setProperty("contentY", std::round(y));
}

void Screenshot::hideClippedShapes()
{
    QList<QQuickItem *> shapes;
    const std::function<void(QQuickItem *)> collect = [&](QQuickItem *at) {
        for (QQuickItem *child : at->childItems()) {
            if (child->inherits("QQuickShape")) shapes.append(child);
            collect(child);
        }
    };
    collect(m_window->contentItem());
    for (QQuickItem *shape : std::as_const(shapes)) {
        const bool hidden = m_hidden.contains(shape);
        if (!hidden && !shape->isVisible()) continue;
        const QRectF drawn = shape->mapRectToScene(shape->boundingRect());
        bool outside = false;
        for (QQuickItem *at = shape->parentItem(); at && !outside; at = at->parentItem())
            outside = at->clip() && !drawn.intersects(at->mapRectToScene(at->clipRect()));
        if (outside && !hidden) {
            shape->setVisible(false);
            m_hidden.append(shape);
        } else if (!outside && hidden) {
            shape->setVisible(true);
            m_hidden.removeAll(shape);
        }
    }
}

void Screenshot::finish(int code, const QString &message)
{
    if (m_done) return;
    m_done = true;
    m_poll.stop();
    m_last = QImage();
    // poll() is the only caller, and its timer runs in the event loop.
    emit finished(code, message);
}
