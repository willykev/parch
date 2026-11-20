#pragma once

#include "editorbinding.h"
#include "sourcetypes.h"

#include <QAbstractListModel>
#include <QColor>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <deque>
#include <vector>

class QTextDocument;

// A run of the source a layer formats: UTF-16 offsets [start, end), and which
// of the layer's styles it takes.
struct DecorationSpan {
    int start = 0;
    int end = 0;
    int style = 0;
    bool operator==(const DecorationSpan &) const = default;
};

// How a layer of decorations combines with the others and follows edits.
struct DecorationLayerOptions {
    // Layers with a higher z are merged over those with a lower.
    int z = 0;
    // An edit removes the ranges it touches, rather than moving them, for a
    // layer whose ranges describe exactly the text they cover.
    bool dropEdited = false;
    // By style: whether a range of that style takes what is typed just
    // inside either of its ends, as text typed at the end of a heading goes
    // on being the heading's. Where one range ends and the next starts, the
    // next takes the typing if it may, and otherwise the one that ends.
    // Styles not listed take nothing there; a dropEdited layer never does.
    QList<bool> growAtEdges = {};
};

// The wavy underlines in and near the view, where the TextEdit shows them:
// in its own coordinates, one row for each line a wavy run covers. Qt Quick
// draws every underline straight, so the view draws these itself.
class DecorationMarks : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { XRole = Qt::UserRole + 1, YRole, WidthRole, HeightRole, ColorRole };
    struct Mark {
        qreal x = 0, y = 0, width = 0, height = 0;
        QColor color;
        bool operator==(const Mark &) const = default;
    };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    // Takes these marks, changing only the rows that differ.
    void update(const std::vector<Mark> &marks);
    const std::vector<Mark> &marks() const { return m_marks; }

private:
    std::vector<Mark> m_marks;
};

// The formatting one view draws over its text. Its layers are named sets of
// source ranges, each range with one of its layer's character formats; the
// layers are combined in the order of their z, a later one's format merged
// over an earlier one's, and each paragraph is formatted as a whole whenever
// Qt lays it out. Every range is a source range, mapped to the view's
// positions through its EditorBinding, CRLF pairs included.
//
// A layer's ranges follow every edit of the source as text attributes do: a
// range grows with what is typed inside it, not at its ends unless its style
// grows at its edges, and moves with what is typed before it; a layer can
// instead drop the ranges an edit touches. Its owner, usually working on a
// worker thread, replaces its ranges, or those within one part of the
// source, with what it found for a source revision. A result for an older revision still applies where no
// edit since has touched the part it covers, moved by the edits before that
// part; otherwise it is stale and dropped, and the owner looks again.
//
// Formatting changes the look of the text and nothing else: never the text,
// the line height the binding sets, or where the caret and the selection
// are. A layer's formats lose whatever sets the font's family, size,
// spacing, stretch or vertical alignment. A wavy underline is not drawn by
// Qt but listed in `waves` for the view to draw.
//
// Focus dims all of the text but one range: the view draws its text in the
// dim colour while focusing, and this formats the range in the normal one,
// and the colours of other layers outside the range in the dim one.
//
// Changing a layer formats again only the paragraphs whose formatting
// changed: those in view at once, the rest a few at a time while the event
// loop is otherwise idle, so that no change of a long document holds up a
// frame.
class EditorDecorations : public QSyntaxHighlighter {
    Q_OBJECT
    Q_PROPERTY(EditorBinding *binding READ binding WRITE setBinding NOTIFY bindingChanged)
    Q_PROPERTY(QAbstractItemModel *waves READ waves CONSTANT)
    Q_PROPERTY(bool focusing READ focusing NOTIFY focusChanged)

public:
    // Off-screen paragraphs are formatted in slices of about this long.
    static constexpr int SliceMs = 4;

    using LayerOptions = DecorationLayerOptions;

    explicit EditorDecorations(QObject *parent = nullptr);
    ~EditorDecorations() override;

    EditorBinding *binding() const;
    void setBinding(EditorBinding *binding);
    QAbstractItemModel *waves() const;
    const DecorationMarks *marks() const { return m_waves; }

    // Makes a layer, or gives an existing one new formats and options. Its
    // ranges are kept; every paragraph they cover is formatted again.
    void defineLayer(const QString &name, const QList<QTextCharFormat> &styles, LayerOptions options = {});
    bool hasLayer(const QString &name) const;
    // The source revision the layers describe: the newest this view has shown.
    Revision revision() const { return m_revision; }
    // Replaces the layer's ranges within `range` with `spans`, all as they
    // were at `revision`; ranges straddling its edges keep their parts
    // outside it. Returns false when the result is stale, or the layer is
    // not defined, and nothing changed.
    bool replaceSpans(const QString &name, Revision revision, SourceRange range, std::vector<DecorationSpan> spans);
    // The same, for the whole source.
    bool setSpans(const QString &name, Revision revision, std::vector<DecorationSpan> spans);
    void clearLayer(const QString &name);
    // The layer's ranges now, in order.
    std::vector<DecorationSpan> spans(const QString &name) const;
    // The layer's range that holds `position`, its end included; null when none does.
    const DecorationSpan *spanAt(const QString &name, int position) const;
    // Where a range of the source as it was at `revision` is now, moved over
    // the edits made since; false, and the range unchanged, when one of them
    // touched it, at its edges too, or the edits are no longer remembered.
    bool follow(Revision revision, SourceRange *range) const;

    // Dims everything but `range`, in the colours given, or stops dimming.
    void setFocus(SourceRange range);
    void clearFocus();
    void setFocusColors(const QColor &dim, const QColor &normal);
    bool focusing() const { return m_focusing; }
    SourceRange focusRange() const { return {m_focus.start, m_focus.end - m_focus.start}; }

    // The formats the layers give a run of the source, as ranges counted
    // from its start: what Qt draws a paragraph with, and a table's cell.
    QList<QTextLayout::FormatRange> formatsFor(int sourceStart, int length) const;
    // The wavy underlines over a run of the source, in source offsets.
    struct Wave {
        int start = 0;
        int end = 0;
        QColor color;
    };
    QList<Wave> wavesFor(int sourceStart, int length) const;

    // How many paragraphs have been formatted since this was made; for
    // checking that a change formats only what it changes.
    int formattedParagraphs() const { return m_formatted; }
    // Paragraphs wait to be formatted again, off-screen.
    bool pending() const { return !m_dirty.empty(); }
    // Formats every waiting paragraph now.
    void flush();
    // Paragraphs of the source that were hidden and are shown again, which
    // were left unformatted while hidden: they are formatted again.
    void reshown(int start, int end);

public slots:
    // The view scrolled or changed size.
    void viewMoved();

signals:
    void bindingChanged();
    void focusChanged();
    // The view shows its document anew, after edits this did not see or for
    // the first time: the layers were cleared, and their owners fill them.
    void layersReset();
    // What the layers give the source from `start` to `end` changed, as a
    // grid's cells there are to show.
    void formatsChanged(int start, int end);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Layer {
        QString name;
        LayerOptions options;
        // What Qt draws for each style, and the colour of its wavy underline, if it has one.
        QList<QTextCharFormat> drawn;
        QList<QColor> waves;
        std::vector<DecorationSpan> spans;
        bool grows(int style) const
        {
            return !options.dropEdited && style >= 0 && style < options.growAtEdges.size() && options.growAtEdges[style];
        }
    };
    struct Edit {
        Revision revision;
        int start, removed, inserted;
    };
    struct Range {
        int start, end;
    };

    void attach();
    bool ready() const;
    // The source in view, as the binding last said; asked again once the
    // view moves, the text changes or is laid out again.
    SourceRange visible();
    void projectionLoaded();
    void sourceEdited(Revision revision, SourceRange range, int inserted);
    Layer *layer(const QString &name);
    const Layer *layer(const QString &name) const;
    // Moves `range` over the edits made since `revision`; false when one of them touches it.
    bool forward(Revision revision, Range &range, int &shift) const;
    // Paragraphs covering this source range are to be formatted again.
    void markDirty(int start, int end);
    // Formats the waiting paragraphs in view, and starts the slices for the rest.
    void schedule();
    void formatVisible();
    void formatSlice();
    // Formats the paragraphs of each range, in order, each paragraph once,
    // and takes those done from the front; with a budget, until it is spent.
    void formatRanges(std::vector<Range> &ranges, const QElapsedTimer *budget);
    // Formats the paragraphs of a source range now; false if the budget ran
    // out first. `reached` is where the paragraphs formatted end.
    bool format(Range range, const QElapsedTimer *budget, int *reached);
    int sourceLength() const;
    // Some layer has a wavy underline over some of the text.
    bool wavy() const;
    void refreshWaves();
    void scheduleWaves();

    QPointer<EditorBinding> m_binding;
    QPointer<QTextDocument> m_document;
    QList<QMetaObject::Connection> m_connections, m_documentConnections;
    std::vector<Layer> m_layers;
    std::deque<Edit> m_edits;
    Revision m_revision = 0;
    bool m_loaded = false;
    // Source ranges whose paragraphs wait to be formatted again, in order and apart.
    std::vector<Range> m_dirty;
    QTimer m_slices;
    // Paragraphs are being formatted, which can make the view report a move.
    bool m_formatting = false;
    SourceRange m_visible{0, 0};
    bool m_visibleKnown = false;
    // The document the layers describe.
    const void *m_shown = nullptr;
    bool m_focusing = false;
    Range m_focus{0, 0};
    QColor m_dim, m_normal;
    DecorationMarks *m_waves;
    QTimer m_waveTimer;
    // The source the waves were last found for.
    Range m_waveWindow{0, 0};
    int m_formatted = 0;
};
