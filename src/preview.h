#pragma once

// The backend, the document and the theme are properties QML sets, so their
// types must be complete here.
#include "backend.h"
#include "document.h"
#include "documentparse.h"
#include "previewbuilder.h"
#include "previewstyle.h"
#include "sourcetypes.h"
#include "theme.h"

#include <QAbstractListModel>
#include <QColor>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSet>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimer>
#include <memory>
#include <vector>

class PreviewResources;
class PreviewWatch;
class QTextDocument;
class QTextTable;

// What a preview places over its text, for the part of the document that is
// in view or near it: the bars beside quotes, rules, the islands (tables,
// code blocks and display math, each a small document a view shows with its
// own sideways scrolling), and, drawn as nothing, the headings and pictures
// that assistive technology is told of. A row keeps its place in the list
// for as long as what it stands for is near the view, so that a view's item
// for it lives that long too.
class PreviewMarks : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        KindRole = Qt::UserRole + 1, // "bar", "rule", "notesRule", "pageBreak", "heading", "image", "island"
        RectRole,                    // where it is, in the surface's coordinates
        TextRole,                    // a heading's text, a picture's description, what an island is
        LevelRole,                   // a heading's level
        IslandRole,                  // the island's id, for Preview::adopt()
        IslandKindRole,              // "code", "table", "math"
        WrapsRole,                   // the island is laid out contentWidth wide
        ContentWidthRole,
        ContentHeightRole,
        CenteredRole,
        TintedRole,
        RulesRole,                   // a table's lines between rows, from its top
        HeaderRuleRole,              // which of them ends the header
        PositionRole,                // an island's place in the surface's text
    };

    struct Row {
        quint64 key = 0;
        QString kind;
        QRectF rect;
        QString text;
        int level = 0;
        quint64 island = 0;
        QString islandKind;
        bool wraps = false;
        qreal contentWidth = 0;
        qreal contentHeight = 0;
        bool centered = false;
        bool tinted = false;
        QList<qreal> rules;
        int headerRule = -1;
        int position = 0;
        bool operator==(const Row &) const = default;
    };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    // Takes the rows now near the view: rows that stay keep their place.
    void show(const QList<Row> &rows);
    const QList<Row> &rows() const { return m_rows; }

private:
    QList<Row> m_rows;
};

// The preview of one view of a document: it keeps a read-only TextEdit (its
// surface) showing what the document reads as, built from the Markdown
// engine's tree by preview::Builder in a style taken from the live theme.
//
// It follows the document off the typing path. Edits wait for a pause in
// typing, or while typing goes on for MaxWaitMs at most; then the
// document's parse of the text as it is then (DocumentParse, shared by
// every view of the document) is read on a worker, and the result is taken
// if the text has not changed since, or, while typing has gone on past
// MaxWaitMs, if it is newer than what is shown; older text is never shown
// over newer. Of the result, only the parts whose key changed
// are built again and put in place of the old ones, so the rest of the
// surface, its selection and its scroll position stay as they were. A large
// change is built a slice at a time, between frames.
//
// It knows where everything it shows came from, so that the editor and the
// preview can find each other: by position, for a click, and by height, for
// scrolling together.
class Preview : public QObject {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    // The render service, the opener of links and the fetcher of pictures are the backend's.
    Q_PROPERTY(Backend *backend READ backend WRITE setBackend NOTIFY backendChanged)
    Q_PROPERTY(Theme *theme READ theme WRITE setTheme NOTIFY themeChanged)
    // Nothing is parsed or built while it is not active, as while no view shows it.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // The TextEdit that shows the text, and the Flickable it scrolls in.
    Q_PROPERTY(QQuickItem *surface READ surface WRITE setSurface NOTIFY surfaceChanged)
    Q_PROPERTY(QQuickItem *viewport READ viewport WRITE setViewport NOTIFY viewportChanged)
    // The reading settings: the face, empty for the default one, the size of
    // body text in pixels, and the line height as a multiple of it.
    Q_PROPERTY(QString family READ family WRITE setFamily NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal lineHeight READ lineHeight WRITE setLineHeight NOTIFY typographyChanged)
    // Device pixels per logical pixel, for the pictures it draws.
    Q_PROPERTY(qreal scale READ scale WRITE setScale NOTIFY typographyChanged)
    Q_PROPERTY(bool remoteImages READ remoteImages WRITE setRemoteImages NOTIFY remoteImagesChanged)

    // What the surface and the islands are drawn with.
    Q_PROPERTY(QString bodyFamily READ bodyFamily NOTIFY styleChanged)
    Q_PROPERTY(int bodySize READ bodySize NOTIFY styleChanged)
    Q_PROPERTY(QString codeFamily READ codeFamily NOTIFY styleChanged)
    Q_PROPERTY(int codeSize READ codeSize NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor READ textColor NOTIFY styleChanged)
    Q_PROPERTY(QColor dimColor READ dimColor NOTIFY styleChanged)
    Q_PROPERTY(QColor codeTint READ codeTint NOTIFY styleChanged)
    Q_PROPERTY(QColor ruleColor READ ruleColor NOTIFY styleChanged)
    Q_PROPERTY(QColor barColor READ barColor NOTIFY styleChanged)
    Q_PROPERTY(qreal padding READ padding NOTIFY styleChanged)
    Q_PROPERTY(qreal tabWidth READ tabWidth NOTIFY styleChanged)

    Q_PROPERTY(QAbstractItemModel *marks READ marks CONSTANT)
    // What it shows is the text as it is now.
    Q_PROPERTY(bool current READ current NOTIFY currentChanged)
    // It has been reading and setting the text for longer than a moment
    // without catching up, which the view says.
    Q_PROPERTY(bool late READ late NOTIFY lateChanged)
    // There is a place to go back to, after a link inside the document was followed.
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY trailChanged)
    // What the text as last read marks as leading nowhere (preview::Unresolved),
    // a sentence each, for assistive technology; empty when nothing does.
    Q_PROPERTY(QString unresolved READ unresolved NOTIFY unresolvedChanged)
    // The text as last read names pictures on the web, whether or not they are shown.
    Q_PROPERTY(bool webPictures READ webPictures NOTIFY webPicturesChanged)
    // Why the text as it is now could not be read for the preview, as when
    // memory ran out; empty when it was.
    Q_PROPERTY(QString failure READ failure NOTIFY failureChanged)

public:
    // How long typing pauses before the text is read again: keys typed one
    // after another are drawn without the preview's work in their frames.
    static constexpr int PauseMs = 180;
    // While typing goes on, the preview reads the text at the latest this
    // long after the first edit it has not read, and so keeps up.
    static constexpr int MaxWaitMs = 750;
    // Reading and setting the text that takes longer than this is late.
    static constexpr int LateMs = 300;
    // How long a slice of building, with the layout of what it built, is
    // meant to take before the rest waits for the next turn of the event loop.
    static constexpr int SliceMs = 8;
    // How long a preview nobody sees keeps what it built.
    static constexpr int ReleaseMs = 60000;
    // A text longer than this, on the surface or in an island, is drawn
    // only where it is in view, as Qt draws a TextEdit's text property of
    // that length: the scene then holds what a view shows, however long the
    // document is.
    static constexpr int LargeTextUnits = 10000;

    explicit Preview(QObject *parent = nullptr);
    ~Preview() override;

    Document *document() const { return m_document; }
    void setDocument(Document *document);
    Backend *backend() const { return m_backend; }
    void setBackend(Backend *backend);
    Theme *theme() const { return m_theme; }
    void setTheme(Theme *theme);
    bool active() const { return m_active; }
    void setActive(bool active);
    QQuickItem *surface() const { return m_surface; }
    void setSurface(QQuickItem *surface);
    QQuickItem *viewport() const { return m_viewport; }
    void setViewport(QQuickItem *viewport);
    QString family() const { return m_family; }
    void setFamily(const QString &family);
    qreal fontSize() const { return m_fontSize; }
    void setFontSize(qreal size);
    qreal lineHeight() const { return m_lineHeight; }
    void setLineHeight(qreal lineHeight);
    qreal scale() const { return m_scale; }
    void setScale(qreal scale);
    bool remoteImages() const { return m_remoteImages; }
    void setRemoteImages(bool load);

    QString bodyFamily() const { return m_style.family; }
    int bodySize() const { return qRound(m_style.size); }
    QString codeFamily() const { return m_style.codeFamily; }
    int codeSize() const { return qRound(m_style.codeSize()); }
    QColor textColor() const { return m_style.text; }
    QColor dimColor() const { return m_style.ink(PreviewStyle::Dim); }
    QColor codeTint() const { return m_style.paper(PreviewStyle::CodePaper); }
    QColor ruleColor() const { return m_style.rule; }
    QColor barColor() const { return m_style.quoteBar; }
    qreal padding() const { return m_style.padding(); }
    qreal tabWidth() const;

    QAbstractItemModel *marks() const { return m_marks; }
    bool current() const { return m_current; }
    bool late() const { return m_late; }
    bool canGoBack() const { return !m_trail.isEmpty(); }
    QString unresolved() const { return m_unresolved; }
    bool webPictures() const { return m_webPictures; }
    QString failure() const { return m_failure; }

    // The document as it is built, its style, and how much it has built: for tests.
    QTextDocument *textDocument() const { return m_text; }
    const PreviewStyle &style() const { return m_style; }
    const std::vector<preview::Section> &sections() const { return m_shown; }
    QTextDocument *islandDocument(quint64 island) const;
    Revision shownRevision() const { return m_shownRevision; }
    // How many sections have been built since the preview was made, how many
    // times a result was taken, and how many were dropped as stale.
    int sectionsBuilt() const { return m_built; }
    int updates() const { return m_updates; }
    int staleResults() const { return m_stale; }
    // The longest any one slice of work held the thread, in milliseconds.
    qreal longestSlice() const { return m_longestSlice; }
    void resetLongestSlice() { m_longestSlice = 0; }
    PreviewResources *resources() const { return m_resources; }
    // The files on disk the text as last read depends on, which are watched.
    QStringList watchedFiles() const;

    // A view's TextEdit takes an island's document to show it.
    Q_INVOKABLE void adopt(quint64 island, QQuickItem *textEdit);

    // Where a position of the surface, or of an island, came from in the
    // source; -1 when it came from none.
    Q_INVOKABLE int sourceAt(int position, quint64 island = 0) const;
    // Where a position came from when an include brought it in: {path,
    // line, column} of its place in the included file, lines and columns
    // from 0; empty for the document's own text.
    Q_INVOKABLE QVariantMap includedAt(int position, quint64 island = 0) const;
    // The source shown at the top of the view, and showing a source position there.
    Q_INVOKABLE int topSource() const;
    Q_INVOKABLE void showSource(int source);
    // The height, in the surface's coordinates, at which a source position
    // is shown, and the source position shown at a height: both run evenly
    // between the starts of two blocks. -1 while nothing is built.
    Q_INVOKABLE qreal heightOfSource(int source) const;
    Q_INVOKABLE int sourceAtHeight(qreal y) const;
    // The same, as the two block starts around the place: the source range
    // [sourceStart, sourceEnd) is shown from `top` to `bottom`.
    struct Span {
        bool valid = false;
        int sourceStart = 0;
        int sourceEnd = 0;
        qreal top = 0;
        qreal bottom = 0;
    };
    Span spanOfSource(int source) const;
    Span spanAtHeight(qreal y) const;

    // Follows a link at a position of the surface or of an island: a place
    // in this document is scrolled to, a task's box is ticked in the source,
    // a document is opened, a web or mail address goes to the desktop's
    // handler, and anything else asks first (linkAsked()), as
    // Backend::linkWay() says. True when the link was taken.
    Q_INVOKABLE bool activate(const QString &link, int position, quint64 island = 0);
    // An address as a question about opening it shows it: every character
    // that cannot be seen by its code, and a very long one shortened in the
    // middle, so that its scheme and host are always in view.
    static QString shownLink(const QUrl &url);
    // Scrolls to a heading's slug, a note or a reference; false for a name
    // nothing has.
    Q_INVOKABLE bool goTo(const QString &name);
    // Back to where the view was before the last link followed inside the document.
    Q_INVOKABLE void goBack();
    // Puts the selection of a TextEdit that shows the surface or an island
    // on the clipboard, as plain text and as HTML.
    Q_INVOKABLE void copy(QQuickItem *textEdit);
    // The same as text, for tests: {plain, html}.
    QStringList copied(QQuickItem *textEdit) const;
    // Finds where the line of the heading with a slug starts in the
    // document's text as it is now, by the outline of the document's shared
    // parse, off the UI thread, and says headingFound() with the number this
    // returns: -1 when it has no such heading. Nothing is said when the
    // preview shows another document by then.
    Q_INVOKABLE int findHeading(const QString &slug);
    // What the text at a point of the surface, or of an island, says when
    // pointed at: where a link leads, what a picture shows, why a formula
    // could not be drawn. Empty where there is nothing to say.
    Q_INVOKABLE QString hintAt(qreal x, qreal y, quint64 island = 0) const;
    // The top of the view in the surface's coordinates, and scrolling so
    // that a height of the surface is at the top of the view.
    qreal viewTop() const;
    void scrollTo(qreal y);
    // Whether building is still going on, a slice at a time, and whether a
    // worker is reading the text.
    bool working() const { return !m_work.empty(); }
    bool parsing() const { return m_parsing; }
    // Makes every worker take at least this long over a text, so that a
    // test can edit the text while it is being read.
    static void setWorkerDelay(int milliseconds);

signals:
    void documentChanged();
    void backendChanged();
    void themeChanged();
    void activeChanged();
    void surfaceChanged();
    void viewportChanged();
    void typographyChanged();
    void remoteImagesChanged();
    void styleChanged();
    void currentChanged();
    void lateChanged();
    void trailChanged();
    void unresolvedChanged();
    void webPicturesChanged();
    void failureChanged();
    // The surface shows other text than before: a result was taken, or a picture arrived.
    void updated();
    // The layout of what is shown changed without its text changing.
    void laidOut();
    // A task was ticked or cleared in the source, at this offset.
    void taskToggled(int source);
    // A document named by a link is to be shown, at the heading with this
    // slug when the link names one.
    void openRequested(const QString &path, const QString &heading);
    // A link leads out of parch in a way the writer is asked about first:
    // `target` is the address to open, or for a file on this computer
    // (`local`) its path, to show in its folder; `shown` is how the
    // question shows it.
    void linkAsked(const QString &target, const QString &shown, const QString &scheme, bool local);
    // What findHeading() found, for the request it numbered.
    void headingFound(int request, const QString &slug, int position);

private slots:
    void viewMoved();

private:
    struct Parsed;
    struct Entry;
    struct Operation;
    struct IslandState;
    struct TopAnchor;

    static Parsed parseJob(ParsedTextPtr parse, quint64 generation);
    void attach();
    void edited();
    void dialectChanged();
    void schedule(int delayMs);
    // Reads the text once typing pauses, or once the first edit not read
    // has waited MaxWaitMs.
    void waitForPause();
    void setLate(bool late);
    void parse();
    // Reads the document's parse of the text on the worker.
    void read(const ParsedTextPtr &parse);
    void take(const Parsed &result);
    // The text could not be read for the preview: it says why, and is
    // ready for the next text.
    void failed(const QString &reason);
    void plan(const Parsed &result);
    void work();
    void finishWork();
    void rebuildAll();
    void rebuildPictures();
    void clear();
    void removeSections(QTextCursor &cursor, size_t at, size_t count);
    void insertSection(QTextCursor &cursor, preview::Builder &builder, size_t at, const preview::Part &part);
    void dropIslands(size_t index);
    void connectServices();
    void trimTop(QTextCursor &cursor);
    // Has the surface draw only what is in view while its text is long.
    void drawInView();
    // Has the surface draw what is in view anew once the next frame is out.
    void redrawAfterFrame();

    int startOf(size_t index) const;
    size_t sectionAt(int position) const;
    void invalidateStarts(size_t from);

    PreviewStyle makeStyle() const;
    void restyle();
    void recolor(const PreviewStyle &colors);
    void applyColors(QTextDocument *document) const;
    void prepareDocument();

    void columnChanged();
    void layoutIslands(size_t first, size_t last, QTextCursor *cursor);
    void measureIsland(preview::Island &island, IslandState &state) const;
    void refreshMarks();
    qreal columnWidth() const;
    QRectF blockRect(int position) const;
    qreal blockBottom(int position) const;
    qreal viewHeight() const;
    TopAnchor captureTop() const;
    void restoreTop(const TopAnchor &anchor);

    const preview::Section *sectionOfIsland(quint64 island, const preview::Island **found = nullptr,
                                            size_t *index = nullptr) const;
    Span spanIn(size_t index, qsizetype at, const QList<preview::Anchor> &blocks) const;
    bool toggleTask(int source);
    void setCurrent(bool current);
    void setUnresolved(const QStringList &messages);
    void setWebPictures(bool web);
    void pictureChanged(const QString &key);
    // A file the text depends on changed on disk: a picture is read again,
    // and an included file, a bibliography or a style has the text read again.
    void dependencyChanged(const QString &path);

    QPointer<Document> m_document;
    QPointer<Backend> m_backend;
    QPointer<Theme> m_theme;
    QPointer<QQuickItem> m_surface;
    QPointer<QQuickItem> m_viewport;
    QPointer<QTextDocument> m_text;
    QMetaObject::Connection m_edits;
    QMetaObject::Connection m_dialect;
    QMetaObject::Connection m_parses;
    // A read waits for the document's parse of the text as it is now.
    bool m_awaiting = false;
    bool m_active = false;
    // The last findHeading() request's number.
    int m_headingRequests = 0;
    QString m_family;
    qreal m_fontSize = 18;
    qreal m_lineHeight = 1.6;
    qreal m_scale = 1;
    bool m_remoteImages = false;
    PreviewStyle m_style;

    PreviewResources *m_resources;
    // The files the text as last read depends on, watched: its local
    // pictures, as the builder finds them, and what it reads besides.
    PreviewWatch *m_watch;
    QSet<QString> m_pictureFiles, m_readFiles;
    PreviewMarks *m_marks;
    QThreadPool m_pool;
    QTimer m_timer;
    QTimer m_release;
    // Since the first edit that no read has begun with; invalid when none waits.
    QElapsedTimer m_waiting;
    // Since the first edit the read under way takes in.
    QElapsedTimer m_readWaiting;
    QTimer m_lateTimer;
    bool m_late = false;
    bool m_parsing = false;
    bool m_parseAgain = false;
    // Results of what was asked for another document, before a clear, or in
    // settings the document has since left, are not taken.
    quint64 m_generation = 0;

    // What the surface holds, section for section, with the part each was
    // built from, which points into m_tree.
    std::vector<preview::Section> m_shown;
    std::vector<preview::Part> m_parts;
    std::vector<quint64> m_ids;
    std::shared_ptr<const md::Document> m_tree;
    // The tree's citations as the document's citation style formats them.
    std::shared_ptr<const DocumentCitations::Formatted> m_citations;
    // The settings the document is read in changed since it was last read.
    bool m_reread = false;
    // Every part of the text as it was last read: what m_shown becomes once
    // the work that is waiting is done.
    std::shared_ptr<const std::vector<preview::Part>> m_target;
    mutable std::vector<int> m_starts;
    mutable size_t m_startsValid = 0;
    std::vector<Operation> m_work;
    QSet<quint64> m_dirty;
    QTimer m_dirtyTimer;
    Revision m_planRevision = 0;
    Revision m_shownRevision = 0;
    bool m_current = false;
    QString m_unresolved;
    bool m_webPictures = false;
    QString m_failure;
    QMetaObject::Connection m_parseFailures;
    quint64 m_nextId = 1;
    // The section whose first block was given no room above it.
    quint64 m_trimmed = 0;
    // How long a slice builds for, so that with the layout of what it built
    // it stays within SliceMs.
    qreal m_buildMs = 2;
    QHash<quint64, IslandState> m_islands;
    qreal m_column = 0;
    // A source position to show once it is built.
    int m_pendingSource = -1;
    QString m_pendingName;
    QList<qreal> m_trail;
    QString m_trailTarget;
    int m_built = 0;
    int m_updates = 0;
    int m_stale = 0;
    qreal m_longestSlice = 0;
    bool m_refreshing = false;
    // Where the first block in view started when the work began, and
    // whether the work took it away.
    int m_viewStart = -1;
    bool m_viewRemoved = false;
    QMetaObject::Connection m_frameSwapped;
};
