#pragma once

#include "editordecorations.h"
#include "mdtree.h"

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>

// How live formatting draws one run of Markdown source: its weight, slant
// and strike, the colour it takes and the fill behind it. The punctuation of
// every construct stays in the text, as written; it is only drawn as a
// marker. A style is a layer style of the editor's decorations by its index.
struct MarkdownStyle {
    enum Color : quint8 {
        Plain,  // the text's own colour
        Marker, // Markdown punctuation, dimmed: `#`, `**`, `](url)`, fences, list and quote marks
        Faded,  // text that is not part of the reading text, dimmed: comments, front matter, definitions
        Link,   // what a link, an image, a note or a citation shows, in the accent
        Code,   // code spans and code blocks
        Math,   // TeX between dollars
    };
    enum Fill : quint8 { NoFill, CodeFill, MarkFill };

    bool bold = false;
    bool italic = false;
    bool strike = false;
    Color color = Plain;
    Fill fill = NoFill;

    // How many styles there are; index 0 is plain text, which has no run.
    static constexpr int Count = 2 * 2 * 2 * 6 * 3;
    int index() const { return int(bold) + 2 * int(italic) + 4 * int(strike) + 8 * int(color) + 48 * int(fill); }
    static MarkdownStyle at(int index);
    // Text typed at either end of a run of this style goes into it, until
    // the styling is found again: everything but punctuation takes it, so
    // that typing at the end of a heading or a code span goes on in it.
    bool growsAtEdges() const { return color != Marker; }
    bool operator==(const MarkdownStyle &) const = default;
};

// The styled runs of a document's own text (never of the files it includes),
// in order and apart, with every style an index of MarkdownStyle: runs of
// one style that meet are one run, and plain text has none, so that a
// paragraph has a few runs for each construct in it and no more.
std::vector<DecorationSpan> markdownStyleRuns(const md::Document &document);

// Where the parts of a document start that its styling is handed to the view
// in: the start of the line of every block, in order, the first at 0. A part
// runs to the next one's start, so that together they cover the text.
std::vector<int> markdownStyleParts(const md::Document &document);

// Whether a format draws every character of `font` where the plain font
// would: true when its weight or slant keeps every advance. `designMetrics`
// is what the view's text document lays its text out with.
bool keepsAdvances(const QFont &font, bool designMetrics, const QTextCharFormat &format);

// Live formatting for one view: its Markdown source styled as it is written,
// through the view's decorations, in one layer. Punctuation stays in the
// text and is drawn dimmed; nothing changes the text, a font's size or
// spacing, the line height or where the caret and the selection are. Bold
// and italic are drawn only where the editor's font draws them as wide as
// its plain glyphs; otherwise their text takes a colour instead.
//
// The text is styled on a worker, never as it is typed: once the view shows
// it, and again once typing has paused for SettleMs. Each pass reads a copy
// of the text, parses it with the Markdown engine, which has no partial
// parse but reads a book in milliseconds, and hands the styling over in
// parts that tile the text, one for each block: those in view first, at
// once, then the rest. A part whose styling is what the view already has,
// and that no edit has touched since, is not handed over at all, and the
// decorations format again only the paragraphs whose formatting changed. A
// part edited while the pass ran is not taken; the next pass styles it.
// Until then, text typed just inside a styled run, as at the end of a
// heading, takes that run's style at once. Every colour is the theme's,
// given by the view and moved toward the text's colour where it would be too
// pale to read, and a new one recolours the text at once.
class MarkdownStyling : public QObject {
    Q_OBJECT
    Q_PROPERTY(EditorDecorations *decorations READ decorations WRITE setDecorations NOTIFY decorationsChanged)
    // Whether the source is styled; without, it is plain text.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // The editor's font, whose bold and italic are checked when it changes.
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY colorsChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY colorsChanged)
    // Punctuation, comments, front matter and definitions.
    Q_PROPERTY(QColor markerColor READ markerColor WRITE setMarkerColor NOTIFY colorsChanged)
    // What links, images, notes and citations show.
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor codeColor READ codeColor WRITE setCodeColor NOTIFY colorsChanged)
    // Behind code spans.
    Q_PROPERTY(QColor codeFill READ codeFill WRITE setCodeFill NOTIFY colorsChanged)
    Q_PROPERTY(QColor mathColor READ mathColor WRITE setMathColor NOTIFY colorsChanged)
    // The highlighter's colour, which highlighted text has laid thinly over the background.
    Q_PROPERTY(QColor markColor READ markColor WRITE setMarkColor NOTIFY colorsChanged)
    // Whether the font draws bold and italic as wide as plain text, so that they are shown.
    Q_PROPERTY(bool boldShown READ boldShown NOTIFY formatsChanged)
    Q_PROPERTY(bool italicShown READ italicShown NOTIFY formatsChanged)
    // A pass is waiting for typing to pause, or running.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    // Long enough for a burst of keys, as a key held down or a paste of
    // several edits gives, to be styled once; short beside a writer's pause.
    static constexpr int SettleMs = 12;
    // The decoration layer's name.
    static QString layerName() { return QStringLiteral("markdown"); }

    explicit MarkdownStyling(QObject *parent = nullptr);
    ~MarkdownStyling() override;

    EditorDecorations *decorations() const { return m_decorations; }
    void setDecorations(EditorDecorations *decorations);
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    QFont font() const { return m_font; }
    void setFont(const QFont &font);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &color) { setColor(m_foreground, color); }
    QColor background() const { return m_background; }
    void setBackground(const QColor &color) { setColor(m_background, color); }
    QColor markerColor() const { return m_marker; }
    void setMarkerColor(const QColor &color) { setColor(m_marker, color); }
    QColor linkColor() const { return m_link; }
    void setLinkColor(const QColor &color) { setColor(m_link, color); }
    QColor codeColor() const { return m_code; }
    void setCodeColor(const QColor &color) { setColor(m_code, color); }
    QColor codeFill() const { return m_codeFill; }
    void setCodeFill(const QColor &color) { setColor(m_codeFill, color); }
    QColor mathColor() const { return m_math; }
    void setMathColor(const QColor &color) { setColor(m_math, color); }
    QColor markColor() const { return m_mark; }
    void setMarkColor(const QColor &color) { setColor(m_mark, color); }
    bool boldShown() const { return m_boldShown; }
    bool italicShown() const { return m_italicShown; }
    bool busy() const { return m_running || m_settle.isActive(); }

    // The format a style is drawn with, as the colours and the font have it now.
    QTextCharFormat format(const MarkdownStyle &style) const;
    // A theme colour for text, moved toward the text's own colour until it
    // reads against each of the backgrounds as body text should (4.5:1), or
    // as well as the text's own colour does there: a theme's yellow or green
    // is often too pale over a light background.
    static QColor readable(const QColor &color, const QColor &foreground, const QList<QColor> &backgrounds);

    // What the passes have done, for measuring.
    struct Stats {
        int passes = 0;
        // Parts handed to the view, and parts found as the view had them.
        int partsSent = 0;
        int partsKept = 0;
        // How long each handing over took on the view's thread.
        std::vector<qint64> applyNanoseconds;
    };
    const Stats &stats() const { return m_stats; }
    void resetStats() { m_stats = {}; }

    // A part of the text and its styling as a pass found it.
    struct Part {
        SourceRange range;
        std::vector<DecorationSpan> runs;
        size_t key = 0;
    };
    // A part the view has the styling of, where it is now.
    struct Applied {
        int start = 0;
        int end = 0;
        size_t key = 0;
    };

signals:
    void decorationsChanged();
    void enabledChanged();
    void fontChanged();
    void colorsChanged();
    void formatsChanged();
    void busyChanged();
    // The parts in view were styled as the text is at `revision`, the
    // newest there is.
    void visibleStyled(Revision revision);

private:
    struct Pass;
    static void run(Pass &pass);

    bool active() const;
    void connectBinding();
    // Takes a colour, and has the layer's formats made again once all have come.
    void setColor(QColor &member, const QColor &color);
    void defineLayer();
    void measureFont();
    void refresh();
    void schedule(int delayMs);
    void start();
    void cancel();
    void edited(Revision revision, SourceRange range, int inserted);
    void take(quint64 pass, Revision revision, std::vector<Part> parts, bool visible, bool last, int kept);
    void remember(SourceRange range, size_t key);
    void setRunning(bool running);

    QPointer<EditorDecorations> m_decorations;
    QPointer<EditorBinding> m_binding;
    QList<QMetaObject::Connection> m_decorationConnections, m_bindingConnections;
    bool m_enabled = true;
    QFont m_font;
    QColor m_foreground, m_background, m_marker, m_link, m_code, m_codeFill, m_math, m_mark;
    bool m_boldShown = true;
    bool m_italicShown = true;
    QTimer m_settle;
    QTimer m_reformat;
    quint64 m_pass = 0;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    bool m_running = false;
    bool m_wasBusy = false;
    // A part was not taken, since it was edited meanwhile.
    bool m_stale = false;
    // The parts whose styling the view has, in order and apart, moved with
    // every edit and forgotten when one touches them.
    std::vector<Applied> m_applied;
    Stats m_stats;
};
