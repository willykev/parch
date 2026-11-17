#include "preview.h"

#include "attachments.h"
#include "codehighlight.h"
#include "documentreading.h"
#include "mdinclude.h"
#include "mdparser_p.h"
#include "documentparse.h"
#include "outline.h"
#include "previewresources.h"
#include "previewwatch.h"
#include "renderrequest.h"
#include "sourcebuffer.h"
#include "workerfailure.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMimeData>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextLayout>
#include <QTextTable>
#include <QUrl>
#include <QThread>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

using preview::Anchor;
using preview::Island;
using preview::Mark;
using preview::Part;
using preview::Section;

// --- The marks ---------------------------------------------------------------------------

int PreviewMarks::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant PreviewMarks::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case KindRole: return row.kind;
    case RectRole: return row.rect;
    case Qt::DisplayRole:
    case TextRole: return row.text;
    case LevelRole: return row.level;
    case IslandRole: return row.island;
    case IslandKindRole: return row.islandKind;
    case WrapsRole: return row.wraps;
    case ContentWidthRole: return row.contentWidth;
    case ContentHeightRole: return row.contentHeight;
    case CenteredRole: return row.centered;
    case TintedRole: return row.tinted;
    case RulesRole: {
        QVariantList rules;
        for (const qreal rule : row.rules) rules << rule;
        return rules;
    }
    case HeaderRuleRole: return row.headerRule;
    case PositionRole: return row.position;
    default: return {};
    }
}

QHash<int, QByteArray> PreviewMarks::roleNames() const
{
    return {{KindRole, "kind"},
            {RectRole, "rect"},
            {TextRole, "label"},
            {LevelRole, "level"},
            {IslandRole, "island"},
            {IslandKindRole, "islandKind"},
            {WrapsRole, "wraps"},
            {ContentWidthRole, "contentWidth"},
            {ContentHeightRole, "contentHeight"},
            {CenteredRole, "centered"},
            {TintedRole, "tinted"},
            {RulesRole, "rules"},
            {HeaderRuleRole, "headerRule"},
            {PositionRole, "position"}};
}

void PreviewMarks::show(const QList<Row> &rows)
{
    QSet<quint64> staying, coming;
    for (const Row &row : rows) coming.insert(row.key);
    for (const Row &row : std::as_const(m_rows)) staying.insert(row.key);
    // The rows that go, from the end, so that the rows before keep their numbers.
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (coming.contains(m_rows.at(i).key)) continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }
    // What is left is in the order of the new rows, which name each row
    // that stays or comes.
    for (int i = 0; i < rows.size(); ++i) {
        const Row &row = rows.at(i);
        if (i < m_rows.size() && m_rows.at(i).key == row.key) {
            if (m_rows.at(i) == row) continue;
            m_rows[i] = row;
            emit dataChanged(index(i), index(i));
        } else if (staying.contains(row.key) && i < m_rows.size()) {
            // A row that stays but is not where it was: it is put where it belongs.
            int from = i + 1;
            while (from < m_rows.size() && m_rows.at(from).key != row.key) ++from;
            if (from < m_rows.size()) {
                beginRemoveRows({}, from, from);
                m_rows.removeAt(from);
                endRemoveRows();
            }
            beginInsertRows({}, i, i);
            m_rows.insert(i, row);
            endInsertRows();
        } else {
            beginInsertRows({}, i, i);
            m_rows.insert(i, row);
            endInsertRows();
        }
    }
}

// --- The preview --------------------------------------------------------------------------

struct Preview::Parsed {
    Revision revision = 0;
    quint64 generation = 0;
    std::shared_ptr<const md::Document> tree;
    // The tree's citations as its citation style formats them.
    std::shared_ptr<const DocumentCitations::Formatted> citations;
    std::shared_ptr<const std::vector<Part>> parts;
    // The files on disk the text depends on: its local pictures, and the
    // files it was read with (included files, bibliographies, a style file).
    QStringList pictures, files;
    // What the preview marks as leading nowhere.
    QStringList unresolved;
    // It names pictures on the web.
    bool webPictures = false;
};

// Sections that go and the parts that are built in their place.
struct Preview::Operation {
    size_t at = 0;
    size_t remove = 0;
    std::vector<Part> parts;
    size_t done = 0;
};

struct Preview::IslandState {
    QPointer<QTextDocument> document;
    QPointer<QQuickItem> view;
    // A table's width when none of its cells wraps; -1 until it is measured.
    qreal natural = -1;
    int columns = 0;
    // The island's text as laid out, and its height with what surrounds it.
    QSizeF size;
    qreal height = 1;
    QList<qreal> rules;
    int headerRule = -1;
};

struct Preview::TopAnchor {
    bool valid = false;
    QTextCursor cursor;
    qreal offset = 0;
};

namespace {

quint64 islandId(quint64 section, size_t index) { return (section << 20) | quint64(index + 1); }

QTextDocument *documentOf(QQuickItem *textEdit)
{
    if (!textEdit) return nullptr;
    auto *quick = textEdit->property("textDocument").value<QQuickTextDocument *>();
    return quick ? quick->textDocument() : nullptr;
}

QTextTable *tableOf(QTextDocument *document)
{
    for (QTextFrame *frame : document->rootFrame()->childFrames())
        if (auto *table = qobject_cast<QTextTable *>(frame)) return table;
    return nullptr;
}

// Text with every character that cannot be seen, or that would reorder
// what is around it, written as its code point.
QString visibleText(const QString &text)
{
    QString shown;
    shown.reserve(text.size());
    for (const QChar c : text) {
        const QChar::Category category = c.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format || category == QChar::Separator_Line
            || category == QChar::Separator_Paragraph)
            shown += QStringLiteral("<U+%1>").arg(uint(c.unicode()), 4, 16, QLatin1Char(u'0')).toUpper();
        else
            shown += c;
    }
    return shown;
}

bool readsAsMarkdown(const QString &path)
{
    static const QStringList suffixes{QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("mdown"),
                                      QStringLiteral("mkd"), QStringLiteral("txt"), QStringLiteral("text")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

// The local pictures a tree shows, by the paths the builder reads them
// from: Markdown images and HTML `img` elements, in every file it includes;
// and whether it names any on the web.
void picturesOf(const md::Document &document, const md::Node &node, QStringList &out, bool &web)
{
    QString written;
    if (node.type == md::NodeType::Image) {
        written = node.dest;
    } else if (node.type == md::NodeType::HtmlElement && node.label == QLatin1StringView("img")) {
        for (const md::Attribute &attribute : node.attributes)
            if (attribute.name == QLatin1StringView("src")) written = attribute.value;
    }
    if (!written.isEmpty() && md::detail::isSafeUrl(written, true)) {
        const int file = std::clamp(node.file, 0, int(document.files.size()) - 1);
        const md::Destination target = md::classifyDestination(written, document.files[size_t(file)].directory);
        if (target.kind == md::Destination::Kind::LocalFile && target.absolute) out.append(target.path);
        else if (target.kind == md::Destination::Kind::Remote) web = true;
    }
    for (const md::Node &child : node.children) picturesOf(document, child, out, web);
}

// The files a reading read besides the text: what it includes, its
// bibliographies, its own or its chapters', and a style from a file.
QStringList filesRead(const DocumentReading::Reading &reading)
{
    QStringList files;
    for (size_t i = 1; i < reading.document.files.size(); ++i) files.append(reading.document.files[i].path);
    files.append(reading.dialect.bibliography);
    for (const QList<DocumentCitations::Database> &databases : reading.citations.fileDatabases)
        for (const DocumentCitations::Database &database : databases)
            if (database) files.append(database->identity());
    if (Dialect::isStyleFile(reading.dialect.citationStyle)) files.append(reading.dialect.citationStyle);
    for (QString &file : files) file = QDir::cleanPath(file);
    return files;
}

} // namespace

Preview::Preview(QObject *parent)
    : QObject(parent), m_resources(new PreviewResources(this)), m_watch(new PreviewWatch(this)),
      m_marks(new PreviewMarks(this))
{
    connect(m_watch, &PreviewWatch::fileChanged, this, &Preview::dependencyChanged);
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &Preview::parse);
    m_lateTimer.setSingleShot(true);
    m_lateTimer.setInterval(LateMs);
    connect(&m_lateTimer, &QTimer::timeout, this, [this] { setLate(!m_current); });
    m_dirtyTimer.setSingleShot(true);
    connect(&m_dirtyTimer, &QTimer::timeout, this, &Preview::rebuildPictures);
    // Out of sight for a while, a preview lets go of what it built; shown
    // again, it builds it again.
    m_release.setSingleShot(true);
    m_release.setInterval(ReleaseMs);
    connect(&m_release, &QTimer::timeout, this, [this] {
        if (!m_active) clear();
    });
    connect(m_resources, &PreviewResources::changed, this, &Preview::pictureChanged);
    m_style = PreviewStyle::paper();
    m_style.size = m_fontSize;
    m_style.lineHeight = m_lineHeight;
}

Preview::~Preview()
{
    ++m_generation;
    m_pool.waitForDone();
    for (size_t i = 0; i < m_shown.size(); ++i) dropIslands(i);
}

qreal Preview::tabWidth() const { return QFontMetricsF(m_style.codeFont()).horizontalAdvance(u' ') * 4; }

// --- What it is given ---------------------------------------------------------------------

void Preview::setDocument(Document *document)
{
    if (document == m_document) return;
    disconnect(m_edits);
    disconnect(m_dialect);
    disconnect(m_parses);
    disconnect(m_parseFailures);
    m_document = document;
    clear();
    // A parse asked of the document before never comes.
    if (std::exchange(m_awaiting, false)) m_parsing = false;
    if (document) {
        m_edits = connect(document->buffer(), &SourceBuffer::sourceChanged, this, &Preview::edited);
        m_dialect = connect(document, &Document::dialectChanged, this, &Preview::dialectChanged);
        m_parses = connect(document->parse(), &DocumentParse::parsed, this, [this](const ParsedTextPtr &parse) {
            if (std::exchange(m_awaiting, false)) read(parse);
        });
        m_parseFailures = connect(document->parse(), &DocumentParse::failed, this, [this](const QString &reason) {
            if (std::exchange(m_awaiting, false)) failed(reason);
        });
    }
    emit documentChanged();
    if (m_active && document) schedule(0);
}

void Preview::setBackend(Backend *backend)
{
    if (backend == m_backend) return;
    m_backend = backend;
    connectServices();
    emit backendChanged();
}

void Preview::setTheme(Theme *theme)
{
    if (theme == m_theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (theme) connect(theme, &Theme::changed, this, &Preview::restyle);
    emit themeChanged();
    restyle();
}

void Preview::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit activeChanged();
    if (!active) {
        m_timer.stop();
        setLate(false);
        m_release.start();
        return;
    }
    m_release.stop();
    connectServices();
    // The languages code is coloured by are loaded here, on this thread,
    // before a worker first reads code.
    CodeHighlight::prepare();
    if (m_document && (!m_tree || m_reread || m_shownRevision != m_document->buffer()->revision())) {
        setCurrent(false);
        schedule(0);
    }
}

void Preview::setSurface(QQuickItem *surface)
{
    if (surface == m_surface) return;
    if (m_surface) disconnect(m_surface, nullptr, this, nullptr);
    clear();
    m_surface = surface;
    m_text = documentOf(surface);
    attach();
    emit surfaceChanged();
    if (m_active && m_document) schedule(0);
}

void Preview::setViewport(QQuickItem *viewport)
{
    if (viewport == m_viewport) return;
    if (m_viewport) disconnect(m_viewport, nullptr, this, nullptr);
    m_viewport = viewport;
    if (viewport) {
        connect(viewport, SIGNAL(contentYChanged()), this, SLOT(viewMoved()));
        connect(viewport, &QQuickItem::heightChanged, this, &Preview::viewMoved);
        connect(viewport, &QQuickItem::widthChanged, this, &Preview::viewMoved);
    }
    emit viewportChanged();
}

void Preview::setFamily(const QString &family)
{
    if (family == m_family) return;
    m_family = family;
    emit typographyChanged();
    restyle();
}

void Preview::setFontSize(qreal size)
{
    if (!(size >= 6 && size <= 200) || size == m_fontSize) return;
    m_fontSize = size;
    emit typographyChanged();
    restyle();
}

void Preview::setLineHeight(qreal lineHeight)
{
    if (!(lineHeight >= 0.8 && lineHeight <= 4) || lineHeight == m_lineHeight) return;
    m_lineHeight = lineHeight;
    emit typographyChanged();
    restyle();
}

void Preview::setScale(qreal scale)
{
    if (!(scale >= 0.5 && scale <= 8) || scale == m_scale) return;
    m_scale = scale;
    emit typographyChanged();
    restyle();
}

void Preview::setRemoteImages(bool load)
{
    if (load == m_remoteImages) return;
    m_remoteImages = load;
    m_resources->setRemoteAllowed(load);
    emit remoteImagesChanged();
    // Sections with pictures are built again, and ask again.
    for (size_t i = 0; i < m_shown.size(); ++i)
        if (!m_shown[i].pictures.isEmpty()) m_dirty.insert(m_ids[i]);
    if (!m_dirty.isEmpty()) m_dirtyTimer.start(0);
}

void Preview::attach()
{
    if (!m_text) return;
    preview::Builder::prepare(m_text);
    drawInView();
    connect(m_surface, &QQuickItem::widthChanged, this, &Preview::columnChanged);
    m_column = m_surface->width();
    // The layout changes after the text does, and on its own when the
    // width does: what is placed over the text follows it.
    connect(m_text->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] {
        if (m_refreshing) return;
        refreshMarks();
        emit laidOut();
    });
}

// --- Style -------------------------------------------------------------------------------

PreviewStyle Preview::makeStyle() const
{
    PreviewStyle style;
    if (m_theme) {
        style = PreviewStyle::screen(*m_theme, m_family, m_fontSize, m_lineHeight);
    } else {
        style = PreviewStyle::paper();
        if (!m_family.isEmpty()) style.family = m_family;
        style.size = m_fontSize;
        style.lineHeight = m_lineHeight;
    }
    style.scale = m_scale;
    return style;
}

void Preview::restyle()
{
    const PreviewStyle style = makeStyle();
    if (m_theme) m_resources->setColors(RenderColors::fromTheme(*m_theme));
    m_resources->setScale(m_scale);
    if (!style.sameTypography(m_style)) {
        m_style = style;
        emit styleChanged();
        // Out of sight, what was built in the old type is let go; it is
        // built in the new when the preview is shown.
        if (m_active) rebuildAll();
        else clear();
    } else if (!style.sameColors(m_style)) {
        recolor(style);
    }
}

void Preview::applyColors(QTextDocument *document) const
{
    struct Run {
        int position;
        int length;
        int ink;
        int paper;
    };
    std::vector<Run> runs;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            const int ink = format.intProperty(PreviewStyle::InkProperty);
            const int paper = format.intProperty(PreviewStyle::PaperProperty);
            if (ink || paper) runs.push_back({fragment.position(), fragment.length(), ink, paper});
        }
    }
    if (runs.empty()) return;
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (const Run &run : runs) {
        QTextCharFormat change;
        if (run.ink) change.setForeground(m_style.ink(PreviewStyle::Ink(run.ink)));
        if (run.paper) change.setBackground(m_style.paper(PreviewStyle::Paper(run.paper)));
        cursor.setPosition(run.position);
        cursor.setPosition(run.position + run.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(change);
    }
    cursor.endEditBlock();
}

// A change of colours alone: the text stays as built, and every run that
// has a colour of its own takes the new one for its role. Plain text has
// none and follows the surface.
void Preview::recolor(const PreviewStyle &colors)
{
    m_style = m_style.withColorsOf(colors);
    if (m_text) {
        m_refreshing = true;
        applyColors(m_text);
        // The boxes of tasks are pictures in the theme's colours.
        const qreal side = std::round(m_style.size * 0.92);
        bool boxes = false;
        for (QTextBlock block = m_text->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat format = it.fragment().charFormat();
                if (!format.isImageFormat() || !format.toImageFormat().name().startsWith(QLatin1String("parch:task/"))) continue;
                boxes = true;
                m_text->markContentsDirty(it.fragment().position(), it.fragment().length());
            }
        }
        if (boxes) {
            for (const bool checked : {false, true})
                m_text->addResource(QTextDocument::ImageResource, QUrl(preview::Builder::taskImageName(checked)),
                                    preview::Builder::taskImage(m_style, checked, side));
        }
        for (const IslandState &state : std::as_const(m_islands))
            if (state.document) applyColors(state.document);
        m_refreshing = false;
    }
    emit styleChanged();
    // Formulas and diagrams are drawn again in the new colours; each stays
    // as it is until then.
    for (size_t i = 0; i < m_shown.size(); ++i)
        if (!m_shown[i].pictures.isEmpty()) m_dirty.insert(m_ids[i]);
    if (!m_dirty.isEmpty()) m_dirtyTimer.start(0);
    refreshMarks();
}

// --- Following the document ---------------------------------------------------------------

void Preview::clear()
{
    ++m_generation;
    m_timer.stop();
    m_waiting.invalidate();
    setLate(false);
    m_dirtyTimer.stop();
    m_work.clear();
    m_dirty.clear();
    for (size_t i = 0; i < m_shown.size(); ++i) dropIslands(i);
    m_shown.clear();
    m_parts.clear();
    m_ids.clear();
    m_islands.clear();
    m_tree.reset();
    m_citations.reset();
    m_target.reset();
    m_reread = false;
    m_pictureFiles.clear();
    m_readFiles.clear();
    m_watch->setFiles({});
    setUnresolved({});
    setWebPictures(false);
    m_startsValid = 0;
    m_shownRevision = 0;
    m_trimmed = 0;
    m_pendingSource = -1;
    m_pendingName.clear();
    if (!m_trail.isEmpty()) {
        m_trail.clear();
        emit trailChanged();
    }
    m_marks->show({});
    m_resources->clear();
    if (m_text) {
        m_refreshing = true;
        preview::Builder::prepare(m_text);
        m_refreshing = false;
        drawInView();
    }
    setCurrent(false);
}

// Qt's TextEdit gives the scene graph the nodes of every block of its text
// unless it observes its view, which it decides for itself only when its
// text property is set; the surface is built through its document instead,
// so the preview decides, as the editor's binding does. Observing, the
// TextEdit makes nodes for the blocks in view, and makes them again as the
// view moves on. It goes on observing while there is text, however short:
// the blocks that were out of view have no nodes, and a TextEdit that has
// stopped observing would not make them when they come into view.
void Preview::drawInView()
{
    if (!m_surface || !m_text) return;
    const bool observing = m_surface->flags().testFlag(QQuickItem::ItemObservesViewport);
    if (!observing && m_text->characterCount() > LargeTextUnits)
        m_surface->setFlag(QQuickItem::ItemObservesViewport, true);
    else if (observing && m_text->isEmpty())
        m_surface->setFlag(QQuickItem::ItemObservesViewport, false);
}

// Observing its view, Qt's TextEdit follows an edit by making nodes again
// from the first of its nodes that the edit touched. An edit that takes
// away the first block in view, as undoing a long paste where it is seen
// does, leaves it no node to start from, and the frame after shows nothing
// there; once that frame is out, the surface is drawn again from the start
// of its text, as far as its view reaches.
void Preview::redrawAfterFrame()
{
    QQuickWindow *window = m_surface ? m_surface->window() : nullptr;
    if (!window || m_frameSwapped) return;
    m_frameSwapped = connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            disconnect(std::exchange(m_frameSwapped, {}));
            if (!m_surface) return;
            // A change of the colour of selected text has the TextEdit make
            // all of its nodes again; the colour is back before it does.
            const QColor colour = m_surface->property("selectedTextColor").value<QColor>();
            m_surface->setProperty("selectedTextColor", QColor::fromRgba(colour.rgba() ^ 1));
            m_surface->setProperty("selectedTextColor", colour);
        },
        Qt::QueuedConnection);
}

void Preview::setUnresolved(const QStringList &messages)
{
    const QString said = messages.isEmpty() ? QString() : messages.join(QStringLiteral(". ")) + u'.';
    if (said == m_unresolved) return;
    m_unresolved = said;
    emit unresolvedChanged();
}

void Preview::setWebPictures(bool web)
{
    if (web == m_webPictures) return;
    m_webPictures = web;
    emit webPicturesChanged();
}

void Preview::setCurrent(bool current)
{
    if (current == m_current) return;
    m_current = current;
    emit currentChanged();
}

void Preview::setLate(bool late)
{
    if (!late) m_lateTimer.stop();
    if (late == m_late) return;
    m_late = late;
    emit lateChanged();
}

void Preview::edited()
{
    setCurrent(false);
    if (!m_waiting.isValid()) m_waiting.start();
    if (m_active) waitForPause();
}

// Keys typed one after another are each drawn in a frame of their own,
// without the preview's reading and setting of the text in it: the text is
// read once typing pauses for PauseMs. While typing goes on, it is read at
// the latest MaxWaitMs after the first edit no read has begun with, so that
// the preview keeps up with the writer.
void Preview::waitForPause()
{
    if (m_parsing) {
        m_parseAgain = true;
        return;
    }
    const qint64 left = m_waiting.isValid() ? std::max<qint64>(0, MaxWaitMs - m_waiting.elapsed()) : 0;
    m_timer.start(int(std::min<qint64>(PauseMs, left)));
}

// The settings the document is read in changed: its Markdown profile, its
// citation style or its bibliography. The same text reads otherwise, so
// what is being read in the old settings is not taken, and the text is read
// again at once, or when the preview is next shown.
void Preview::dialectChanged()
{
    ++m_generation;
    m_reread = true;
    setCurrent(false);
    if (m_active) schedule(0);
}

// Asks for the text to be parsed once the wait is over. A wait already
// running is not started again, so that steady typing is still followed.
void Preview::schedule(int delayMs)
{
    if (m_parsing) {
        m_parseAgain = true;
        return;
    }
    if (!m_timer.isActive()) m_timer.start(delayMs);
}

namespace {
std::atomic<int> workerDelay{0};
}

void Preview::setWorkerDelay(int milliseconds) { workerDelay = std::max(0, milliseconds); }

// Reads the text as every output of the document reads it: the document's
// parse of it, in the dialect its settings and front matter choose, with
// its includes from disk and its bibliography read, which every view of the
// document shares; here its citations are formatted by its citation style,
// and its parts found.
Preview::Parsed Preview::parseJob(ParsedTextPtr parse, quint64 generation)
{
    if (const int delay = workerDelay; delay > 0) QThread::msleep(delay);
    WorkerFailure::check(WorkerFailure::Job::Preview);
    Parsed result;
    result.revision = parse->revision;
    result.generation = generation;
    // The citations point into the tree, which the parse holds: the two
    // are kept together.
    struct Read {
        ParsedTextPtr parse;
        DocumentCitations::Formatted formatted;
    };
    const DocumentReading::Reading &reading = parse->reading;
    auto read = std::make_shared<const Read>(Read{parse, DocumentCitations::format(reading.document, reading.citations)});
    result.tree = std::shared_ptr<const md::Document>(read, &read->parse->reading.document);
    result.citations = std::shared_ptr<const DocumentCitations::Formatted>(read, &read->formatted);
    result.parts = std::make_shared<std::vector<Part>>(preview::parts(*result.tree, result.citations.get()));
    preview::warm(*result.tree);
    picturesOf(reading.document, reading.document.root, result.pictures, result.webPictures);
    result.files = filesRead(reading);
    result.unresolved = preview::Unresolved(reading.document).messages();
    return result;
}

void Preview::parse()
{
    if (!m_active || !m_document || !m_text) return;
    if (m_parsing) {
        m_parseAgain = true;
        return;
    }
    m_parsing = true;
    m_parseAgain = false;
    // This read takes in every edit so far; one that takes long says so.
    m_readWaiting = m_waiting;
    m_waiting.invalidate();
    if (!m_late && !m_lateTimer.isActive()) m_lateTimer.start();
    // The text as it is now, parsed for every view of the document: at
    // once when it is already, or else once it is.
    m_awaiting = true;
    if (const ParsedTextPtr parse = m_document->parse()->request()) {
        m_awaiting = false;
        read(parse);
    } else if (const QString why = m_document->parse()->failure(); !why.isEmpty()) {
        m_awaiting = false;
        failed(why);
    }
}

void Preview::read(const ParsedTextPtr &parse)
{
    if (!m_active || !m_document || !m_text) {
        m_parsing = false;
        return;
    }
    QtConcurrent::run(&m_pool, &Preview::parseJob, parse, m_generation)
        .then(this, [this](const Parsed &result) {
            m_parsing = false;
            take(result);
        })
        .onFailed(this, [this](const QUnhandledException &failure) { failed(WorkerFailure::reason(failure.exception())); })
        .onFailed(this, [this] { failed(QString()); });
}

void Preview::failed(const QString &reason)
{
    m_parsing = false;
    const QString why = reason.isEmpty() ? QStringLiteral("it could not be read") : reason;
    if (std::exchange(m_failure, why) != why) emit failureChanged();
    if (m_backend)
        m_backend->reportProblem(tr("Preview not updated"),
                                 tr("The preview could not read “%1”: %2.").arg(m_document ? m_document->title() : QString(), why));
    setLate(false);
    // What was asked meanwhile is read, from the text as it is then.
    if (std::exchange(m_parseAgain, false) && m_active && m_document && m_text) waitForPause();
}

void Preview::take(const Parsed &result)
{
    const bool again = std::exchange(m_parseAgain, false);
    if (result.generation != m_generation || !m_document || !m_text || !m_active) {
        if (m_active && m_document && m_text) m_timer.start(0);
        return;
    }
    // Never stale text over newer: the result is for the text as it is, or
    // it is dropped and the text read again once typing allows, as soon as
    // the first edit it was for has waited MaxWaitMs. Typing that goes on
    // past that wait is shown as far as the read took it, which is newer
    // than what is shown, and read again: keys that come quicker than a
    // read would otherwise keep the preview from following them at all.
    const bool overdue = m_readWaiting.isValid() && m_readWaiting.elapsed() >= MaxWaitMs;
    if (result.revision != m_document->buffer()->revision() && (!overdue || result.revision <= m_shownRevision)) {
        ++m_stale;
        if (m_readWaiting.isValid() && (!m_waiting.isValid() || m_readWaiting.elapsed() > m_waiting.elapsed()))
            m_waiting = m_readWaiting;
        waitForPause();
        return;
    }
    if (!m_failure.isEmpty()) {
        m_failure.clear();
        emit failureChanged();
    }
    plan(result);
    if (again) waitForPause();
}

// Works out which sections go and which parts are built, leaving every
// section whose key is the new part's as it is.
void Preview::plan(const Parsed &result)
{
    m_work.clear();
    const std::vector<Part> &parts = *result.parts;
    const size_t before = m_shown.size(), after = parts.size();
    const auto keep = [&](size_t shown, size_t part) {
        m_parts[shown] = parts[part];
        preview::sourceOf(*result.tree, parts[part], &m_shown[shown].sourceStart, &m_shown[shown].sourceEnd,
                          result.citations.get());
    };
    size_t head = 0;
    while (head < before && head < after && m_shown[head].key == parts[head].key) {
        keep(head, head);
        ++head;
    }
    size_t tail = 0;
    while (tail < before - head && tail < after - head && m_shown[before - 1 - tail].key == parts[after - 1 - tail].key) {
        keep(before - 1 - tail, after - 1 - tail);
        ++tail;
    }
    const size_t oldMiddle = before - head - tail, newMiddle = after - head - tail;
    if (oldMiddle == newMiddle) {
        // Nothing came or went: each part that differs takes its section's place.
        size_t i = 0;
        while (i < oldMiddle) {
            if (m_shown[head + i].key == parts[head + i].key) {
                keep(head + i, head + i);
                ++i;
                continue;
            }
            size_t run = i;
            while (run < oldMiddle && m_shown[head + run].key != parts[head + run].key) ++run;
            Operation operation;
            operation.at = head + i;
            operation.remove = run - i;
            operation.parts.assign(parts.begin() + long(head + i), parts.begin() + long(head + run));
            m_work.push_back(std::move(operation));
            i = run;
        }
    } else {
        Operation operation;
        operation.at = head;
        operation.remove = oldMiddle;
        operation.parts.assign(parts.begin() + long(head), parts.begin() + long(head + newMiddle));
        m_work.push_back(std::move(operation));
    }
    // The sections that go were built from the tree that goes with them.
    m_tree = result.tree;
    m_citations = result.citations;
    m_target = result.parts;
    m_reread = false;
    // What the text was read with is watched from now on, and nothing else.
    m_pictureFiles = QSet<QString>(result.pictures.cbegin(), result.pictures.cend());
    m_readFiles = QSet<QString>(result.files.cbegin(), result.files.cend());
    m_watch->setFiles(result.files + result.pictures);
    setUnresolved(result.unresolved);
    setWebPictures(result.webPictures);
    m_planRevision = result.revision;
    ++m_updates;
    // Text that reads as it did, as after an undo and its redo, is shown already.
    if (m_work.empty()) finishWork();
    else work();
}

void Preview::removeSections(QTextCursor &cursor, size_t at, size_t count)
{
    if (count == 0) return;
    const int start = startOf(at);
    int length = 0;
    for (size_t i = at; i < at + count; ++i) {
        length += m_shown[i].length;
        dropIslands(i);
    }
    if (length > 0) {
        // The first block in view goes, with what the surface drew of it.
        if (m_viewStart > start - 1 && m_viewStart < start + length - 1 && m_surface
            && m_surface->flags().testFlag(QQuickItem::ItemObservesViewport))
            m_viewRemoved = true;
        // From the end of the block before to the end of the last block that
        // goes, so that the block after stays whole. Whichever format the
        // joined block is left with, the block before gets its own back.
        cursor.setPosition(start - 1);
        const QTextBlockFormat format = cursor.blockFormat();
        const QTextCharFormat chars = cursor.blockCharFormat();
        cursor.setPosition(start + length - 1, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        cursor.setBlockFormat(format);
        cursor.setBlockCharFormat(chars);
    }
    m_shown.erase(m_shown.begin() + long(at), m_shown.begin() + long(at + count));
    m_parts.erase(m_parts.begin() + long(at), m_parts.begin() + long(at + count));
    m_ids.erase(m_ids.begin() + long(at), m_ids.begin() + long(at + count));
    invalidateStarts(at);
}

void Preview::insertSection(QTextCursor &cursor, preview::Builder &builder, size_t at, const Part &part)
{
    cursor.setPosition(startOf(at) - 1);
    Section section = builder.build(part, cursor);
    const quint64 id = m_nextId++;
    for (size_t i = 0; i < section.islands.size(); ++i) {
        IslandState state;
        state.document = section.islands[i].document;
        if (section.islands[i].kind == Island::Table)
            if (QTextTable *table = tableOf(state.document)) state.columns = table->columns();
        m_islands.insert(islandId(id, i), state);
    }
    m_shown.insert(m_shown.begin() + long(at), std::move(section));
    m_parts.insert(m_parts.begin() + long(at), part);
    m_ids.insert(m_ids.begin() + long(at), id);
    invalidateStarts(at);
    ++m_built;
    if (!m_shown[at].islands.empty()) layoutIslands(at, at, &cursor);
}

// An island's document goes with its section. A view that still shows it
// is given an empty one first.
void Preview::dropIslands(size_t index)
{
    Section &section = m_shown[index];
    for (size_t i = 0; i < section.islands.size(); ++i) {
        Island &island = section.islands[i];
        const auto state = m_islands.find(islandId(m_ids[index], i));
        if (state != m_islands.end()) {
            if (state->view && island.document && documentOf(state->view) == island.document) {
                auto *quick = state->view->property("textDocument").value<QQuickTextDocument *>();
                quick->setTextDocument(new QTextDocument(state->view));
            }
            m_islands.erase(state);
        }
        delete island.document.data();
        island.document = nullptr;
    }
}

void Preview::connectServices()
{
    m_resources->setFetcher(m_backend ? m_backend->remoteFetcher() : nullptr);
    // The render service starts its thread when a preview is first shown.
    m_resources->setRenderService(m_backend && m_active ? m_backend->renderService() : nullptr);
}

void Preview::work()
{
    if (m_work.empty()) return;
    if (!m_text || !m_tree) {
        m_work.clear();
        return;
    }
    QElapsedTimer timer;
    timer.start();
    const TopAnchor top = captureTop();
    m_viewStart = top.valid ? top.cursor.position() : -1;
    m_refreshing = true;
    {
        QTextCursor cursor(m_text);
        cursor.beginEditBlock();
        preview::BuildOptions options;
        options.islandParent = this;
        options.citations = m_citations.get();
        preview::Builder builder(*m_tree, m_style, m_resources, options);
        bool more = false;
        // From the last operation to the first, so that the sections before
        // an operation are where it expects them.
        while (!m_work.empty() && !more) {
            Operation &operation = m_work.back();
            if (operation.remove > 0) {
                removeSections(cursor, operation.at, operation.remove);
                operation.remove = 0;
            }
            while (operation.done < operation.parts.size()) {
                insertSection(cursor, builder, operation.at + operation.done, operation.parts[operation.done]);
                ++operation.done;
                if (qreal(timer.nsecsElapsed()) / 1e6 >= m_buildMs
                    && (operation.done < operation.parts.size() || m_work.size() > 1)) {
                    more = true;
                    break;
                }
            }
            if (!more) m_work.pop_back();
        }
        trimTop(cursor);
        cursor.endEditBlock();
    }
    m_refreshing = false;
    drawInView();
    if (std::exchange(m_viewRemoved, false)) redrawAfterFrame();
    restoreTop(top);
    refreshMarks();
    // Once the text is in, the surface lays it out, which takes longer than
    // building it: the next slice builds as much as kept this one, with
    // its layout, within SliceMs.
    const qreal slice = qreal(timer.nsecsElapsed()) / 1e6;
    if (slice > SliceMs) m_buildMs = std::max(0.25, m_buildMs * std::max(0.3, SliceMs / slice));
    else if (slice < SliceMs * 0.6) m_buildMs = std::min<qreal>(SliceMs, m_buildMs * 1.3);
    m_longestSlice = std::max(m_longestSlice, slice);
    if (!m_work.empty()) {
        // The rest after the events that are waiting, such as a key.
        QTimer::singleShot(0, this, &Preview::work);
        return;
    }
    finishWork();
}

// The text starts at the top of the page: the first block has no room
// above it of its own, as a heading has elsewhere, so that the text and its
// preview start level. The section that was first before gets its room back.
void Preview::trimTop(QTextCursor &cursor)
{
    size_t first = 0;
    while (first < m_shown.size() && m_shown[first].length == 0) ++first;
    const quint64 id = first < m_shown.size() ? m_ids[first] : 0;
    if (id == m_trimmed) return;
    if (m_trimmed && std::find(m_ids.begin(), m_ids.end(), m_trimmed) != m_ids.end()) {
        m_dirty.insert(m_trimmed);
        if (!m_dirtyTimer.isActive()) m_dirtyTimer.start(0);
    }
    m_trimmed = id;
    if (!id) return;
    cursor.setPosition(startOf(first));
    QTextBlockFormat format = cursor.blockFormat();
    if (format.topMargin() == 0) return;
    format.setTopMargin(0);
    cursor.setBlockFormat(format);
}

void Preview::finishWork()
{
    setLate(false);
    m_shownRevision = m_planRevision;
    QSet<QString> pictures;
    for (const Section &section : m_shown)
        for (const QString &key : section.pictures) pictures.insert(key);
    m_resources->keepOnly(pictures);
    setCurrent(m_document && m_shownRevision == m_document->buffer()->revision());
    if (m_pendingSource >= 0) {
        const int source = std::exchange(m_pendingSource, -1);
        showSource(source);
    }
    if (!m_pendingName.isEmpty()) {
        const QString name = std::exchange(m_pendingName, QString());
        goTo(name);
    }
    emit updated();
}

// Everything again, as after a change of the face or the size.
void Preview::rebuildAll()
{
    if (!m_tree || !m_text || !m_target) return;
    // The view comes back to the text it showed, wherever that is then.
    if (m_pendingSource < 0 && m_work.empty() && viewTop() > 0) m_pendingSource = topSource();
    // Every part of the text as it was last read, in place of whatever is
    // built of it so far.
    m_work.clear();
    Operation operation;
    operation.at = 0;
    operation.remove = m_shown.size();
    operation.parts = *m_target;
    m_work.push_back(std::move(operation));
    m_dirty.clear();
    work();
}

QStringList Preview::watchedFiles() const { return m_watch->files(); }

// A picture changed on disk is read again, and the sections that show it
// are built again once it has been; the picture as it was stays until then.
// What the text was read with, changed, has the text read again, and only
// the parts that read otherwise now are built again.
void Preview::dependencyChanged(const QString &path)
{
    if (m_pictureFiles.contains(path)) m_resources->reread(path);
    if (!m_readFiles.contains(path)) return;
    m_reread = true;
    // Every view of the document reads the text again with it.
    if (m_document) m_document->parse()->invalidate();
    if (m_active && m_document) schedule(0);
}

void Preview::pictureChanged(const QString &key)
{
    for (size_t i = 0; i < m_shown.size(); ++i)
        if (m_shown[i].pictures.contains(key)) m_dirty.insert(m_ids[i]);
    if (!m_dirty.isEmpty() && !m_dirtyTimer.isActive()) m_dirtyTimer.start(0);
}

// The sections whose pictures arrived are built again, each in its place.
void Preview::rebuildPictures()
{
    if (m_dirty.isEmpty() || !m_tree || !m_text) return;
    if (!m_work.empty()) {
        m_dirtyTimer.start(10);
        return;
    }
    const QSet<quint64> dirty = std::exchange(m_dirty, {});
    for (size_t i = 0; i < m_shown.size(); ++i) {
        if (!dirty.contains(m_ids[i])) continue;
        Operation operation;
        operation.at = i;
        operation.remove = 1;
        operation.parts = {m_parts[i]};
        m_work.push_back(std::move(operation));
    }
    m_planRevision = m_shownRevision;
    work();
}

// --- Positions ---------------------------------------------------------------------------

void Preview::invalidateStarts(size_t from) { m_startsValid = std::min(m_startsValid, from + 1); }

int Preview::startOf(size_t index) const
{
    if (m_starts.size() < m_shown.size() + 1) m_starts.resize(m_shown.size() + 1);
    if (m_startsValid == 0) {
        m_starts[0] = 1;
        m_startsValid = 1;
    }
    index = std::min(index, m_shown.size());
    while (m_startsValid <= index) {
        m_starts[m_startsValid] = m_starts[m_startsValid - 1] + m_shown[m_startsValid - 1].length;
        ++m_startsValid;
    }
    return m_starts[index];
}

size_t Preview::sectionAt(int position) const
{
    if (m_shown.empty()) return 0;
    startOf(m_shown.size());
    const auto begin = m_starts.begin(), end = m_starts.begin() + long(m_shown.size());
    const auto after = std::upper_bound(begin, end, position);
    return after == begin ? 0 : size_t(after - begin) - 1;
}

const Section *Preview::sectionOfIsland(quint64 island, const Island **found, size_t *index) const
{
    const quint64 id = island >> 20;
    const size_t which = size_t(island & 0xfffff);
    if (which == 0) return nullptr;
    for (size_t i = 0; i < m_shown.size(); ++i) {
        if (m_ids[i] != id) continue;
        if (which > m_shown[i].islands.size()) return nullptr;
        if (found) *found = &m_shown[i].islands[which - 1];
        if (index) *index = i;
        return &m_shown[i];
    }
    return nullptr;
}

QTextDocument *Preview::islandDocument(quint64 island) const
{
    const auto state = m_islands.constFind(island);
    return state == m_islands.cend() ? nullptr : state->document.data();
}

// --- Layout ------------------------------------------------------------------------------

qreal Preview::columnWidth() const { return m_column > 0 ? m_column : 600; }

QRectF Preview::blockRect(int position) const
{
    const QTextBlock block = m_text->findBlock(std::clamp(position, 0, std::max(0, m_text->characterCount() - 1)));
    return block.isValid() ? m_text->documentLayout()->blockBoundingRect(block) : QRectF();
}

// Where a block ends. The layout's own rectangle for a block leaves out the
// room a line height adds to its lines, so the end is taken from where the
// next block starts, less the margin between the two, which is the larger
// of theirs.
qreal Preview::blockBottom(int position) const
{
    const QTextBlock block = m_text->findBlock(std::clamp(position, 0, std::max(0, m_text->characterCount() - 1)));
    if (!block.isValid()) return 0;
    QAbstractTextDocumentLayout *layout = m_text->documentLayout();
    const QTextBlockFormat format = block.blockFormat();
    const QTextBlock next = block.next();
    if (!next.isValid()) return layout->documentSize().height() - format.bottomMargin();
    return layout->blockBoundingRect(next).top() - std::max(format.bottomMargin(), next.blockFormat().topMargin());
}

void Preview::measureIsland(Island &island, IslandState &state) const
{
    QTextDocument *document = island.document;
    if (!document) return;
    const qreal padding = island.tinted ? m_style.padding() : 0;
    const qreal room = std::max<qreal>(40, columnWidth() - island.indent - 2 * padding);
    if (island.wraps) {
        // A table is laid out to the column, but not so narrow that every
        // cell is a word wide when it could scroll sideways instead.
        if (state.natural < 0) {
            document->setTextWidth(-1);
            state.natural = std::ceil(document->size().width());
        }
        const qreal comfortable = state.columns * m_style.size * 12;
        qreal width = std::max(room, std::min(state.natural, comfortable));
        document->setTextWidth(width);
        QSizeF size = document->size();
        if (size.width() > width + 0.5) {
            width = std::ceil(size.width());
            document->setTextWidth(width);
            size = document->size();
        }
        state.size = QSizeF(width, size.height());
        state.rules.clear();
        state.headerRule = -1;
        if (QTextTable *table = tableOf(document)) {
            QAbstractTextDocumentLayout *layout = document->documentLayout();
            for (int row = 1; row < table->rows(); ++row) {
                qreal top = std::numeric_limits<qreal>::max();
                for (int column = 0; column < table->columns(); ++column) {
                    const QTextTableCell cell = table->cellAt(row, column);
                    if (cell.row() != row) continue;
                    const qreal padded = cell.format().toTableCellFormat().topPadding();
                    top = std::min(top, layout->blockBoundingRect(cell.firstCursorPosition().block()).top() - padded);
                }
                if (top == std::numeric_limits<qreal>::max()) continue;
                if (row == island.headerRows) state.headerRule = int(state.rules.size());
                state.rules.append(std::round(top));
            }
        }
    } else if (island.kind == Island::Code) {
        // Its lines are a fixed distance apart, so its height is known
        // without laying it out; its width is its view's to find.
        qreal height = 0;
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            const QTextBlockFormat format = block.blockFormat();
            height += format.lineHeight() + format.topMargin() + format.bottomMargin();
        }
        state.size = QSizeF(0, height);
    } else {
        document->setTextWidth(-1);
        state.size = document->size();
    }
    state.height = std::ceil(state.size.height() + 2 * padding);
}

// Measures the islands of sections first to last and gives each one's
// block its height.
void Preview::layoutIslands(size_t first, size_t last, QTextCursor *cursor)
{
    if (!m_text) return;
    for (size_t i = first; i <= last && i < m_shown.size(); ++i) {
        Section &section = m_shown[i];
        for (size_t k = 0; k < section.islands.size(); ++k) {
            const auto state = m_islands.find(islandId(m_ids[i], k));
            if (state == m_islands.end()) continue;
            const qreal before = state->height;
            measureIsland(section.islands[k], *state);
            const QTextBlock block = m_text->findBlock(startOf(i) + section.islands[k].placeholder);
            if (!block.isValid()) continue;
            QTextBlockFormat format = block.blockFormat();
            if (state->height == before && format.lineHeight() == state->height) continue;
            format.setLineHeight(state->height, QTextBlockFormat::FixedHeight);
            QTextCursor own(block);
            (cursor ? *cursor : own).setPosition(block.position());
            (cursor ? *cursor : own).setBlockFormat(format);
        }
    }
}

void Preview::columnChanged()
{
    if (!m_surface) return;
    const qreal width = m_surface->width();
    if (width == m_column) return;
    m_column = width;
    if (m_shown.empty()) return;
    // Tables are laid out again at the new width.
    m_refreshing = true;
    {
        QTextCursor cursor(m_text);
        cursor.beginEditBlock();
        for (size_t i = 0; i < m_shown.size(); ++i) {
            bool wraps = false;
            for (const Island &island : m_shown[i].islands) wraps = wraps || island.wraps;
            if (wraps) layoutIslands(i, i, &cursor);
        }
        cursor.endEditBlock();
    }
    m_refreshing = false;
    refreshMarks();
}

qreal Preview::viewTop() const
{
    if (!m_viewport || !m_surface) return 0;
    QQuickItem *content = m_viewport->property("contentItem").value<QQuickItem *>();
    const qreal offset = content ? m_surface->mapToItem(content, QPointF(0, 0)).y() : m_surface->y();
    return m_viewport->property("contentY").toReal() - offset;
}

qreal Preview::viewHeight() const { return m_viewport ? m_viewport->height() : 0; }

void Preview::scrollTo(qreal y)
{
    if (!m_viewport || !m_surface) return;
    QQuickItem *content = m_viewport->property("contentItem").value<QQuickItem *>();
    const qreal offset = content ? m_surface->mapToItem(content, QPointF(0, 0)).y() : m_surface->y();
    // Not past the end of what there is to show, nor before its start.
    const qreal most = std::max<qreal>(0, m_viewport->property("contentHeight").toReal() - m_viewport->height());
    const qreal contentY = std::clamp(y + offset, qreal(0), std::max(most, m_viewport->property("contentY").toReal()));
    m_viewport->setProperty("contentY", std::round(contentY));
}

void Preview::viewMoved() { refreshMarks(); }

// The block at the top of the view and how far the view has moved into it,
// so that the same text stays there when blocks above it change.
Preview::TopAnchor Preview::captureTop() const
{
    TopAnchor anchor;
    if (!m_text || !m_viewport || m_shown.empty()) return anchor;
    const qreal top = viewTop();
    if (top <= 0) return anchor;
    const int position = m_text->documentLayout()->hitTest(QPointF(0, top), Qt::FuzzyHit);
    if (position < 0) return anchor;
    const QTextBlock block = m_text->findBlock(position);
    if (!block.isValid()) return anchor;
    anchor.cursor = QTextCursor(block);
    anchor.offset = top - m_text->documentLayout()->blockBoundingRect(block).top();
    anchor.valid = true;
    return anchor;
}

void Preview::restoreTop(const TopAnchor &anchor)
{
    if (!anchor.valid || anchor.cursor.isNull()) return;
    const qreal y = m_text->documentLayout()->blockBoundingRect(anchor.cursor.block()).top() + anchor.offset;
    if (std::abs(y - viewTop()) >= 1) scrollTo(y);
}

void Preview::refreshMarks()
{
    if (!m_text || m_shown.empty()) {
        m_marks->show({});
        return;
    }
    QAbstractTextDocumentLayout *layout = m_text->documentLayout();
    // What is in view, and half a view either side of it.
    const qreal height = std::max<qreal>(viewHeight(), 200);
    const qreal top = viewTop() - height / 2, bottom = viewTop() + height * 1.5;
    const qreal step = height / 2;
    const qreal ruleTop = std::floor(top / step) * step, ruleBottom = std::ceil(bottom / step) * step;
    const size_t first = sectionAt(std::max(0, layout->hitTest(QPointF(0, std::max<qreal>(0, top)), Qt::FuzzyHit)));
    const size_t last = sectionAt(std::max(0, layout->hitTest(QPointF(0, std::max<qreal>(0, bottom)), Qt::FuzzyHit)));
    const qreal column = columnWidth();
    // As far right as the view reaches: the bars of quotes nested deeper
    // than that could not be seen.
    const qreal right = m_viewport && m_surface ? m_viewport->width() - m_surface->x() : column;
    const qreal bar = std::max<qreal>(2, std::round(m_style.size / 7));
    QList<PreviewMarks::Row> rows;
    for (size_t i = first; i <= last && i < m_shown.size(); ++i) {
        const Section &section = m_shown[i];
        const int base = startOf(i);
        quint64 number = 0;
        for (const Mark &mark : section.marks) {
            PreviewMarks::Row row;
            row.key = (m_ids[i] << 20) | (0x80000 + number++);
            const QRectF firstRect = blockRect(base + mark.first);
            switch (mark.kind) {
            case Mark::QuoteBar:
                row.kind = QStringLiteral("bar");
                row.rect = QRectF(mark.x, firstRect.top(), bar, blockBottom(base + mark.last) - firstRect.top());
                break;
            case Mark::Rule:
                row.kind = QStringLiteral("rule");
                row.rect = QRectF(mark.x, firstRect.top(), column - mark.x, 1);
                break;
            case Mark::NotesRule:
                row.kind = QStringLiteral("notesRule");
                row.rect = QRectF(0, firstRect.top(), std::round(column * 0.28), 1);
                break;
            case Mark::PageBreak:
                row.kind = QStringLiteral("pageBreak");
                row.rect = QRectF(mark.x, firstRect.top(), column - mark.x, blockBottom(base + mark.first) - firstRect.top());
                break;
            case Mark::Heading:
                row.kind = QStringLiteral("heading");
                row.rect = QRectF(mark.x, firstRect.top(), column - mark.x, firstRect.height());
                row.level = mark.level;
                row.text = mark.text;
                break;
            case Mark::Image:
                row.kind = QStringLiteral("image");
                row.rect = QRectF(mark.x, firstRect.top(), column - mark.x, firstRect.height());
                row.text = mark.text;
                break;
            }
            if (row.rect.bottom() < top || row.rect.top() > bottom || row.rect.left() > right) continue;
            rows.append(row);
        }
        for (size_t k = 0; k < section.islands.size(); ++k) {
            const Island &island = section.islands[k];
            const quint64 id = islandId(m_ids[i], k);
            const auto state = m_islands.constFind(id);
            if (state == m_islands.cend()) continue;
            PreviewMarks::Row row;
            row.key = id;
            row.kind = QStringLiteral("island");
            row.island = id;
            row.islandKind = island.kind == Island::Table  ? QStringLiteral("table")
                             : island.kind == Island::Math ? QStringLiteral("math")
                                                           : QStringLiteral("code");
            row.rect = QRectF(island.indent, blockRect(base + island.placeholder).top(), column - island.indent, state->height);
            if (row.rect.bottom() < top || row.rect.top() > bottom || row.rect.left() > right) continue;
            row.text = island.description;
            row.wraps = island.wraps;
            row.contentWidth = state->size.width();
            row.contentHeight = state->size.height();
            row.centered = island.centered;
            row.tinted = island.tinted;
            // A long table's lines near the view only, in steps of half a
            // view, so that they are made again only every so often.
            const qreal textTop = row.rect.top() + (island.tinted ? m_style.padding() : 0);
            for (qsizetype rule = 0; rule < state->rules.size(); ++rule) {
                const qreal y = textTop + state->rules.at(rule);
                if (y < ruleTop || y > ruleBottom) continue;
                if (rule == state->headerRule) row.headerRule = int(row.rules.size());
                row.rules.append(state->rules.at(rule));
            }
            row.position = base + island.placeholder;
            rows.append(row);
        }
    }
    m_marks->show(rows);
}

void Preview::adopt(quint64 island, QQuickItem *textEdit)
{
    const auto state = m_islands.find(island);
    if (state == m_islands.end() || !state->document || !textEdit) return;
    auto *quick = textEdit->property("textDocument").value<QQuickTextDocument *>();
    if (!quick) return;
    if (quick->textDocument() != state->document) quick->setTextDocument(state->document);
    state->view = textEdit;
    // A long island is drawn only where it is in view, as the surface is.
    // It scrolls sideways in a Flickable of its own, which passes the
    // preview's view on to it by observing that view too.
    const bool large = state->document->characterCount() > LargeTextUnits;
    textEdit->setFlag(QQuickItem::ItemObservesViewport, large);
    for (QQuickItem *outer = textEdit->parentItem(); outer && outer != m_surface; outer = outer->parentItem()) {
        if (!outer->flags().testFlag(QQuickItem::ItemIsViewport)) continue;
        outer->setFlag(QQuickItem::ItemObservesViewport, large);
        break;
    }
}

// --- Finding each other ------------------------------------------------------------------

int Preview::sourceAt(int position, quint64 island) const
{
    const QList<Anchor> *anchors = nullptr;
    int relative = position, base = -1;
    if (island) {
        const Island *found = nullptr;
        const Section *section = sectionOfIsland(island, &found);
        if (!section || !found) return -1;
        anchors = &found->anchors;
        base = section->sourceStart;
    } else {
        if (m_shown.empty() || position < 1) return m_shown.empty() ? -1 : std::max(0, m_shown.front().sourceStart);
        const size_t index = sectionAt(position);
        anchors = &m_shown[index].anchors;
        relative = position - startOf(index);
        base = m_shown[index].sourceStart;
    }
    if (base < 0) return -1;
    const auto after = std::upper_bound(anchors->cbegin(), anchors->cend(), relative,
                                        [](int at, const Anchor &anchor) { return at < anchor.position; });
    if (after == anchors->cbegin()) return base;
    const Anchor &anchor = *(after - 1);
    return base + anchor.source + std::min(relative - anchor.position, anchor.length);
}

QVariantMap Preview::includedAt(int position, quint64 island) const
{
    const QList<Anchor> *own = nullptr, *included = nullptr;
    int relative = position;
    if (island) {
        const Island *found = nullptr;
        if (!sectionOfIsland(island, &found) || !found) return {};
        own = &found->anchors;
        included = &found->included;
    } else {
        if (m_shown.empty() || position < 0) return {};
        const size_t index = sectionAt(position);
        own = &m_shown[index].anchors;
        included = &m_shown[index].included;
        relative = position - startOf(index);
    }
    // The nearest place before it that says where it came from: included
    // text when that place is one an include brought in.
    const auto nearest = [relative](const QList<Anchor> &anchors) -> const Anchor * {
        const Anchor *best = nullptr;
        for (const Anchor &anchor : anchors)
            if (anchor.position <= relative && (!best || anchor.position >= best->position)) best = &anchor;
        return best;
    };
    const Anchor *in = nearest(*included);
    const Anchor *outside = nearest(*own);
    if (!in || (outside && outside->position > in->position) || !m_tree || in->file <= 0
        || size_t(in->file) >= m_tree->files.size())
        return {};
    const md::SourceFile &file = m_tree->files[size_t(in->file)];
    if (file.path.isEmpty()) return {};
    const int offset = std::clamp(in->source + std::min(relative - in->position, in->length), 0, int(file.text.size()));
    int line = 0, lineStart = 0;
    for (int i = 0; i < offset; ++i) {
        const QChar c = file.text.at(i);
        if (c == u'\n' || (c == u'\r' && (i + 1 >= file.text.size() || file.text.at(i + 1) != u'\n'))) {
            ++line;
            lineStart = i + 1;
        }
    }
    return {{QStringLiteral("path"), file.path},
            {QStringLiteral("line"), line},
            {QStringLiteral("column"), offset - lineStart}};
}

namespace {
// The sections in the order of the source: the blocks of the text, before
// the notes and the bibliography.
size_t flowCount(const std::vector<Section> &sections)
{
    size_t count = 0;
    while (count < sections.size() && sections[count].kind == Part::Block) ++count;
    return count;
}

QList<Anchor> blockAnchors(const Section &section)
{
    QList<Anchor> blocks;
    for (const Anchor &anchor : section.anchors)
        if (anchor.block && (blocks.isEmpty() || anchor.source > blocks.last().source)) blocks.append(anchor);
    return blocks;
}
} // namespace

// The range of the source that a block of a section stands for, and the
// heights it is shown between: from where the block starts to where the
// next block starts, in the source and on the surface alike, so that what
// lies between two blocks, as a blank line does, belongs to the first.
// `at` is the block among the section's block anchors.
Preview::Span Preview::spanIn(size_t index, qsizetype at, const QList<Anchor> &blocks) const
{
    const size_t flow = flowCount(m_shown);
    const Section &section = m_shown[index];
    const int base = startOf(index);
    // The text starts at the top of the surface, whatever margin its first
    // block has above it.
    const auto topOf = [&](size_t which) {
        if (which == 0) return qreal(0);
        return which < m_shown.size() ? blockRect(startOf(which)).top() : m_text->documentLayout()->documentSize().height();
    };
    // Where the next section starts in the source; after the last, the end of the text.
    int nextStart = section.sourceEnd;
    if (index + 1 < flow) nextStart = m_shown[index + 1].sourceStart;
    else if (m_tree && !m_tree->files.empty()) nextStart = std::max(nextStart, int(m_tree->files.front().text.size()));
    Span span;
    span.valid = true;
    if (blocks.isEmpty()) {
        span.sourceStart = section.sourceStart;
        span.sourceEnd = nextStart;
        span.top = topOf(index);
        span.bottom = topOf(index + 1);
        return span;
    }
    span.sourceStart = section.sourceStart + (at == 0 ? 0 : blocks.at(at).source);
    span.top = at == 0 ? topOf(index) : blockRect(base + blocks.at(at).position).top();
    if (at + 1 < blocks.size()) {
        span.sourceEnd = section.sourceStart + blocks.at(at + 1).source;
        span.bottom = blockRect(base + blocks.at(at + 1).position).top();
    } else {
        span.sourceEnd = nextStart;
        span.bottom = topOf(index + 1);
    }
    return span;
}

Preview::Span Preview::spanOfSource(int source) const
{
    Span span;
    const size_t flow = flowCount(m_shown);
    if (!m_text || flow == 0) return span;
    // The last section that starts at or before the place.
    size_t low = 0, high = flow;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (m_shown[middle].sourceStart <= source) low = middle + 1;
        else high = middle;
    }
    if (low == 0) {
        // Before the first block, as metadata is: the top of the surface.
        span.valid = true;
        span.sourceEnd = m_shown[0].sourceStart;
        return span;
    }
    const size_t index = low - 1;
    const QList<Anchor> blocks = blockAnchors(m_shown[index]);
    const int relative = source - m_shown[index].sourceStart;
    qsizetype at = 0;
    while (at + 1 < blocks.size() && blocks.at(at + 1).source <= relative) ++at;
    return spanIn(index, at, blocks);
}

Preview::Span Preview::spanAtHeight(qreal y) const
{
    Span span;
    const size_t flow = flowCount(m_shown);
    if (!m_text || flow == 0) return span;
    const int position = std::max(1, m_text->documentLayout()->hitTest(QPointF(0, std::max<qreal>(0, y)), Qt::FuzzyHit));
    size_t index = sectionAt(position);
    // A section that holds nothing is passed over for the one that shows there.
    while (index + 1 < m_shown.size() && m_shown[index].length == 0) ++index;
    if (index >= flow) {
        // The notes and the bibliography come after all of the text.
        span.valid = true;
        span.sourceStart = span.sourceEnd = m_shown[flow - 1].sourceEnd;
        span.top = blockRect(startOf(flow)).top();
        span.bottom = m_text->documentLayout()->documentSize().height();
        return span;
    }
    const QList<Anchor> blocks = blockAnchors(m_shown[index]);
    const int relative = m_text->findBlock(position).position() - startOf(index);
    qsizetype at = 0;
    while (at + 1 < blocks.size() && blocks.at(at + 1).position <= relative) ++at;
    return spanIn(index, at, blocks);
}

qreal Preview::heightOfSource(int source) const
{
    const Span span = spanOfSource(source);
    if (!span.valid) return -1;
    if (span.sourceEnd <= span.sourceStart || span.bottom <= span.top) return span.top;
    const qreal part = std::clamp(qreal(source - span.sourceStart) / qreal(span.sourceEnd - span.sourceStart), qreal(0), qreal(1));
    return span.top + part * (span.bottom - span.top);
}

int Preview::sourceAtHeight(qreal y) const
{
    const Span span = spanAtHeight(y);
    if (!span.valid) return -1;
    if (span.bottom <= span.top || span.sourceEnd <= span.sourceStart) return span.sourceStart;
    const qreal part = std::clamp((y - span.top) / (span.bottom - span.top), qreal(0), qreal(1));
    return span.sourceStart + int(std::floor(part * (span.sourceEnd - span.sourceStart)));
}

int Preview::topSource() const
{
    const int source = sourceAtHeight(viewTop());
    return source < 0 ? 0 : source;
}

void Preview::showSource(int source)
{
    const qreal y = m_work.empty() ? heightOfSource(source) : -1;
    if (y < 0) {
        // Shown once it is built.
        m_pendingSource = source;
        return;
    }
    // The start of the text is shown with the room above it.
    if (y < 0.5 && m_viewport) m_viewport->setProperty("contentY", 0);
    else scrollTo(y);
}

bool Preview::goTo(const QString &name)
{
    if (name.isEmpty() || !m_text) return false;
    if (!m_work.empty() || m_shown.empty()) {
        m_pendingName = name;
        return true;
    }
    // "fnref-3-2" is the second place that refers to note 3.
    static const QRegularExpression again(QStringLiteral("^(fnref-\\d+)-(\\d+)$"));
    QString wanted = name;
    int occurrence = 1;
    if (const QRegularExpressionMatch match = again.match(name); match.hasMatch()) {
        wanted = match.captured(1);
        occurrence = std::max(1, match.captured(2).toInt());
    }
    int position = -1, seen = 0;
    for (size_t i = 0; i < m_shown.size() && position < 0; ++i)
        for (const preview::NamedAnchor &anchor : m_shown[i].names)
            if (anchor.name == wanted && ++seen == occurrence) {
                position = startOf(i) + anchor.position;
                break;
            }
    if (position < 0) return false;
    // The way back from a note returns the view to exactly where it was
    // left for the note.
    if (!m_trail.isEmpty() && wanted.startsWith(QLatin1String("fnref-"))
        && m_trailTarget == QStringLiteral("fn-") + wanted.mid(6)) {
        goBack();
        return true;
    }
    const QTextBlock block = m_text->findBlock(position);
    qreal y = m_text->documentLayout()->blockBoundingRect(block).top();
    if (block.layout() && position > block.position()) {
        const QTextLine line = block.layout()->lineForTextPosition(position - block.position());
        if (line.isValid()) y += line.y();
    }
    m_trail.append(viewTop());
    while (m_trail.size() > 64) m_trail.removeFirst();
    m_trailTarget = name;
    emit trailChanged();
    // A little above it, so that it does not sit against the edge.
    scrollTo(std::max<qreal>(0, y - m_style.size));
    return true;
}

void Preview::goBack()
{
    if (m_trail.isEmpty()) return;
    const qreal y = m_trail.takeLast();
    m_trailTarget.clear();
    emit trailChanged();
    scrollTo(y);
}

bool Preview::toggleTask(int source)
{
    if (!m_document || m_document->state() == Document::State::ReadOnly || m_document->isTextless()) return false;
    SourceBuffer *buffer = m_document->buffer();
    // Only a box that shows the text as it is can be ticked.
    if (m_shownRevision != buffer->revision() || !m_work.empty()) return false;
    const QSharedPointer<const SourceSnapshot> snapshot = buffer->snapshot();
    const QString &text = snapshot->text;
    if (source < 0 || source + 3 > text.size() || text.at(source) != u'[' || text.at(source + 2) != u']') return false;
    const QChar mark = text.at(source + 1);
    if (mark != u' ' && mark != u'x' && mark != u'X') return false;
    const QString next = mark == u' ' ? QStringLiteral("x") : QStringLiteral(" ");
    if (buffer->replace({source + 1, 1}, next) != SourceBuffer::EditOutcome::Applied) return false;
    emit taskToggled(source);
    return true;
}

bool Preview::activate(const QString &link, int position, quint64 island)
{
    if (link.isEmpty()) return false;
    if (link.startsWith(QLatin1String("parch:task/"))) {
        bool ok = false;
        const int offset = link.mid(11).toInt(&ok);
        const Section *section = island ? sectionOfIsland(island) : m_shown.empty() ? nullptr : &m_shown[sectionAt(position)];
        if (!ok || !section || section->sourceStart < 0) return false;
        return toggleTask(section->sourceStart + offset);
    }
    if (link.startsWith(u'#')) return goTo(link.mid(1));
    const QUrl url(link);
    switch (Backend::linkWay(url)) {
    case Backend::LinkWay::Refused:
        return false;
    case Backend::LinkWay::Open:
        return m_backend && m_backend->openLink(url);
    case Backend::LinkWay::Local: {
        const QString path = QDir::cleanPath(url.toLocalFile());
        if (readsAsMarkdown(path) && !QFileInfo(path).isDir()) {
            emit openRequested(path, url.fragment(QUrl::FullyDecoded));
            return true;
        }
        // Any other file, a script or a desktop entry as much as a picture,
        // is never handed to a program from a link: it is offered in its
        // folder, once asked.
        emit linkAsked(path, visibleText(m_backend ? m_backend->shownText(path) : path), QStringLiteral("file"), true);
        return true;
    }
    case Backend::LinkWay::Ask:
        emit linkAsked(url.toString(QUrl::FullyEncoded), shownLink(url), url.scheme().toLower(), false);
        return true;
    }
    return false;
}

QString Preview::shownLink(const QUrl &url)
{
    // Fully encoded, a character that would hide or reorder what is shown
    // stays a code, and a host is shown as the name it really asks for.
    QString text = visibleText(url.toString(QUrl::FullyEncoded));
    constexpr qsizetype Most = 400, Head = 300, Tail = 80;
    if (text.size() > Most) text = text.left(Head) + u'…' + text.right(Tail);
    return text;
}

int Preview::findHeading(const QString &slug)
{
    const int request = ++m_headingRequests;
    if (!m_document || slug.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, request, slug] { emit headingFound(request, slug, -1); }, Qt::QueuedConnection);
        return request;
    }
    // The document's own parse of the text as it is now, which a document
    // just opened, or shown as source alone, has once a worker has read it.
    const QPointer<Document> asked = m_document;
    // A text that cannot be read has no heading to find.
    m_document->parse()->whenParsed(
        this,
        [this, asked, request, slug](const ParsedTextPtr &parse) {
            if (!asked || asked != m_document) return;
            QtConcurrent::run([parse, slug] {
                try {
                    for (const Heading &heading : Outline::headings(parse->document(), parse->text))
                        if (heading.slug == slug) return heading.position;
                } catch (...) {
                }
                return -1;
            }).then(this, [this, asked, request, slug](int position) {
                if (asked && asked == m_document) emit headingFound(request, slug, position);
            });
        },
        [this, asked, request, slug](const QString &) {
            if (asked && asked == m_document) emit headingFound(request, slug, -1);
        });
    return request;
}

QString Preview::hintAt(qreal x, qreal y, quint64 island) const
{
    QTextDocument *document = island ? islandDocument(island) : m_text.data();
    if (!document) return {};
    // Only what the pointer is on, not what is nearest to it.
    return document->documentLayout()->formatAt(QPointF(x, y)).toCharFormat().toolTip();
}

// --- Copying -----------------------------------------------------------------------------

namespace {

// The text of a table, a line for each row, its cells apart by tabs.
QString tableText(QTextTable *table)
{
    QStringList lines;
    for (int row = 0; row < table->rows(); ++row) {
        QStringList cells;
        for (int column = 0; column < table->columns(); ++column) {
            const QTextTableCell cell = table->cellAt(row, column);
            if (cell.row() != row || cell.column() != column) {
                if (cell.row() == row) continue;
                cells << QString();
                continue;
            }
            QTextCursor cursor = cell.firstCursorPosition();
            cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
            cells << cursor.selectedText().replace(QChar::ParagraphSeparator, u' ').replace(QChar::LineSeparator, u' ');
        }
        lines << cells.join(u'\t');
    }
    return lines.join(u'\n');
}

QString islandText(QTextDocument *document)
{
    if (QTextTable *table = tableOf(document)) return tableText(table);
    return document->toPlainText();
}

// The text of a part of a block, with what stands for a picture in its place.
QString blockText(const QTextBlock &block, int from, int to)
{
    QString text;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        const int start = std::max(from, fragment.position()), end = std::min(to, fragment.position() + fragment.length());
        if (start >= end) continue;
        const QTextCharFormat format = fragment.charFormat();
        // A place to break that the preview put in a long word is not text.
        if (format.boolProperty(preview::BreakProperty)) continue;
        if (format.isImageFormat()) {
            const QString name = format.toImageFormat().name();
            for (int i = start; i < end; ++i) {
                if (name == preview::Builder::taskImageName(true)) text += QStringLiteral("[x]");
                else if (name == preview::Builder::taskImageName(false)) text += QStringLiteral("[ ]");
                else text += format.toolTip();
            }
            continue;
        }
        text += fragment.text().mid(start - fragment.position(), end - start);
    }
    text.replace(QChar::LineSeparator, u'\n');
    text.replace(QChar(0x2002), u' ');
    text.replace(QChar::Nbsp, u' ');
    return text;
}

// A character format with what a reader elsewhere can use: no colours,
// faces or sizes of this desktop.
QTextCharFormat portable(const QTextCharFormat &format, const QString &codeFamily)
{
    QTextCharFormat clean;
    if (format.fontWeight() >= QFont::DemiBold) clean.setFontWeight(QFont::Bold);
    if (format.fontItalic()) clean.setFontItalic(true);
    if (format.fontUnderline()) clean.setFontUnderline(true);
    if (format.fontStrikeOut()) clean.setFontStrikeOut(true);
    if (format.verticalAlignment() != QTextCharFormat::AlignNormal) clean.setVerticalAlignment(format.verticalAlignment());
    const QStringList families = format.fontFamilies().toStringList();
    if (families.contains(codeFamily) || families.contains(QStringLiteral("monospace"))
        || format.intProperty(PreviewStyle::PaperProperty) == PreviewStyle::CodePaper)
        clean.setFontFamilies({QStringLiteral("monospace")});
    if (format.isAnchor() && !format.anchorHref().startsWith(QLatin1String("parch:")) && !format.anchorHref().startsWith(u'#')) {
        clean.setAnchor(true);
        clean.setAnchorHref(format.anchorHref());
    }
    return clean;
}

} // namespace

QStringList Preview::copied(QQuickItem *textEdit) const
{
    QTextDocument *document = documentOf(textEdit);
    if (!document) return {};
    const int start = textEdit->property("selectionStart").toInt(), end = textEdit->property("selectionEnd").toInt();
    if (end <= start) return {};
    const bool main = document == m_text;
    QStringList lines;
    QTextDocument rich;
    QTextCursor out(&rich);
    bool first = true;
    const auto place = [&](const QTextDocumentFragment &fragment) {
        if (!first) out.insertBlock(QTextBlockFormat(), QTextCharFormat());
        first = false;
        out.insertFragment(fragment);
    };
    for (QTextBlock block = document->findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
        const int from = std::max(start, block.position());
        const int to = std::min(end, block.position() + block.length() - 1);
        const QTextBlockFormat format = block.blockFormat();
        // An island's block stands for the island: all of its text is copied.
        if (main && format.hasProperty(preview::IslandProperty)) {
            const size_t index = sectionAt(block.position());
            const int number = format.intProperty(preview::IslandProperty);
            if (index < m_shown.size() && number >= 1 && number <= int(m_shown[index].islands.size())) {
                QTextDocument *island = m_shown[index].islands[size_t(number - 1)].document;
                if (island) {
                    lines << islandText(island) << QString();
                    place(QTextDocumentFragment(island));
                }
            }
            continue;
        }
        if (to < from) continue;
        const QString text = blockText(block, from, to);
        // The blocks that are empty to hold a rule's place are not text.
        if (text.isEmpty() && format.lineHeightType() == QTextBlockFormat::FixedHeight) continue;
        lines << text;
        // A paragraph's end, and a heading's, is a blank line; the lines of
        // a list stay together.
        if (format.bottomMargin() >= m_style.gap() * 0.6 || format.headingLevel() > 0) lines << QString();
        if (!first) out.insertBlock();
        first = false;
        QTextBlockFormat kept;
        if (format.headingLevel() > 0) kept.setHeadingLevel(format.headingLevel());
        if (format.leftMargin() > 0) kept.setLeftMargin(format.leftMargin());
        out.setBlockFormat(kept);
        // Run by run, in formats a reader elsewhere can use.
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int a = std::max(from, fragment.position()), b = std::min(to, fragment.position() + fragment.length());
            if (a >= b) continue;
            const QTextCharFormat charFormat = fragment.charFormat();
            if (charFormat.boolProperty(preview::BreakProperty)) continue;
            QString piece;
            if (charFormat.isImageFormat()) {
                const QString name = charFormat.toImageFormat().name();
                piece = name == preview::Builder::taskImageName(true)    ? QStringLiteral("[x]")
                        : name == preview::Builder::taskImageName(false) ? QStringLiteral("[ ]")
                                                                         : charFormat.toolTip();
            } else {
                piece = fragment.text().mid(a - fragment.position(), b - a);
            }
            QTextCharFormat clean = portable(charFormat, m_style.codeFamily);
            if (format.headingLevel() > 0) clean.setFontWeight(QFont::Bold);
            out.insertText(piece, clean);
        }
    }
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    // What came from an island still has this desktop's colours and faces,
    // and pictures only this preview can show: those go too.
    struct Run {
        int position;
        int length;
        QTextCharFormat format;
    };
    std::vector<Run> runs;
    for (QTextBlock block = rich.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            runs.push_back({it.fragment().position(), it.fragment().length(), it.fragment().charFormat()});
    for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
        out.setPosition(run->position);
        out.setPosition(run->position + run->length, QTextCursor::KeepAnchor);
        if (run->format.isImageFormat()) out.insertText(run->format.toolTip(), QTextCharFormat());
        else out.setCharFormat(portable(run->format, m_style.codeFamily));
    }
    return {lines.join(u'\n'), rich.toHtml()};
}

void Preview::copy(QQuickItem *textEdit)
{
    const QStringList text = copied(textEdit);
    if (text.size() != 2 || text.at(0).isEmpty()) return;
    auto *data = new QMimeData;
    data->setText(text.at(0));
    data->setHtml(text.at(1));
    QGuiApplication::clipboard()->setMimeData(data);
}
